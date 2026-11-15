// PS5 host threads using the title's pthread implementation.
// Affinity: the console's pthread_getaffinity_np and pthread_setaffinity_np take sets of 8 or 16 bytes and refuse
// FreeBSD's 32-byte cpuset_t with ERANGE (PS5_PayloadSDK platform/docs/PROBE.md, 2026-09-28): run 16 logged mask=0 for
// every thread and MKW_REPLAY_CORE never applied. A 16-byte set is used; a title's threads run on CPUs 0-12 (0x1fff).
// CPU time: pthread_getcpuclockid answers with the caller's clock (run 16: every thread the same ms per frame), so a
// thread's time is read on the thread itself through a signal (sigsample.c).
// Priorities: recorded per thread and applied only while plat_thread_priority_mode is on (MKW_THREAD_PRIO, perf.c);
// off, every runtime thread keeps the process's priority, as before task 19.
#define _GNU_SOURCE
#include "platform.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include <pthread_np.h>
#include <sys/cpuset.h>
typedef cpuset_t CpuSet;
#else
typedef cpu_set_t CpuSet;
#endif

struct PlatThread { pthread_t thread; };

typedef struct Start {
    PlatThreadFn fn;
    void *arg;
    pthread_t thread;
    uint32_t id;
    int want;   // the PLAT_PRIO_* level the thread asked for, PRIO_UNSET none
    char name[64];
    struct Start *next;
} Start;

#define PRIO_UNSET (-1000)

static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static Start *live;
static uint64_t default_mask;
static pthread_t main_pthread;
static uint32_t main_tid;
static int base_prio = -1;
static int main_want = PRIO_UNSET;
static int prio_on;                       // plat_thread_priority_mode
static _Thread_local Start *self_start;   // the calling thread's record, NULL on the main thread

__attribute__((constructor)) static void ps5_threads_init(void) {
    main_pthread = pthread_self();
    main_tid = plat_thread_id();
#ifdef __PROSPERO__
    int p = pthread_getprio(main_pthread);
    base_prio = (p > 0 && p < 768) ? p : 700;
#else
    base_prio = 700;
#endif
}

uint32_t plat_thread_id(void) {
#ifdef __PROSPERO__
    return (uint32_t)pthread_getthreadid_np();
#else
    return (uint32_t)gettid();
#endif
}

static void *start_thread(void *arg) {
    Start *start = arg;
    start->thread = pthread_self();
    start->id = plat_thread_id();
    if (start->name[0]) {
#ifdef __PROSPERO__
        pthread_set_name_np(start->thread, start->name);
#else
        char name[16];
        snprintf(name, sizeof name, "%.15s", start->name);
        pthread_setname_np(start->thread, name);
#endif
    }
    uint64_t dmask = __atomic_load_n(&default_mask, __ATOMIC_ACQUIRE);
    if (dmask) plat_thread_affinity(NULL, dmask);
    start->want = PRIO_UNSET;
    self_start = start;
    pthread_mutex_lock(&live_lock);
    start->next = live;
    live = start;
    pthread_mutex_unlock(&live_lock);
    // Give every runtime thread its own signal stack so stack overflows and guest faults run safely on SA_ONSTACK.
    enum { ALT_STACK_BYTES = 65536 };
    void *alt = malloc(ALT_STACK_BYTES);
    if (alt) {
        stack_t ss = {.ss_sp = alt, .ss_size = ALT_STACK_BYTES};
        if (sigaltstack(&ss, NULL) != 0) {
            free(alt);
            alt = NULL;
        }
    }
    plat_sample_add_self(start->name);   // after the signal stack: the sampler's SIGPROF runs on it
    start->fn(start->arg);
    plat_sample_remove_self();
    if (alt) {
        stack_t off = {.ss_flags = SS_DISABLE};
        sigaltstack(&off, NULL);
        free(alt);
    }
    pthread_mutex_lock(&live_lock);
    Start **slot = &live;
    while (*slot != start) slot = &(*slot)->next;
    *slot = start->next;
    pthread_mutex_unlock(&live_lock);
    free(start);
    return NULL;
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    if (!fn) return NULL;
    PlatThread *handle = malloc(sizeof *handle);
    Start *start = calloc(1, sizeof *start);
    pthread_attr_t attr;
    if (handle && start && !pthread_attr_init(&attr)) {
        start->fn = fn;
        start->arg = arg;
        if (name) snprintf(start->name, sizeof start->name, "%s", name);
        int result = pthread_attr_setstacksize(&attr, stack ? stack : (1u << 20));
        if (!result) result = pthread_create(&handle->thread, &attr, start_thread, start);
        pthread_attr_destroy(&attr);
        if (!result) return handle;
        fprintf(stderr, "plat_thread_new_ex(%s, stack=%zu) failed: %d (%s)\n",
                name ? name : "?", stack, result, strerror(result));
    }
    free(start);
    free(handle);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

void plat_thread_join(PlatThread *t) {
    if (pthread_join(t->thread, NULL)) abort();
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) abort();
    deadline.tv_sec += ns / 1000000000u;
    deadline.tv_nsec += ns % 1000000000u;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    int result = pthread_timedjoin_np(t->thread, NULL, &deadline);
    if (result == ETIMEDOUT) return 0;
    if (result) abort();
    free(t);
    return 1;
}

void plat_thread_detach(PlatThread *t) {
    if (pthread_detach(t->thread)) abort();
    free(t);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (!t) return 0;
    plat_thread_detach(t);
    return 1;
}

#ifdef __PROSPERO__
// 16 bytes: the largest set the console takes (see the top). CPU n is bit n of the first word.
typedef struct { uint64_t w[2]; } AffSet;
static uint64_t thread_mask(pthread_t thread) {
    AffSet set = {{0, 0}};
    if (pthread_getaffinity_np(thread, sizeof set, (cpuset_t *)&set)) return 0;
    return set.w[0];
}
static int set_pthread_affinity(pthread_t thread, uint64_t mask) {
    AffSet set = {{mask, 0}};
    return pthread_setaffinity_np(thread, sizeof set, (const cpuset_t *)&set) == 0;
}
#else
static uint64_t thread_mask(pthread_t thread) {
    CpuSet set;
    if (pthread_getaffinity_np(thread, sizeof set, &set)) return 0;
    uint64_t mask = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (CPU_ISSET(i, &set)) mask |= 1ull << i;
    return mask;
}
static int set_pthread_affinity(pthread_t thread, uint64_t mask) {
    CpuSet set;
    CPU_ZERO(&set);
    for (unsigned i = 0; i < 64; ++i)
        if (mask & (1ull << i)) CPU_SET(i, &set);
    return pthread_setaffinity_np(thread, sizeof set, &set) == 0;
}
#endif

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    pthread_t thread = t ? t->thread : pthread_self();
    uint64_t previous = thread_mask(thread);
    if (!previous || !mask) return 0;
    return set_pthread_affinity(thread, mask) ? previous : 0;
}

uint32_t plat_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 && count <= UINT32_MAX ? (uint32_t)count : 0;
}

void plat_yield(void) { sched_yield(); }

uint64_t plat_self_cpu_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value)) return 0;
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

// Read on the thread itself (sigsample.c): see the top.
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    if (tid == plat_thread_id()) return plat_self_cpu_ns();
    return plat_sample_cpu_tid(tid);
}

int64_t plat_threads_cpu_ns_named(const char *name) { return plat_sample_cpu_named(name); }

int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    uint64_t mask = thread_mask(pthread_self());
    if (!mask) {
        if (all) *all = 0;
        if (first) *first = 0;
        return 0;
    }
    if (all) *all = mask;
    // PS5 Zen 2 pairs SMT siblings on adjacent logical processors (2k, 2k+1).
    uint64_t f = 0;
    int cores = 0;
    for (int k = 0; k < 32; k++) {
        uint64_t pair = mask & (3ull << (k * 2));
        if (!pair) continue;
        cores++;
        f |= (pair & (1ull << (k * 2))) ? (1ull << (k * 2)) : (1ull << (k * 2 + 1));
    }
    if (first) *first = f;
    return cores;
}

int plat_process_affinity(uint64_t mask) {
    if (!mask) return 0;
    __atomic_store_n(&default_mask, mask, __ATOMIC_RELEASE);
    int ok = 1;
    if (main_pthread) ok &= set_pthread_affinity(main_pthread, mask);
    pthread_mutex_lock(&live_lock);
    for (const Start *thread = live; thread; thread = thread->next)
        if (thread->thread) ok &= set_pthread_affinity(thread->thread, mask);
    pthread_mutex_unlock(&live_lock);
    return ok;
}

static int ps5_prio_of(int prio) {
    int b = base_prio > 0 ? base_prio : 700;
    int p = prio >= PLAT_PRIO_TIME_CRITICAL ? b - 250
          : prio >= PLAT_PRIO_HIGHEST       ? b - 180
          : prio >= PLAT_PRIO_ABOVE_NORMAL  ? b - 90
          : prio >= PLAT_PRIO_NORMAL        ? b
          : prio >= PLAT_PRIO_BELOW_NORMAL  ? b + 30
          : prio >= PLAT_PRIO_LOWEST        ? b + 50
          : b + 60;
    if (p < 256) p = 256;
    if (p > 767) p = 767;
    return p;
}

// want: a PLAT_PRIO_* level, or PRIO_UNSET for the process's own priority.
static void set_pthread_prio(pthread_t thread, int want) {
#ifdef __PROSPERO__
    int on = __atomic_load_n(&prio_on, __ATOMIC_RELAXED);
    pthread_setprio(thread, on && want != PRIO_UNSET ? ps5_prio_of(want) : (base_prio > 0 ? base_prio : 700));
#else
    (void)thread; (void)want;
#endif
}

void plat_thread_priority(int prio) {
    pthread_mutex_lock(&live_lock);
    if (self_start) self_start->want = prio;
    else if (pthread_equal(pthread_self(), main_pthread)) main_want = prio;
    pthread_mutex_unlock(&live_lock);
    set_pthread_prio(pthread_self(), prio);
}

void plat_thread_set_priority(PlatThread *t, int prio) {
    if (!t) return;
    pthread_mutex_lock(&live_lock);
    for (Start *s = live; s; s = s->next)
        if (pthread_equal(s->thread, t->thread)) s->want = prio;
    pthread_mutex_unlock(&live_lock);
    set_pthread_prio(t->thread, prio);
}

// The level the thread asked for (what it runs at while the priorities are on).
int plat_thread_priority_get(void) {
    int want = self_start ? self_start->want : pthread_equal(pthread_self(), main_pthread) ? main_want : PRIO_UNSET;
    return want == PRIO_UNSET ? PLAT_PRIO_NORMAL : want;
}

int plat_thread_priority_mode(int on) {
    pthread_mutex_lock(&live_lock);
    __atomic_store_n(&prio_on, on != 0, __ATOMIC_RELAXED);
    if (main_pthread) set_pthread_prio(main_pthread, main_want);
    for (const Start *s = live; s; s = s->next)
        if (s->thread) set_pthread_prio(s->thread, s->want);
    pthread_mutex_unlock(&live_lock);
    return on != 0;
}

void plat_threads_log(void (*emit)(const char *line)) {
    if (!emit) return;
    char buf[160];
    if (main_pthread) {
        uint64_t m = thread_mask(main_pthread);
#ifdef __PROSPERO__
        int p = pthread_getprio(main_pthread);
#else
        int p = 0;
#endif
        snprintf(buf, sizeof buf, "tid=%u name=main mask=%#llx os_prio=%d",
                 main_tid, (unsigned long long)m, p);
        emit(buf);
    }
    pthread_mutex_lock(&live_lock);
    for (const Start *t = live; t; t = t->next) {
        uint64_t m = t->thread ? thread_mask(t->thread) : 0;
#ifdef __PROSPERO__
        int p = t->thread ? pthread_getprio(t->thread) : 0;
#else
        int p = 0;
#endif
        snprintf(buf, sizeof buf, "tid=%u name=%s mask=%#llx os_prio=%d asked=%d",
                 t->id, t->name[0] ? t->name : "?", (unsigned long long)m, p, t->want == PRIO_UNSET ? 0 : t->want);
        emit(buf);
    }
    pthread_mutex_unlock(&live_lock);
}

PlatIntr *plat_intr_self(void) { return NULL; }
void plat_intr_free(PlatIntr *t) { (void)t; }

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    (void)t; (void)rip; (void)rsp;
    return 0;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    (void)t; (void)to;
    return 0;
}
