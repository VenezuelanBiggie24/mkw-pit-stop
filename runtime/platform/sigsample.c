// Per-thread sampling by signal on POSIX systems (Linux, PS5): a registry of the runtime's threads, each one's CPU
// time read on the thread itself, and the sampler thread of the profiler (core/diag/prof_posix.c).
//
// Why a signal: the PS5's pthread_getcpuclockid answers every thread with the caller's clock (run 16, 14/11/2026: the
// `sched: host threads` line gave 0.69 ms/f for every thread), and its ITIMER_PROF signals the process, not the
// threads that run (12 samples in 600 frames). A thread's own CLOCK_THREAD_CPUTIME_ID is right on both systems, so
// another thread sends SIGPROF and the handler, on the target thread, stores what it reads (plat_sample_cpu_ns).
// The same handler takes the profiler's samples: the sampler thread sends SIGPROF to each registered thread every
// period (pthread_kill), and the handler hands the interrupted instruction pointer to the hook with the thread's CPU
// time since its previous sample, so time spent blocked weighs nothing (t20-linux3: unweighted, futex waits were 97%
// of every thread's samples).
//
// The registry holds the threads plat_thread_new starts (linux.c, ps5_threads.c) and the main thread; driver threads
// are not in it. The handler is async-signal-safe: it reads its slot through a thread-local index, a clock, and calls
// the hook, which must be async-signal-safe too. A thread leaves the registry (plat_sample_remove_self) before it ends,
// under the lock the sender holds, so no signal goes to a thread that has ended.
// A signal interrupts a blocking call: the runtime's waits are pthread condition variables and mutexes (a spurious
// wake-up at most) and nanosleep loops that resume on EINTR.
#if defined(__linux__) || defined(__PROSPERO__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>

#include "platform.h"

#define SLOTS 512
#define STACK_N 28

typedef struct {
    pthread_t th;
    uint32_t tid;
    volatile int used;          // 1 while the thread is registered
    char name[32];
    uintptr_t stack_hi;         // above every frame the thread runs after registering (the stack scan's limit)
    volatile uint32_t req, ack; // CPU-time handshake: the reader bumps req, the handler stores cpu and sets ack = req
    volatile uint64_t cpu;      // ns, the thread's CPU time at its last ack
    uint64_t last_sample;       // ns, its CPU time at its last profiler sample (the handler's only)
    volatile uint32_t bt_req, bt_ack;   // stack handshake (plat_sample_stacks), as req/ack
    void *bt[STACK_N];          // the interrupted rip, then backtrace()'s frames
    int nbt;
} Slot;

static Slot slots[SLOTS];
static int nslots;                       // high-water mark
static pthread_mutex_t reg = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int my_slot = -1;   // the calling thread's slot, -1 none
static volatile int installed;
static PlatSampleHook volatile hook;     // the profiler's, NULL while it is off
static volatile uint32_t period_us;
static volatile int sampler_on;

static uint64_t self_cpu(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t ctx_rip(void *ctx) {
    ucontext_t *uc = ctx;
#ifdef __PROSPERO__
    return (uint64_t)uc->uc_mcontext.mc_rip;
#elif defined(__aarch64__)   // Linux AArch64: the program counter
    return (uint64_t)uc->uc_mcontext.pc;
#else
    return (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
}

static uint64_t ctx_caller(void *ctx, uint64_t rip) {
    uint64_t base = plat_exe_base();
    uint64_t exe_hi = base + (1ull << 29);
    if (!base || (rip >= base && rip < exe_hi)) return 0;
    ucontext_t *uc = ctx;
#ifdef __PROSPERO__
    uint64_t rsp = (uint64_t)uc->uc_mcontext.mc_rsp;
    uint64_t rbp = (uint64_t)uc->uc_mcontext.mc_rbp;
#elif defined(__aarch64__)   // Linux AArch64: SP and the frame pointer X29, whose records are laid out as RBP's
    uint64_t rsp = (uint64_t)uc->uc_mcontext.sp;
    uint64_t rbp = (uint64_t)uc->uc_mcontext.regs[29];
#else
    uint64_t rsp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
    uint64_t rbp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
#endif
    uint64_t fp = rbp;
    for (int i = 0; i < 8 && fp >= rsp && fp < rsp + 0x10000 && !(fp & 7); i++) {
        uint64_t next_fp = *(const uint64_t *)(uintptr_t)fp;
        uint64_t ret = *(const uint64_t *)(uintptr_t)(fp + 8);
        if (ret >= base && ret < exe_hi) return ret;
        if (next_fp <= fp) break;
        fp = next_fp;
    }
    if (rsp && !(rsp & 7)) {
        uint64_t page_end = (rsp + 0x4000) & ~0x3fffull;
        uint64_t lim = rsp + 512 < page_end ? rsp + 512 : page_end;
        for (uint64_t p = rsp; p + 8 <= lim; p += 8) {
            uint64_t ret = *(const uint64_t *)(uintptr_t)p;
            if (ret >= base && ret < exe_hi) return ret;
        }
    }
    return 0;
}

static void on_signal(int sig, siginfo_t *si, void *ctx) {
    (void)sig, (void)si;
    int s = my_slot;
    if (s < 0) return;
    int saved = errno;
    Slot *e = &slots[s];
    uint32_t r = __atomic_load_n(&e->req, __ATOMIC_ACQUIRE);
    if (r != e->ack) {
        e->cpu = self_cpu();
        __atomic_store_n(&e->ack, r, __ATOMIC_RELEASE);
    }
    uint32_t b = __atomic_load_n(&e->bt_req, __ATOMIC_ACQUIRE);
    if (b != e->bt_ack) {   // plat_sample_stacks: where this thread is (backtrace was primed at install)
        e->bt[0] = ctx ? (void *)(uintptr_t)ctx_rip(ctx) : NULL;
#ifdef __PROSPERO__
        // backtrace stops at the PS5's signal trampoline (every hang showed only libkernel's wait, 0x80000038c): the
        // values on the interrupted stack that point into the executable instead, the callers among them (and some
        // stale ones). Frames 1-3 stay empty: the hang watch prints callers from frame 4 (glibc's layout).
        int n = 4;
        e->bt[1] = e->bt[2] = e->bt[3] = NULL;
        uint64_t base = plat_exe_base(), exe_hi = base + (1ull << 29);
        uint64_t rsp = ctx ? (uint64_t)((ucontext_t *)ctx)->uc_mcontext.mc_rsp : 0;
        if (base && rsp && !(rsp & 7) && rsp < e->stack_hi && e->stack_hi - rsp < (256u << 10))   // not a fiber's stack
            for (uint64_t p = rsp; p + 8 <= e->stack_hi && p < rsp + 0x8000 && n < STACK_N; p += 8) {
                uint64_t v = *(const uint64_t *)(uintptr_t)p;
                if (v >= base && v < exe_hi) e->bt[n++] = (void *)(uintptr_t)v;
            }
        e->nbt = n;
#else
        int n = backtrace(e->bt + 1, STACK_N - 1);
        e->nbt = 1 + (n > 0 ? n : 0);
#endif
        __atomic_store_n(&e->bt_ack, b, __ATOMIC_RELEASE);
    }
    PlatSampleHook h = __atomic_load_n(&hook, __ATOMIC_ACQUIRE);
    if (h && ctx) {   // weighted by the CPU time since the previous sample: a thread blocked in a wait weighs ~0
        uint64_t c = self_cpu(), d = e->last_sample && c > e->last_sample ? c - e->last_sample : 0;
        e->last_sample = c;
        uint64_t rip = ctx_rip(ctx);
        h(e->tid, s, rip, ctx_caller(ctx, rip), d);
    }
    errno = saved;
}

static int install(void) {
    if (__atomic_load_n(&installed, __ATOMIC_ACQUIRE)) return 1;
    void *prime[2];
    backtrace(prime, 2);   // glibc loads its unwinder on the first call (malloc): not in the handler
    pthread_mutex_lock(&reg);
    if (!installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        if (!sigaction(SIGPROF, &sa, NULL)) __atomic_store_n(&installed, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&reg);
    return installed;
}

void plat_sample_add_self(const char *name) {
    if (my_slot >= 0) return;
    pthread_mutex_lock(&reg);
    for (int i = 0; i < SLOTS; i++)
        if (!slots[i].used) {
            Slot *e = &slots[i];
            e->th = pthread_self();
            e->tid = plat_thread_id();
            // This frame's saved rbp and return address are the highest bytes known to be mapped: a thread registers
            // in its first frame, and +0x400 read past its stack's top (a crash at the first hang report, 15/11/2026).
            e->stack_hi = (uintptr_t)__builtin_frame_address(0) + 16;
            snprintf(e->name, sizeof e->name, "%s", name && *name ? name : "?");
            e->ack = e->req;
            e->cpu = 0;
            e->last_sample = 0;
            __atomic_store_n(&e->used, 1, __ATOMIC_RELEASE);
            if (i >= nslots) nslots = i + 1;
            my_slot = i;
            break;
        }
    pthread_mutex_unlock(&reg);
}

void plat_sample_remove_self(void) {
    int s = my_slot;
    if (s < 0) return;
    pthread_mutex_lock(&reg);
    my_slot = -1;   // a signal already on its way finds no slot
    __atomic_store_n(&slots[s].used, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&reg);
}

int plat_sample_self_name(char *out, size_t cap) { return plat_sample_slot_name(my_slot, out, cap); }

int plat_sample_slot_name(int slot, char *out, size_t cap) {
    if (slot < 0 || slot >= SLOTS) return 0;
    snprintf(out, cap, "%s", slots[slot].name);
    return 1;
}

// Asks every registered thread whose name matches (NULL: all) for its CPU time and waits up to 20 ms for the
// answers. A thread that does not answer in time keeps its previous value.
static void refresh(const char *name, uint32_t tid) {
    if (!install()) return;
    uint32_t want[SLOTS];
    int n = 0;
    pthread_mutex_lock(&reg);
    for (int i = 0; i < nslots; i++) {
        Slot *e = &slots[i];
        want[i] = 0;
        if (!e->used || (name && strcmp(e->name, name)) || (tid && e->tid != tid)) continue;
        if (i == my_slot) {   // the caller itself: no signal needed
            e->cpu = self_cpu();
            continue;
        }
        want[i] = __atomic_add_fetch(&e->req, 1, __ATOMIC_ACQ_REL);
        if (pthread_kill(e->th, SIGPROF)) want[i] = 0;
        else n++;
    }
    pthread_mutex_unlock(&reg);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (n) {
        n = 0;
        for (int i = 0; i < nslots; i++)
            if (want[i] && __atomic_load_n(&slots[i].ack, __ATOMIC_ACQUIRE) != want[i]) n++;
        if (!n) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) * 1000000000ll + (now.tv_nsec - t0.tv_nsec) > 20000000ll) break;
        sched_yield();
    }
}

int64_t plat_sample_cpu_named(const char *name) {
    refresh(name, 0);
    int64_t total = -1;
    pthread_mutex_lock(&reg);
    for (int i = 0; i < nslots; i++)
        if (slots[i].used && !strcmp(slots[i].name, name)) total = (total < 0 ? 0 : total) + (int64_t)slots[i].cpu;
    pthread_mutex_unlock(&reg);
    return total;
}

uint64_t plat_sample_cpu_tid(uint32_t tid) {
    if (!tid) return 0;
    refresh(NULL, tid);
    uint64_t ns = 0;
    pthread_mutex_lock(&reg);
    for (int i = 0; i < nslots; i++)
        if (slots[i].used && slots[i].tid == tid) ns = slots[i].cpu;
    pthread_mutex_unlock(&reg);
    return ns;
}

// plat_sample_filter: the sampler signals only the threads of that name (empty: all). The PS5 title hung with the
// replay "in a submission" whenever every thread was sampled (15/11/2026: twice in the race's intro movie, once 550
// frames into the race); the game's own threads ("guest", the render thread among them) are what the frame needs.
static char only_name[32];
void plat_sample_filter(const char *name) { snprintf(only_name, sizeof only_name, "%s", name ? name : ""); }

// The sampler: SIGPROF to every registered thread each period. Its own thread is registered too (for the CPU-time
// line) but never signalled by itself.
static void sampler_main(void *arg) {
    (void)arg;
    while (__atomic_load_n(&sampler_on, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&reg);
        for (int i = 0; i < nslots; i++)
            if (slots[i].used && i != my_slot && (!only_name[0] || !strcmp(slots[i].name, only_name)))
                pthread_kill(slots[i].th, SIGPROF);
        pthread_mutex_unlock(&reg);
        struct timespec ts = {0, (long)period_us * 1000};
        while (nanosleep(&ts, &ts) && errno == EINTR) {}
    }
}

int plat_sample_start(uint32_t us, PlatSampleHook h) {
    if (!h || !us || !install()) return 0;
    period_us = us;
    __atomic_store_n(&hook, h, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&sampler_on, 1, __ATOMIC_ACQ_REL)) return 1;
    if (!plat_thread_spawn(sampler_main, NULL, "prof sampler", 256 << 10)) {
        __atomic_store_n(&sampler_on, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&hook, NULL, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

void plat_sample_stop(void) {
    __atomic_store_n(&sampler_on, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hook, NULL, __ATOMIC_RELEASE);
}

void plat_sample_hold(int on) {
    if (!__atomic_load_n(&installed, __ATOMIC_ACQUIRE)) return;   // no handler yet: nothing sends the signal
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGPROF);
    pthread_sigmask(on ? SIG_BLOCK : SIG_UNBLOCK, &s, NULL);
}

// Every registered thread's stack, read on the thread through SIGPROF (as the CPU time): for the hang watch, so a hang
// names where each thread waits (run 25 and t26-peek1 hung with the replay "in a submission" and no lock named).
// emit gets the interrupted rip first, then the handler's own frames, the signal trampoline and the rest of the stack.
int plat_sample_stacks(void (*emit)(const char *name, uint32_t tid, void *const *frames, int n, void *ctx), void *ctx) {
    if (!install()) return 0;
    uint32_t want[SLOTS];
    pthread_mutex_lock(&reg);
    for (int i = 0; i < nslots; i++) {
        want[i] = 0;
        if (!slots[i].used || i == my_slot) continue;
        want[i] = __atomic_add_fetch(&slots[i].bt_req, 1, __ATOMIC_ACQ_REL);
        if (pthread_kill(slots[i].th, SIGPROF)) want[i] = 0;
    }
    pthread_mutex_unlock(&reg);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {   // 200 ms at most for the answers
        int left = 0;
        for (int i = 0; i < nslots; i++)
            if (want[i] && __atomic_load_n(&slots[i].bt_ack, __ATOMIC_ACQUIRE) != want[i]) left++;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!left || (now.tv_sec - t0.tv_sec) * 1000000000ll + (now.tv_nsec - t0.tv_nsec) > 200000000ll) break;
        sched_yield();
    }
    int n = 0;
    for (int i = 0; i < nslots; i++)
        if (want[i] && __atomic_load_n(&slots[i].bt_ack, __ATOMIC_ACQUIRE) == want[i]) {
            emit(slots[i].name, slots[i].tid, slots[i].bt, slots[i].nbt, ctx);
            n++;
        }
    return n;
}

// The main thread joins at start, before main() (its signal stack is the platform's business).
__attribute__((constructor)) static void sample_main_thread(void) { plat_sample_add_self("main"); }
#endif
