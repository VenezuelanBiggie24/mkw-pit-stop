// PS5 platform memory with demand-backed 64 KiB direct-memory units.
// Guest commits record RW page state in the 2-level chunk directory without
// allocating physical DMEM; first access (SIGSEGV/SIGBUS) or explicit host
// kernel-write / GPU-import backing (plat_mem_back) allocates, maps, zeroes,
// and applies the unit's mixed native protections.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform.h"
#include "prot_watch.h"
#include "ps5_guest_memory.h"
#include "ps5_mixed.h"

#include <ps5platform/kernel.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

enum {
    PAGE = PS5_GUEST_PAGE,
    CHUNK_SHIFT = 24, // 16 MiB per chunk = 256 x 64 KiB Ps5MixedBlock units
    CHUNK_SIZE = 1u << CHUNK_SHIFT,
    CHUNK_UNITS = CHUNK_SIZE / PS5_MEM_UNIT,
    ALT_STACK_BYTES = 65536,
    LOG_EVERY_UNITS = 4096, // Log backed totals every 256 MiB of physical growth.
};

_Static_assert(PS5_GUEST_NONE + PLAT_MEM_RESERVED == PS5_GUEST_RESERVED &&
               PS5_GUEST_NONE + PLAT_PROT_READ == PS5_GUEST_READ &&
               PS5_GUEST_NONE + PLAT_PROT_RW == PS5_GUEST_RW, "guest/platform page states differ");
static const uint64_t address_limit = 1ull << 39;

typedef enum { RANGE_OTHER = 0, RANGE_HEAP = 1, RANGE_ALIAS = 2 } RangeKind;

typedef struct VmRange {
    uint64_t base, end;
    // Fixed-size directory of 16 MiB chunks, allocated once on reserve and
    // populated lazily on commit. Never reallocated or freed before shutdown,
    // so fault_handler and plat_mem_back can walk it without taking vm_lock.
    Ps5MixedBlock *volatile *chunks;
    size_t chunk_count;
    bool host; // Native anonymous allocation, with no guest subpages.
    RangeKind kind;
    struct VmRange *next;
} VmRange;

static VmRange *ranges;
static VmRange *cleanup_pending;
static bool stopped;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t backed_units[3];
static size_t backed_by_cause[PLAT_BACK_N];
static size_t next_log_units = LOG_EVERY_UNITS;
static bool oom_logged;
static void (*g_vk_census)(char *out, size_t cap, int in_signal);

int sceKernelGetPageTableStats(int *cpu_total, int *cpu_free, int *gpu_total, int *gpu_free);

void ps5_mem_set_vk_census(void (*fn)(char *out, size_t cap, int in_signal)) {
    __atomic_store_n(&g_vk_census, fn, __ATOMIC_RELEASE);
}

static int (*g_fault_hook)(uint64_t addr);
static int (*g_watch_hook)(uint64_t addr, uint64_t rip);
static void (*g_crash_hook)(const PlatCrash *c);
static void (*g_early_hold_hook)(uint64_t addr, size_t n);
static int (*g_frozen_hook)(uint64_t addr);

_Thread_local int plat_fault_write;
_Thread_local uint64_t plat_fault_pc, plat_fault_ret;

static struct sigaction prev_sigsegv, prev_sigbus, prev_sigill, prev_sigfpe;
static pthread_once_t fault_once = PTHREAD_ONCE_INIT;

static inline void unit_lock(Ps5MixedBlock *u) {
    while (__atomic_test_and_set(&u->lock, __ATOMIC_ACQUIRE)) {}
}

static inline void unit_unlock(Ps5MixedBlock *u) {
    __atomic_clear(&u->lock, __ATOMIC_RELEASE);
}

static bool sdk_ok(const char *operation, uint64_t address, uint64_t bytes, int result) {
    if (result)
        fprintf(stderr, "PS5 memory %s %#llx+%#llx failed (%#x)\n", operation, (unsigned long long)address,
                (unsigned long long)bytes, (unsigned)result);
    return !result;
}

static void log_backed_totals(const char *call, uint64_t address, int err, int in_signal) {
    if (err && __atomic_exchange_n(&oom_logged, true, __ATOMIC_ACQ_REL)) return;
    int64_t phys = 0;
    size_t avail = 0, flex_avail = 0, flex_cfg = 0;
    int64_t dmem_total = sceKernelGetDirectMemorySize();
    int avail_rc = sceKernelAvailableDirectMemorySize(0, INT64_MAX, PS5_MEM_UNIT, &phys, &avail);
    int flex_a_rc = sceKernelAvailableFlexibleMemorySize(&flex_avail);
    int flex_c_rc = sceKernelConfiguredFlexibleMemorySize(&flex_cfg);
    size_t heap_mib = (__atomic_load_n(&backed_units[RANGE_HEAP], __ATOMIC_RELAXED) * PS5_MEM_UNIT) >> 20;
    size_t alias_mib = (__atomic_load_n(&backed_units[RANGE_ALIAS], __ATOMIC_RELAXED) * PS5_MEM_UNIT) >> 20;
    size_t other_mib = (__atomic_load_n(&backed_units[RANGE_OTHER], __ATOMIC_RELAXED) * PS5_MEM_UNIT) >> 20;
    size_t total_mib = heap_mib + alias_mib + other_mib;
    size_t cw_mib = (__atomic_load_n(&backed_by_cause[PLAT_BACK_WRITE_FAULT], __ATOMIC_RELAXED) * PS5_MEM_UNIT) >> 20;
    size_t cr_mib = (__atomic_load_n(&backed_by_cause[PLAT_BACK_READ_FAULT], __ATOMIC_RELAXED) * PS5_MEM_UNIT) >> 20;
    size_t crr_mib = (__atomic_load_n(&backed_by_cause[PLAT_BACK_RT_READ], __ATOMIC_RELAXED) * PS5_MEM_UNIT) >> 20;
    size_t ci_mib = (__atomic_load_n(&backed_by_cause[PLAT_BACK_IMPORT], __ATOMIC_RELAXED) * PS5_MEM_UNIT) >> 20;
    char vk_buf[256] = "vk=none";
    void (*vk_fn)(char *, size_t, int) = __atomic_load_n(&g_vk_census, __ATOMIC_ACQUIRE);
    if (vk_fn) vk_fn(vk_buf, sizeof vk_buf, in_signal);
    int pt_cpu_tot = 0, pt_cpu_free = 0, pt_gpu_tot = 0, pt_gpu_free = 0;
    int pt_rc = sceKernelGetPageTableStats(&pt_cpu_tot, &pt_cpu_free, &pt_gpu_tot, &pt_gpu_free);
    char pt_buf[64];
    if (pt_rc == 0) snprintf(pt_buf, sizeof pt_buf, "pt=cpu:%d/%d gpu:%d/%d", pt_cpu_free, pt_cpu_tot, pt_gpu_free, pt_gpu_tot);
    else snprintf(pt_buf, sizeof pt_buf, "pt_rc=%#x", (unsigned)pt_rc);
    if (err) {
        fprintf(stderr,
                "PS5 memory back %#llx failed in %s (%#x): backed=%zu MiB (heap=%zu, alias=%zu, other=%zu), "
                "causes(w=%zu,rf=%zu,rr=%zu,imp=%zu MiB), "
                "dmem_avail=%zu/%lld MiB (rc=%#x), flex=%zu/%zu MiB (rc=%#x/%#x), %s, %s\n",
                (unsigned long long)address, call, (unsigned)err, total_mib, heap_mib, alias_mib, other_mib,
                cw_mib, cr_mib, crr_mib, ci_mib,
                avail_rc == 0 ? (avail >> 20) : 0, dmem_total > 0 ? (long long)(dmem_total >> 20) : 0ll,
                (unsigned)avail_rc, flex_a_rc == 0 ? (flex_avail >> 20) : 0,
                flex_c_rc == 0 ? (flex_cfg >> 20) : 0, (unsigned)flex_a_rc, (unsigned)flex_c_rc, pt_buf, vk_buf);
    } else {
        fprintf(stderr,
                "PS5 memory %s: backed=%zu MiB (heap=%zu, alias=%zu, other=%zu), causes(w=%zu,rf=%zu,rr=%zu,imp=%zu MiB), "
                "dmem_avail=%zu/%lld MiB, flex=%zu/%zu MiB, %s, %s\n",
                call, total_mib, heap_mib, alias_mib, other_mib, cw_mib, cr_mib, crr_mib, ci_mib,
                avail_rc == 0 ? (avail >> 20) : 0,
                dmem_total > 0 ? (long long)(dmem_total >> 20) : 0ll,
                flex_a_rc == 0 ? (flex_avail >> 20) : 0, flex_c_rc == 0 ? (flex_cfg >> 20) : 0, pt_buf, vk_buf);
    }
}

void ps5_mem_log_census(const char *tag) { log_backed_totals(tag ? tag : "census", 0, 0, 0); }

void plat_mem_census_log(const char *tag) { ps5_mem_log_census(tag); }

uint64_t plat_mem_dmem_avail(void) {
    int64_t phys = 0;
    size_t avail = 0;
    if (sceKernelAvailableDirectMemorySize(0, INT64_MAX, PS5_MEM_UNIT, &phys, &avail) == 0)
        return (uint64_t)avail;
    return UINT64_MAX;
}

static bool back_unit_locked(Ps5MixedBlock *unit, RangeKind kind, int in_signal, int cause) {
    if (unit->backing.committed) return true;
    Ps5MemCommitReport rep = {0};
    int rc = ps5_mixed_back_report(unit, &rep);
    if (rc) {
        const char *call = rep.allocation_status ? "sceKernelAllocateDirectMemory"
                           : (rep.fixed_attempted && rep.mapping_status) ? "sceKernelMapDirectMemory"
                           : "ps5_mixed_back";
        int err = rep.allocation_status ? rep.allocation_status
                  : (rep.fixed_attempted && rep.mapping_status) ? rep.mapping_status : rc;
        log_backed_totals(call, unit->backing.address, err, in_signal);
        return false;
    }
    __atomic_add_fetch(&backed_units[kind], 1, __ATOMIC_RELAXED);
    if ((unsigned)cause < PLAT_BACK_N)
        __atomic_add_fetch(&backed_by_cause[cause], 1, __ATOMIC_RELAXED);
    for (uint64_t off = 0; off < PS5_MEM_UNIT; off += PS5_MEM_PAGE) {
        uint64_t p = unit->backing.address + off;
        if (prot_watch_is_early(p)) {
            int prot = prot_watch_has_gpu(p, PS5_MEM_PAGE)
                           ? (PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE)
                           : 0;
            sceKernelMprotect((void *)(uintptr_t)p, PS5_MEM_PAGE, prot);
        }
    }
    prot_watch_touch(unit->backing.address, PS5_MEM_UNIT);
    size_t total = __atomic_load_n(&backed_units[RANGE_HEAP], __ATOMIC_RELAXED) +
                   __atomic_load_n(&backed_units[RANGE_ALIAS], __ATOMIC_RELAXED) +
                   __atomic_load_n(&backed_units[RANGE_OTHER], __ATOMIC_RELAXED);
    size_t target = __atomic_load_n(&next_log_units, __ATOMIC_RELAXED);
    if (total >= target &&
        __atomic_compare_exchange_n(&next_log_units, &target, target + LOG_EVERY_UNITS, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        log_backed_totals("summary", unit->backing.address, 0, in_signal);
    }
    return true;
}

static VmRange **range_slot(uint64_t address) {
    VmRange **slot = &ranges;
    while (*slot && (*slot)->end <= address) slot = &(*slot)->next;
    return slot;
}

static VmRange *find_range_lockfree(uint64_t address) {
    for (VmRange *r = __atomic_load_n(&ranges, __ATOMIC_ACQUIRE); r;
         r = __atomic_load_n(&r->next, __ATOMIC_ACQUIRE)) {
        if (address < r->base) return NULL;
        if (address < r->end) return r;
    }
    return NULL;
}

static Ps5MixedBlock *find_unit_lockfree(const VmRange *range, uint64_t address) {
    if (!range || range->host || !range->chunks || address < range->base || address >= range->end) return NULL;
    size_t cidx = (size_t)((address - range->base) >> CHUNK_SHIFT);
    Ps5MixedBlock *chunk = __atomic_load_n(&range->chunks[cidx], __ATOMIC_ACQUIRE);
    if (!chunk) return NULL;
    size_t uidx = (size_t)(((address - range->base) & (CHUNK_SIZE - 1)) / PS5_MEM_UNIT);
    return &chunk[uidx];
}

// Page-protection changes by caller (diagnosis, 60 fps work 15/11/2026: the game's threads spent ~10 ms a frame of CPU in
// change_locked, ps5_pw_protect and ps5_mixed_protect in a race). plat_mem_protect and plat_mem_early_protect count
// by their caller's return address; the write watch's own calls by kind (1 a fault's unprotect, 2 an arm).
// perf.c logs the table every 600 frames (plat_mem_protect_stats).
#define PSTAT_N 64
static struct { uintptr_t ra; uint64_t n, ns; } pstat[PSTAT_N];

static void pstat_add(uintptr_t ra, uint64_t ns) {
    for (uint32_t i = (uint32_t)((ra >> 4) * 0x9e3779b1u) % PSTAT_N, k = 0; k < PSTAT_N; k++, i = (i + 1) % PSTAT_N) {
        uintptr_t cur = __atomic_load_n(&pstat[i].ra, __ATOMIC_ACQUIRE);
        if (!cur) {
            uintptr_t z = 0;
            if (!__atomic_compare_exchange_n(&pstat[i].ra, &z, ra, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) cur = z;
            else cur = ra;
        }
        if (cur == ra) {
            __atomic_add_fetch(&pstat[i].n, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&pstat[i].ns, ns, __ATOMIC_RELAXED);
            return;
        }
    }
}

void plat_mem_protect_stats(void (*emit)(uintptr_t ra, uint64_t n, uint64_t ns, void *ctx), void *ctx) {
    for (int i = 0; i < PSTAT_N; i++) {
        uintptr_t ra = __atomic_load_n(&pstat[i].ra, __ATOMIC_ACQUIRE);
        if (!ra) continue;
        uint64_t n = __atomic_exchange_n(&pstat[i].n, 0, __ATOMIC_RELAXED);
        uint64_t ns = __atomic_exchange_n(&pstat[i].ns, 0, __ATOMIC_RELAXED);
        if (n) emit(ra, n, ns, ctx);
    }
}

static int ps5_pw_protect(uint64_t addr, size_t size, int rw) {
    int prot = rw ? (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE) : PS5_KERNEL_PROT_CPU_READ;
    if (prot_watch_has_gpu(addr, size)) prot |= PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE;
    uint64_t t0 = plat_now_ns();
    int ok = sceKernelMprotect((void *)(uintptr_t)addr, size, prot) == 0;
    pstat_add(rw ? 1 : 2, plat_now_ns() - t0);
    return ok;
}

static int ps5_pw_state(uint64_t addr) {
    VmRange *r = find_range_lockfree(addr);
    Ps5MixedBlock *u = find_unit_lockfree(r, addr);
    if (!u || !u->backing.reserved || !u->backing.committed || !u->live_pages) return PLAT_MEM_RESERVED;
    size_t first = ((addr - u->backing.address) / PS5_MEM_PAGE) * (PS5_MEM_PAGE / PAGE);
    bool any_rw = false, any_read = false;
    for (size_t i = first; i < first + (PS5_MEM_PAGE / PAGE); ++i) {
        Ps5GuestPage p = (Ps5GuestPage)__atomic_load_n(&u->pages[i], __ATOMIC_RELAXED);
        if (p == PS5_GUEST_RW) { any_rw = true; break; }
        if (p == PS5_GUEST_READ) any_read = true;
    }
    return any_rw ? PLAT_PROT_RW : any_read ? PLAT_PROT_READ : PLAT_PROT_NONE;
}

static struct sigaction *prev_action_for(int sig) {
    switch (sig) {
    case SIGSEGV: return &prev_sigsegv;
    case SIGBUS:  return &prev_sigbus;
    case SIGILL:  return &prev_sigill;
    case SIGFPE:  return &prev_sigfpe;
    default:      return NULL;
    }
}

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = (ucontext_t *)uc;
#ifdef __PROSPERO__
    uint64_t rip = (uint64_t)u->uc_mcontext.mc_rip;
    uint64_t rsp = (uint64_t)u->uc_mcontext.mc_rsp;
    uint64_t err = (uint64_t)u->uc_mcontext.mc_err;
#elif defined(__linux__)
    uint64_t rip = (uint64_t)u->uc_mcontext.gregs[REG_RIP];
    uint64_t rsp = (uint64_t)u->uc_mcontext.gregs[REG_RSP];
    uint64_t err = (uint64_t)u->uc_mcontext.gregs[REG_ERR];
#else
    (void)u;
    uint64_t rip = 0, rsp = 0, err = 0;
#endif
    uint64_t addr = si ? (uint64_t)(uintptr_t)si->si_addr : 0;
    bool is_write = (err & 2) != 0;
    bool is_exec = (err & 16) != 0;

    bool back_failed = false;
    // A protect batch open on this thread (change) must not swallow the protections this handler makes.
    Ps5ProtBatch *batch = ps5_mem_batch_swap(NULL);
    if (sig == SIGSEGV || sig == SIGBUS) {
        VmRange *r = find_range_lockfree(addr);
        Ps5MixedBlock *unit = find_unit_lockfree(r, addr);
        if (unit) {
            unit_lock(unit);
            bool committed = unit->backing.reserved && unit->live_pages != 0;
            Ps5GuestPage st = ps5_mixed_state(unit, addr);
            bool perm_ok = is_write ? (st == PS5_GUEST_RW) : (st >= PS5_GUEST_READ);
            if (committed && perm_ok && !unit->backing.committed) {
                bool backed = back_unit_locked(unit, r->kind, 1, is_write ? PLAT_BACK_WRITE_FAULT : PLAT_BACK_READ_FAULT);
                if (backed && is_write) prot_watch_mark_written(addr & ~(uint64_t)(PS5_MEM_PAGE - 1), PS5_MEM_PAGE);
                unit_unlock(unit);
                if (backed) { ps5_mem_batch_swap(batch); return; } // Retry the faulting access now that the unit is backed.
                // Backing a committed guest unit failed (OOM / VM map limit). Never fall through to
                // thaw_fault (which sees an RW guest page and returns 1, causing an infinite fault loop).
                back_failed = true;
            } else if (committed && perm_ok && unit->backing.committed) {
                if (prot_watch_is_early(addr)) {
                    unit_unlock(unit);
                } else if (is_write && prot_watch_fault(addr)) {
                    unit_unlock(unit);
                    ps5_mem_batch_swap(batch);
                    return;
                } else {
                    unit_unlock(unit);
                }
            } else {
                unit_unlock(unit);
            }
        }
    }

    if (sig == SIGSEGV || sig == SIGBUS) plat_fault_guarded();   // a guarded copy (fault_guard.c): no hook, back to it
    plat_fault_write = is_write ? 1 : 0;
    plat_fault_pc = rip;
    plat_fault_ret = (rsp && !(rsp & 7)) ? *(const uint64_t *)(uintptr_t)rsp : 0;

    if (!back_failed && (sig == SIGSEGV || sig == SIGBUS)) {
        int (*fault_fn)(uint64_t) = __atomic_load_n(&g_fault_hook, __ATOMIC_ACQUIRE);
        if (fault_fn && fault_fn(addr)) { ps5_mem_batch_swap(batch); return; }
        int (*watch_fn)(uint64_t, uint64_t) = __atomic_load_n(&g_watch_hook, __ATOMIC_ACQUIRE);
        if (is_write && watch_fn && watch_fn(addr, rip)) { ps5_mem_batch_swap(batch); return; }
    }

    void (*crash_fn)(const PlatCrash *) = __atomic_load_n(&g_crash_hook, __ATOMIC_ACQUIRE);
    if (crash_fn) {
        PlatCrash c = {
            .what = back_failed ? "direct memory backing failed" :
                    sig == SIGSEGV ? "access violation" :
                    sig == SIGBUS  ? "bus error" :
                    sig == SIGILL  ? "illegal instruction" : "arithmetic exception",
            .code = (uint32_t)sig,
            .rip = rip,
            .offset = rip,   // the eboot's load base is not known here: report the absolute rip
            .access = -1,
            .ret = plat_fault_ret,
            .module = "no module",
        };
        if (sig == SIGSEGV || sig == SIGBUS) {
            c.access = is_exec ? 8 : is_write ? 1 : 0;
            c.addr = addr;
        }
        crash_fn(&c);
    }
    if (back_failed) {
        fflush(NULL);
        _exit(1);
    }

    struct sigaction *prev = prev_action_for(sig);
    if (prev) sigaction(sig, prev, NULL);
    else signal(sig, SIG_DFL);
}

static void ensure_alt_stack(void) {
    stack_t cur = {0};
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_size >= ALT_STACK_BYTES) return;
    void *mem = malloc(ALT_STACK_BYTES);
    if (!mem) return;
    stack_t ss = {.ss_sp = mem, .ss_size = ALT_STACK_BYTES};
    if (sigaltstack(&ss, NULL) != 0) free(mem);
}

static void install_signal_handlers(void) {
    prot_watch_init(PS5_MEM_PAGE, ps5_pw_protect, ps5_pw_state);
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, &prev_sigsegv);
    sigaction(SIGBUS, &sa, &prev_sigbus);
    sigaction(SIGILL, &sa, &prev_sigill);
    sigaction(SIGFPE, &sa, &prev_sigfpe);
}

static void fault_install(void) {
    ensure_alt_stack();
    pthread_once(&fault_once, install_signal_handlers);
}

static bool pending_overlap(uint64_t address, uint64_t end) {
    for (const VmRange *range = cleanup_pending; range; range = range->next)
        if (address < range->end && range->base < end) return true;
    return false;
}

static void discard_unexpected(VmRange *range, void *address, size_t bytes) {
    if (address && !sdk_ok("release unexpected mapping", (uintptr_t)address, bytes,
                           sceKernelMunmap(address, bytes))) {
        *range = (VmRange){.base = (uintptr_t)address, .end = (uintptr_t)address + bytes,
                           .next = cleanup_pending};
        cleanup_pending = range;
    } else free(range);
}

int plat_mem_reserve(uint64_t address, uint64_t bytes, int watch) {
    fault_install();
    if (!address || !bytes || (address | bytes) % PS5_MEM_UNIT ||
        address >= address_limit || bytes > address_limit - address) return 0;
    size_t chunk_count = (size_t)((bytes + CHUNK_SIZE - 1) >> CHUNK_SHIFT);
    VmRange *range = calloc(1, sizeof *range);
    Ps5MixedBlock *volatile *chunks = calloc(chunk_count, sizeof *chunks);
    if (!range || !chunks) {
        free((void *)chunks);
        free(range);
        return 0;
    }
    RangeKind kind = RANGE_OTHER;
    if (bytes == (1ull << 36)) kind = RANGE_ALIAS;
    else if (watch && bytes >= (4ull << 30)) kind = RANGE_HEAP;

    pthread_mutex_lock(&vm_lock);
    VmRange **slot = range_slot(address);
    bool exact = false;
    void *unexpected = NULL;
    if (!stopped && (!*slot || (*slot)->base >= address + bytes) && !pending_overlap(address, address + bytes)) {
        void *placed = (void *)(uintptr_t)address;
        int result = sceKernelReserveVirtualRange(&placed, bytes, 0, PS5_MEM_UNIT);
        if (sdk_ok("reserve", address, bytes, result)) {
            exact = placed == (void *)(uintptr_t)address;
            if (!exact) {
                fprintf(stderr, "PS5 memory reserve %#llx relocated to %p\n", (unsigned long long)address, placed);
                unexpected = placed;
            }
        }
    }
    if (exact) {
        *range = (VmRange){
            .base = address,
            .end = address + bytes,
            .chunks = chunks,
            .chunk_count = chunk_count,
            .kind = kind,
            .next = *slot,
        };
        __atomic_store_n(slot, range, __ATOMIC_RELEASE);
        if (watch) prot_watch_reserve(address, bytes);
    } else {
        free((void *)chunks);
        discard_unexpected(range, unexpected, bytes);
    }
    pthread_mutex_unlock(&vm_lock);
    return exact;
}

void *plat_mem_alloc(size_t bytes) {
    if (!bytes || bytes > SIZE_MAX - (PS5_MEM_PAGE - 1)) return NULL;
    bytes = (bytes + PS5_MEM_PAGE - 1) & ~(size_t)(PS5_MEM_PAGE - 1);
    VmRange *range = calloc(1, sizeof *range);
    if (!range) return NULL;
    pthread_mutex_lock(&vm_lock);
    void *mapped = stopped ? MAP_FAILED :
                   mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped != MAP_FAILED) {
        uint64_t base = (uintptr_t)mapped;
        VmRange **slot = range_slot(base);
        // A hint-free allocation could land in a hole left by a lost guest
        // slot. Keep that address unavailable to either allocator.
        if ((*slot && (*slot)->base < base + bytes) || pending_overlap(base, base + bytes)) {
            discard_unexpected(range, mapped, bytes);
            mapped = MAP_FAILED;
        } else {
            *range = (VmRange){.base = base, .end = base + bytes, .host = true, .next = *slot};
            __atomic_store_n(slot, range, __ATOMIC_RELEASE);
        }
    } else free(range);
    pthread_mutex_unlock(&vm_lock);
    return mapped == MAP_FAILED ? NULL : mapped;
}

// Caller must hold vm_lock.
static Ps5MixedBlock *find_unit(VmRange *range, uint64_t address, bool create) {
    uint64_t base = address & ~(uint64_t)(PS5_MEM_UNIT - 1);
    size_t cidx = (size_t)((base - range->base) >> CHUNK_SHIFT);
    Ps5MixedBlock *chunk = __atomic_load_n(&range->chunks[cidx], __ATOMIC_ACQUIRE);
    if (!chunk) {
        if (!create) return NULL;
        chunk = calloc(CHUNK_UNITS, sizeof *chunk);
        if (!chunk) return NULL;
        uint64_t chunk_base = range->base + ((uint64_t)cidx << CHUNK_SHIFT);
        for (size_t i = 0; i < CHUNK_UNITS; ++i) {
            uint64_t uaddr = chunk_base + (uint64_t)i * PS5_MEM_UNIT;
            if (uaddr >= range->end) break;
            // The full window owns this unit. There is no separate reservation call.
            chunk[i].backing = (Ps5MemBlock){.address = uaddr, .reserved = true};
        }
        __atomic_store_n(&range->chunks[cidx], chunk, __ATOMIC_RELEASE);
    }
    size_t uidx = (size_t)(((base - range->base) & (CHUNK_SIZE - 1)) / PS5_MEM_UNIT);
    return &chunk[uidx];
}

typedef enum { COMMIT, DECOMMIT, PROTECT } Change;
static bool change(uint64_t address, uint64_t bytes, Change action, int protection) {
    if (!bytes || (address | bytes) % PAGE || bytes > UINT64_MAX - address) return false;
    uint64_t end = address + bytes;
    VmRange *range = *range_slot(address);
    if (!range || address < range->base || end > range->end) return false;
    if (range->host) {
        // The dispatch table is native-page aligned. A partial native page
        // may contain other live host data.
        if (action != PROTECT || (address | bytes) % PS5_MEM_PAGE) return false;
        const int flags[] = {0, PS5_KERNEL_PROT_CPU_READ, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE};
        return sdk_ok("protect host", address, bytes,
                      sceKernelMprotect((void *)(uintptr_t)address, bytes, flags[protection]));
    }

    // Reject invalid spans before making any changes.
    if ((action == DECOMMIT || (action == PROTECT && protection == PLAT_PROT_NONE)) &&
        prot_watch_has_gpu(address, bytes))
        return false;
    for (uint64_t at = address; at < end; at += PAGE) {
        Ps5MixedBlock *unit = find_unit(range, at, false);
        Ps5GuestPage state = unit ? ps5_mixed_state(unit, at) : PS5_GUEST_RESERVED;
        if (state == PS5_GUEST_FREE || (action == PROTECT && state == PS5_GUEST_RESERVED)) return false;
    }

    // Commits stay lazy per 64 KiB unit regardless of span size: MKW's
    // nninitStartup commits the entire 8 GiB heap in one 2 MiB-aligned call,
    // so eager backing of >= 2 MiB spans would exhaust direct memory immediately.
    // PROTECT: one sceKernelMprotect per run of native pages left in one mode (ps5_mem_batch_begin), the units held
    // locked, in address order, until the flush. Nothing else holds two unit locks.
    Ps5ProtBatch batch;
    Ps5MixedBlock *held[64];
    int nheld = 0, batching = action == PROTECT;
    if (batching) ps5_mem_batch_begin(&batch);
#define CHANGE_RELEASE()                                                                                               \
    do {                                                                                                               \
        int berr_ = batching ? ps5_mem_batch_flush() : 0;                                                              \
        for (int k_ = 0; k_ < nheld; k_++) unit_unlock(held[k_]);                                                      \
        nheld = 0;                                                                                                     \
        if (berr_) {                                                                                                   \
            sdk_ok("protect", address, bytes, berr_);                                                                  \
            ok_all = false;                                                                                            \
        }                                                                                                              \
    } while (0)
    bool ok_all = true;
    for (uint64_t at = address; at < end; at += PAGE) {
        Ps5MixedBlock *unit = find_unit(range, at, action == COMMIT);
        if (!unit) {
            if (action == DECOMMIT) continue;
            if (batching) { CHANGE_RELEASE(); ps5_mem_batch_end(); }
            return false;
        }
        if (!batching) {
            unit_lock(unit);
        } else if (!nheld || held[nheld - 1] != unit) {
            if (nheld == (int)(sizeof held / sizeof *held)) CHANGE_RELEASE();
            if (!ok_all) { ps5_mem_batch_end(); return false; }
            unit_lock(unit);
            held[nheld++] = unit;
        }
        const size_t offset = at - unit->backing.address;
        Ps5GuestPage state = ps5_mixed_state(unit, at);
        bool was_committed = unit->backing.committed;
        int result;
        if (action == DECOMMIT)
            result = state == PS5_GUEST_RESERVED ? 0 : ps5_mixed_decommit(unit, offset);
        else if (state == PS5_GUEST_RESERVED)
            result = ps5_mixed_commit_lazy(unit, offset);
        else
            result = ps5_mixed_protect(unit, offset, (Ps5GuestPage)(PS5_GUEST_NONE + protection));
        if (!result) {
            if (action == DECOMMIT && state != PS5_GUEST_RESERVED)
                prot_watch_mark_written(at & ~(uint64_t)(PS5_MEM_PAGE - 1), PS5_MEM_PAGE);
            else if (action == PROTECT && protection == PLAT_PROT_RW && unit->backing.committed &&
                     prot_watch_is_armed(at))
                result = ps5_mem_protect_native(&unit->backing,
                                                (at - unit->backing.address) & ~(uint64_t)(PS5_MEM_PAGE - 1),
                                                PS5_MEM_PAGE, PS5_MEM_READ);
        }
        if (action == DECOMMIT && was_committed && !unit->backing.committed)
            __atomic_sub_fetch(&backed_units[range->kind], 1, __ATOMIC_RELAXED);
        if (!batching) unit_unlock(unit);
        if (!sdk_ok(action == COMMIT ? "commit" : action == DECOMMIT ? "decommit" : "protect", at, PAGE, result)) {
            if (batching) { CHANGE_RELEASE(); ps5_mem_batch_end(); }
            return false;
        }
    }
    if (batching) { CHANGE_RELEASE(); ps5_mem_batch_end(); }
#undef CHANGE_RELEASE
    return ok_all;
}

static int change_locked(uint64_t address, uint64_t bytes, Change action, int protection) {
    pthread_mutex_lock(&vm_lock);
    bool ok = change(address, bytes, action, protection);
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

int plat_mem_commit(uint64_t address, size_t bytes) { return change_locked(address, bytes, COMMIT, PLAT_PROT_RW); }
int plat_mem_decommit(uint64_t address, uint64_t bytes) { return change_locked(address, bytes, DECOMMIT, 0); }
__attribute__((noinline)) int plat_mem_protect(uint64_t address, uint64_t bytes, int protection) {
    uint64_t t0 = plat_now_ns();
    int ok = protection >= PLAT_PROT_NONE && protection <= PLAT_PROT_RW &&
             change_locked(address, bytes, PROTECT, protection);
    pstat_add((uintptr_t)__builtin_return_address(0), plat_now_ns() - t0);
    return ok;
}

size_t plat_mem_early_page_size(void) { return PS5_MEM_PAGE; }

static PlatEarlyResult early_protect_failed(uint64_t lo, uint64_t restore_hi, int on) {
    if (!on) return PLAT_EARLY_UNRESTORED;
    if (restore_hi > lo &&
        plat_mem_early_protect(lo, (size_t)(restore_hi - lo), 0, 0) != PLAT_EARLY_OK)
        return PLAT_EARLY_UNRESTORED;
    return PLAT_EARLY_FAILED;
}

// Mark a run before arming to keep write-watch from changing the protection. Keep it marked until a restore succeeds.
static int early_protect_run(uint64_t lo, uint64_t hi, int prot, int on) {
    if (on)
        for (uint64_t p = lo; p < hi; p += PS5_MEM_PAGE) prot_watch_set_early(p, 1);
    if (sceKernelMprotect((void *)(uintptr_t)lo, (size_t)(hi - lo), prot) != 0) return 0;
    if (!on)
        for (uint64_t p = lo; p < hi; p += PS5_MEM_PAGE) prot_watch_set_early(p, 0);
    return 1;
}

static PlatEarlyResult early_protect(uint64_t addr, size_t n, int on, int restore_prot) {
    (void)restore_prot;
    if (!n || n > UINT64_MAX - (PS5_MEM_PAGE - 1) || addr > UINT64_MAX - n - (PS5_MEM_PAGE - 1))
        return on ? PLAT_EARLY_FAILED : PLAT_EARLY_UNRESTORED;
    uint64_t lo = addr & ~(uint64_t)(PS5_MEM_PAGE - 1);
    uint64_t hi = (addr + n + PS5_MEM_PAGE - 1) & ~(uint64_t)(PS5_MEM_PAGE - 1);
    // A run of equal protection goes across 64 KiB units in one sceKernelMprotect (~44 us each on the PS5): the
    // early-read watch's arms (memory_early.c flush_watch_run) were one call a unit, 85 us each, ~1.2 ms a frame in a
    // race (15/11/2026). The run's units stay locked, in address order, until it is made; a failure unlocks them all
    // before early_protect_failed, which locks units itself.
    Ps5MixedBlock *held[64];
    int nheld = 0;
    uint64_t run_lo = 0, run_hi = 0;
    int run_prot = -1;
#define EP_UNLOCK_ALL() do { for (int k_ = 0; k_ < nheld; k_++) unit_unlock(held[k_]); nheld = 0; } while (0)
#define EP_FLUSH() (run_lo < run_hi ? early_protect_run(run_lo, run_hi, run_prot, on) : 1)
    for (uint64_t p = lo; p < hi;) {
        VmRange *r = find_range_lockfree(p);
        Ps5MixedBlock *u = find_unit_lockfree(r, p);
        uint64_t u_end = (p & ~(uint64_t)(PS5_MEM_UNIT - 1)) + PS5_MEM_UNIT;
        if (u_end > hi) u_end = hi;
        if (!u || nheld == (int)(sizeof held / sizeof *held)) {   // the run ends here: made, then its units unlocked
            int ok = EP_FLUSH();
            uint64_t made_hi = run_hi;
            run_lo = run_hi = 0;
            EP_UNLOCK_ALL();
            if (!ok) return early_protect_failed(lo, made_hi, on);
            if (!u) {
                if (on) return early_protect_failed(lo, p, on);
                p = u_end;
                continue;
            }
        }
        unit_lock(u);
        held[nheld++] = u;
        if (!u->backing.reserved || !u->live_pages) {
            int ok = EP_FLUSH();
            uint64_t made_hi = run_hi;
            run_lo = run_hi = 0;
            if (!on)
                for (uint64_t q = p; q < u_end; q += PS5_MEM_PAGE) prot_watch_set_early(q, 0);
            EP_UNLOCK_ALL();
            if (!ok) return early_protect_failed(lo, made_hi, on);
            if (on) return early_protect_failed(lo, p, on);
            p = u_end;
            continue;
        }
        for (; p < u_end; p += PS5_MEM_PAGE) {
            size_t first = ((p - u->backing.address) / PS5_MEM_PAGE) * (PS5_MEM_PAGE / PAGE);
            bool any_rw = false, any_read = false;
            for (size_t i = first; i < first + (PS5_MEM_PAGE / PAGE); ++i) {
                Ps5GuestPage st = (Ps5GuestPage)__atomic_load_n(&u->pages[i], __ATOMIC_RELAXED);
                if (st == PS5_GUEST_RW) { any_rw = true; break; }
                if (st == PS5_GUEST_READ) any_read = true;
            }
            if (on && !any_rw && !any_read) {
                int ok = EP_FLUSH();
                uint64_t made_hi = run_lo < run_hi ? run_hi : p;
                EP_UNLOCK_ALL();
                (void)ok;
                return early_protect_failed(lo, made_hi, on);
            }
            if (!u->backing.committed) {   // no mapping to protect: the run stops before it
                if (!EP_FLUSH()) {
                    uint64_t made_hi = run_hi;
                    EP_UNLOCK_ALL();
                    return early_protect_failed(lo, made_hi, on);
                }
                run_lo = run_hi = 0;
                prot_watch_set_early(p, on != 0);
                continue;
            }
            int prot;
            if (on) {
                prot = prot_watch_has_gpu(p, PS5_MEM_PAGE)
                           ? (PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE)
                           : 0;
            } else {
                int rw = any_rw && !prot_watch_is_armed(p);
                prot = rw ? (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
                          : (any_read || any_rw) ? PS5_KERNEL_PROT_CPU_READ
                                                 : 0;
                if ((any_read || any_rw) && prot_watch_has_gpu(p, PS5_MEM_PAGE))
                    prot |= PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE;
            }
            if (run_lo < run_hi && (prot != run_prot || run_hi != p)) {
                if (!early_protect_run(run_lo, run_hi, run_prot, on)) {
                    uint64_t made_hi = run_hi;
                    EP_UNLOCK_ALL();
                    return early_protect_failed(lo, made_hi, on);
                }
                run_lo = run_hi = 0;
                // The units before u held only that run: they can go (u may hold the next one).
                for (int k = 0; k < nheld - 1; k++) unit_unlock(held[k]);
                held[0] = held[nheld - 1], nheld = 1;
            }
            if (!run_hi) {
                run_lo = p;
                run_prot = prot;
            }
            run_hi = p + PS5_MEM_PAGE;
        }
    }
    {
        int ok = EP_FLUSH();
        uint64_t made_hi = run_hi;
        EP_UNLOCK_ALL();
        if (!ok) return early_protect_failed(lo, made_hi, on);
    }
#undef EP_FLUSH
#undef EP_UNLOCK_ALL
    return PLAT_EARLY_OK;
}

__attribute__((noinline)) PlatEarlyResult plat_mem_early_protect(uint64_t addr, size_t n, int on, int restore_prot) {
    uint64_t t0 = plat_now_ns();
    PlatEarlyResult r = early_protect(addr, n, on, restore_prot);
    pstat_add((uintptr_t)__builtin_return_address(0), plat_now_ns() - t0);
    return r;
}

int plat_mem_back_hold_as(uint64_t addr, size_t n, int cause) {
    if (!n) return 1;
    if (n > UINT64_MAX - addr) return 0;
    void (*eh)(uint64_t, size_t) = __atomic_load_n(&g_early_hold_hook, __ATOMIC_ACQUIRE);
    if (eh) eh(addr, n);
    uint64_t end = addr + n;
    uint64_t at = addr & ~(uint64_t)(PS5_MEM_UNIT - 1);
    while (at < end) {
        VmRange *r = find_range_lockfree(at);
        if (!r || r->host) {
            if (r && r->end > at) {
                at = (r->end + PS5_MEM_UNIT - 1) & ~(uint64_t)(PS5_MEM_UNIT - 1);
                continue;
            }
            at += PS5_MEM_UNIT;
            continue;
        }
        Ps5MixedBlock *u = find_unit_lockfree(r, at);
        if (u) {
            unit_lock(u);
            bool ok = true;
            if (u->backing.reserved && u->live_pages && !u->backing.committed)
                ok = back_unit_locked(u, r->kind, 0, cause);
            unit_unlock(u);
            if (!ok) return 0;
            at += PS5_MEM_UNIT;
        } else {
            // Skip the entire unallocated 16 MiB chunk.
            uint64_t next_chunk = r->base + (((at - r->base) >> CHUNK_SHIFT) + 1) * (uint64_t)CHUNK_SIZE;
            at = next_chunk < r->end ? next_chunk : r->end;
        }
    }
    prot_watch_hold(addr, n);
    return 1;
}

static void (*thaw_fn)(uint64_t addr, size_t n);
void plat_mem_thaw_hook(void (*fn)(uint64_t addr, size_t n)) { __atomic_store_n(&thaw_fn, fn, __ATOMIC_RELEASE); }

void plat_mem_early_resolve(uint64_t addr, size_t n) {
    if (!n) return;
    void (*eh)(uint64_t, size_t) = __atomic_load_n(&g_early_hold_hook, __ATOMIC_ACQUIRE);
    if (eh) eh(addr, n);
    void (*th)(uint64_t, size_t) = __atomic_load_n(&thaw_fn, __ATOMIC_ACQUIRE);
    if (th) th(addr, n);
}

int plat_mem_back_hold(uint64_t addr, size_t n) {
    return plat_mem_back_hold_as(addr, n, PLAT_BACK_RT_READ);
}

void plat_mem_back_release(uint64_t addr, size_t n) {
    if (n) prot_watch_release(addr, n);
}

int plat_mem_watch_info(uint64_t addr) { return prot_watch_page_info(addr); }
void plat_mem_gpu_import(uint64_t addr, size_t n, int on) { prot_watch_gpu_import(addr, n, on); }

int plat_mem_back(uint64_t addr, size_t n) {
    if (!plat_mem_back_hold(addr, n)) return 0;
    plat_mem_back_release(addr, n);
    return 1;
}

static int page_state(VmRange *range, uint64_t address, uint64_t *end) {
    if (!range || address < range->base) {
        *end = range ? range->base : UINT64_MAX;
        return PLAT_MEM_FREE;
    }
    if (range->host) {
        void *start = NULL, *finish = NULL;
        uint32_t flags = 0;
        if (sceKernelQueryMemoryProtection((void *)(uintptr_t)address, &start, &finish, &flags) ||
            (uintptr_t)start > address || (uintptr_t)finish <= address) {
            *end = range->end;
            return PLAT_MEM_FREE;
        }
        *end = (uintptr_t)finish < range->end ? (uintptr_t)finish : range->end;
        if (!(flags & PS5_KERNEL_PROT_CPU_READ)) return PLAT_PROT_NONE;
        return flags & PS5_KERNEL_PROT_CPU_WRITE ? PLAT_PROT_RW : PLAT_PROT_READ;
    }
    const uint64_t base = address & ~(uint64_t)(PS5_MEM_UNIT - 1);
    size_t cidx = (size_t)((base - range->base) >> CHUNK_SHIFT);
    Ps5MixedBlock *chunk = range->chunks[cidx];
    if (!chunk) {
        size_t next_c = cidx + 1;
        while (next_c < range->chunk_count && !range->chunks[next_c]) ++next_c;
        uint64_t next_addr = range->base + ((uint64_t)next_c << CHUNK_SHIFT);
        *end = next_addr < range->end ? next_addr : range->end;
        return PLAT_MEM_RESERVED;
    }
    const Ps5MixedBlock *unit = &chunk[((base - range->base) & (CHUNK_SIZE - 1)) / PS5_MEM_UNIT];
    if (!unit->backing.reserved || !unit->live_pages) {
        *end = base + PS5_MEM_UNIT;
        return unit->backing.reserved ? PLAT_MEM_RESERVED : PLAT_MEM_FREE;
    }
    *end = (address | (PAGE - 1)) + 1;
    return ps5_mixed_state(unit, address) - PS5_GUEST_NONE;
}

static int state_locked(uint64_t address, uint64_t limit, uint64_t *end) {
    VmRange *range = *range_slot(address);
    int state = page_state(range, address, end);
    // Adjacent records or windows can have the same logical state.
    while (*end < limit) {
        if (range && *end >= range->end) range = range->next;
        uint64_t next;
        if (page_state(range, *end, &next) != state) break;
        *end = next;
    }
    if (*end > limit) *end = limit;
    return state;
}

int plat_mem_state(uint64_t address, uint64_t limit, uint64_t *end) {
    pthread_mutex_lock(&vm_lock);
    int state = state_locked(address, limit, end);
    pthread_mutex_unlock(&vm_lock);
    return state;
}

static bool readable_locked(uint64_t address, uint64_t bytes) {
    if (bytes > UINT64_MAX - address) return false;
    for (uint64_t at = address, end = address + bytes, next; at < end; at = next)
        if (state_locked(at, end, &next) < PLAT_PROT_READ) return false;
    return true;
}

int plat_mem_readable(uint64_t address, uint64_t bytes) {
    pthread_mutex_lock(&vm_lock);
    bool ok = readable_locked(address, bytes ? bytes : 1);
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

int plat_mem_read(uint64_t address, void *out, size_t bytes) {
    if (!bytes) return 1;
    if (!out) return 0;
    pthread_mutex_lock(&vm_lock);
    bool ok = readable_locked(address, bytes);
    pthread_mutex_unlock(&vm_lock);
    if (!ok || !plat_mem_back(address, bytes)) return 0;
    pthread_mutex_lock(&vm_lock);
    ok = readable_locked(address, bytes);
    // Guarded: a page the early-read watch protects faults here with vm_lock held, and the fault hook's frz_lock is
    // held by mem_early_watch waiting for vm_lock (run 25's hang, fault_guard.c). The copy fails instead; mem_copy_out
    // then reads it the plain way, holding no platform lock.
    if (ok) ok = plat_mem_copy_guarded(out, (const void *)(uintptr_t)address, bytes);
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

int plat_mem_peek(void *out, uint64_t address, size_t bytes) { return plat_mem_read(address, out, bytes); }
void *plat_aligned_alloc(size_t size, size_t alignment) {
    void *p = NULL;
    return posix_memalign(&p, alignment, size) ? NULL : p;
}
void plat_aligned_free(void *p) { free(p); }

int plat_mem_poke(uint64_t address, const void *src, size_t bytes) {
    (void)address; (void)src; (void)bytes;
    return 0; // Protected write-through is unavailable on this platform.
}
int plat_mem_replace_anon(uint64_t address, uint64_t bytes) {
    (void)address; (void)bytes;
    return 0; // The anonymous replacement path is Linux-only.
}
uint64_t plat_mem_written(uint64_t address, uint64_t bytes, uint64_t *pages, uint64_t max, int reset) {
    static int pt_reset_logged;
    int did_protect = 0;
    uint64_t got = prot_watch_written(address, bytes, pages, max, reset, &did_protect);
    if (did_protect && !__atomic_exchange_n(&pt_reset_logged, 1, __ATOMIC_ACQ_REL)) {
        int cpu_tot = 0, cpu_free = 0, gpu_tot = 0, gpu_free = 0;
        if (sceKernelGetPageTableStats(&cpu_tot, &cpu_free, &gpu_tot, &gpu_free) == 0)
            fprintf(stderr, "PS5 memory write-watch first reset: pt=cpu:%d/%d gpu:%d/%d\n",
                    cpu_free, cpu_tot, gpu_free, gpu_tot);
    }
    return got;
}

uint64_t plat_mem_write_watch_faults(int reset) { return prot_watch_faults(reset); }
uint64_t plat_mem_write_watch_repeats(int reset) { return prot_watch_repeat_faults(reset); }
uint64_t plat_mem_write_watch_hot_skips(int reset) { return prot_watch_hot_skips(reset); }

uint64_t plat_mem_populated(uint64_t address, uint64_t bytes, uint64_t (*runs)[2], uint64_t max) {
    if (!max || !bytes) return 0;
    if (bytes > UINT64_MAX - address) return UINT64_MAX;
    uint64_t lo = address & ~(uint64_t)(PAGE - 1);
    uint64_t hi = (address + bytes + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    uint64_t got = 0;
    uint64_t at = lo;
    while (at < hi && got < max) {
        VmRange *r = find_range_lockfree(at);
        if (!r || r->host) return UINT64_MAX;
        uint64_t ubase = at & ~(uint64_t)(PS5_MEM_UNIT - 1);
        Ps5MixedBlock *u = find_unit_lockfree(r, ubase);
        if (!u) {
            uint64_t next_chunk = r->base + (((ubase - r->base) >> CHUNK_SHIFT) + 1) * (uint64_t)CHUNK_SIZE;
            at = next_chunk < hi ? next_chunk : hi;
            continue;
        }
        uint64_t uend = ubase + PS5_MEM_UNIT;
        uint64_t seg_lo = at > ubase ? at : ubase;
        uint64_t seg_hi = uend < hi ? uend : hi;
        if (__atomic_load_n(&u->backing.committed, __ATOMIC_ACQUIRE)) {
            if (got && runs[got - 1][1] == seg_lo) runs[got - 1][1] = seg_hi;
            else runs[got][0] = seg_lo, runs[got][1] = seg_hi, ++got;
        }
        at = seg_hi;
    }
    return got;
}

void plat_mem_early_hold_hook(void (*fn)(uint64_t addr, size_t n)) {
    __atomic_store_n(&g_early_hold_hook, fn, __ATOMIC_RELEASE);
}
void plat_mem_frozen_hook(int (*fn)(uint64_t addr)) {
    __atomic_store_n(&g_frozen_hook, fn, __ATOMIC_RELEASE);
}

uint64_t plat_mem_reclaim_zeros(uint64_t target_bytes, uint64_t *scanned_bytes) {
    int (*frozen_fn)(uint64_t) = __atomic_load_n(&g_frozen_hook, __ATOMIC_ACQUIRE);
    if (!frozen_fn) {
        if (scanned_bytes) *scanned_bytes = 0;
        return 0;
    }
    uint64_t scanned = 0, reclaimed = 0;
    for (VmRange *r = __atomic_load_n(&ranges, __ATOMIC_ACQUIRE); r && (!target_bytes || reclaimed < target_bytes);
         r = __atomic_load_n(&r->next, __ATOMIC_ACQUIRE)) {
        if (r->host || !r->chunks || (r->kind != RANGE_HEAP && r->kind != RANGE_ALIAS)) continue;
        for (size_t c = 0; c < r->chunk_count && (!target_bytes || reclaimed < target_bytes); ++c) {
            Ps5MixedBlock *chunk = __atomic_load_n(&r->chunks[c], __ATOMIC_ACQUIRE);
            if (!chunk) continue;
            for (size_t i = 0; i < CHUNK_UNITS && (!target_bytes || reclaimed < target_bytes); ++i) {
                Ps5MixedBlock *u = &chunk[i];
                if (!__atomic_load_n(&u->backing.committed, __ATOMIC_ACQUIRE)) continue;
                uint64_t uaddr = u->backing.address;
                if (!frozen_fn(uaddr) || prot_watch_has_gpu(uaddr, PS5_MEM_UNIT)) continue;
                unit_lock(u);
                bool can_scan = u->backing.committed && u->live_pages &&
                                !prot_watch_has_gpu(uaddr, PS5_MEM_UNIT) && frozen_fn(uaddr);
                for (size_t p = 0; can_scan && p < PS5_MEM_UNIT / PAGE; ++p)
                    if (u->pages[p] < PS5_GUEST_READ || prot_watch_is_early(uaddr + p * PAGE)) can_scan = false;
                if (can_scan) {
                    scanned += PS5_MEM_UNIT;
                    const uint64_t *q = (const uint64_t *)(uintptr_t)uaddr;
                    bool zero = true;
                    for (size_t k = 0; k < PS5_MEM_UNIT / sizeof(uint64_t); ++k) {
                        if (q[k]) { zero = false; break; }
                    }
                    if (zero && ps5_mem_decommit(&u->backing) == 0) {
                        __atomic_sub_fetch(&backed_units[r->kind], 1, __ATOMIC_RELAXED);
                        reclaimed += PS5_MEM_UNIT;
                    }
                }
                unit_unlock(u);
            }
        }
    }
    if (scanned_bytes) *scanned_bytes = scanned;
    return reclaimed;
}

uint64_t plat_mem_backed_bytes(uint64_t addr, uint64_t size) {
    if (!size || size > UINT64_MAX - addr) return 0;
    uint64_t end = addr + size;
    uint64_t at = addr & ~(uint64_t)(PS5_MEM_UNIT - 1);
    uint64_t total = 0;
    while (at < end) {
        VmRange *r = find_range_lockfree(at);
        if (!r || r->host) {
            if (r && r->end > at) {
                at = (r->end + PS5_MEM_UNIT - 1) & ~(uint64_t)(PS5_MEM_UNIT - 1);
                continue;
            }
            at += PS5_MEM_UNIT;
            continue;
        }
        Ps5MixedBlock *u = find_unit_lockfree(r, at);
        if (u) {
            if (__atomic_load_n(&u->backing.committed, __ATOMIC_RELAXED)) {
                uint64_t lo = at > addr ? at : addr;
                uint64_t hi = (at + PS5_MEM_UNIT) < end ? (at + PS5_MEM_UNIT) : end;
                if (hi > lo) total += hi - lo;
            }
            at += PS5_MEM_UNIT;
        } else {
            uint64_t next_chunk = r->base + (((at - r->base) >> CHUNK_SHIFT) + 1) * (uint64_t)CHUNK_SIZE;
            at = next_chunk < r->end ? next_chunk : r->end;
        }
    }
    return total;
}

uint64_t plat_mem_release_backing(uint64_t addr, uint64_t size, PlatBackingDiscarded discarded) {
    if (!size || size > UINT64_MAX - addr || addr > UINT64_MAX - (PS5_MEM_UNIT - 1)) return 0;
    uint64_t freed = 0;
    uint64_t lo = (addr + PS5_MEM_UNIT - 1) & ~(uint64_t)(PS5_MEM_UNIT - 1);
    uint64_t hi = (addr + size) & ~(uint64_t)(PS5_MEM_UNIT - 1);
    for (uint64_t at = lo; at < hi;) {
        VmRange *r = find_range_lockfree(at);
        if (!r || r->host) {
            if (r && r->end > at) {
                at = (r->end + PS5_MEM_UNIT - 1) & ~(uint64_t)(PS5_MEM_UNIT - 1);
                continue;
            }
            at += PS5_MEM_UNIT;
            continue;
        }
        Ps5MixedBlock *u = find_unit_lockfree(r, at);
        if (!u) {
            uint64_t next_chunk = r->base + (((at - r->base) >> CHUNK_SHIFT) + 1) * (uint64_t)CHUNK_SIZE;
            at = next_chunk < r->end ? next_chunk : r->end;
            continue;
        }
        int unmapped = 0;
        unit_lock(u);
        if (u->backing.reserved && u->backing.committed && u->live_pages &&
            !prot_watch_has_gpu(u->backing.address, PS5_MEM_UNIT)) {
            bool early = false;
            for (uint64_t p = u->backing.address; p < u->backing.address + PS5_MEM_UNIT; p += PS5_MEM_PAGE) {
                if (prot_watch_is_early(p)) { early = true; break; }
            }
            if (!early) {
                int result = ps5_mem_decommit(&u->backing);
                unmapped = !u->backing.committed;
                if (unmapped) {
                    __atomic_sub_fetch(&backed_units[r->kind], 1, __ATOMIC_RELAXED);
                    if (!u->backing.allocated) freed += PS5_MEM_UNIT;
                }
                if (result) sdk_ok("release backing", at, PS5_MEM_UNIT, result);
            }
        }
        unit_unlock(u);
        if (unmapped && discarded && !discarded(at, at + PS5_MEM_UNIT)) break;
        at += PS5_MEM_UNIT;
    }
    return freed;
}

// MKW_POOL_RELEASE (vk_memory.c): a finalized pool's whole units that hold only zeros. ps5_mem_commit zero-fills a unit
// it commits again (ps5_memory.c), so the game reads what it left there. Task 32: releasing every unit of a finalized
// pool discarded the character cards' bytes, which MKW keeps after the finalize and samples again (magenta bands).
uint64_t plat_mem_release_zero_backing(uint64_t addr, uint64_t size) {
    if (!size || size > UINT64_MAX - addr || addr > UINT64_MAX - (PS5_MEM_UNIT - 1)) return 0;
    uint64_t freed = 0;
    uint64_t lo = (addr + PS5_MEM_UNIT - 1) & ~(uint64_t)(PS5_MEM_UNIT - 1);
    uint64_t hi = (addr + size) & ~(uint64_t)(PS5_MEM_UNIT - 1);
    for (uint64_t at = lo; at < hi; at += PS5_MEM_UNIT) {
        VmRange *r = find_range_lockfree(at);
        if (!r || r->host) continue;
        Ps5MixedBlock *u = find_unit_lockfree(r, at);
        if (!u || !__atomic_load_n(&u->backing.committed, __ATOMIC_ACQUIRE)) continue;
        unit_lock(u);
        bool ok = u->backing.reserved && u->backing.committed && u->live_pages &&
                  !prot_watch_has_gpu(u->backing.address, PS5_MEM_UNIT);
        for (size_t p = 0; ok && p < PS5_MEM_UNIT / PAGE; ++p)   // readable and not early-watched: the scan reads it
            ok = u->pages[p] >= PS5_GUEST_READ && !prot_watch_is_early(u->backing.address + p * PAGE);
        const uint64_t *q = (const uint64_t *)(uintptr_t)u->backing.address;
        for (size_t k = 0; ok && k < PS5_MEM_UNIT / sizeof(uint64_t); ++k) ok = !q[k];
        if (ok) {
            int result = ps5_mem_decommit(&u->backing);
            if (!u->backing.committed) {
                __atomic_sub_fetch(&backed_units[r->kind], 1, __ATOMIC_RELAXED);
                if (!u->backing.allocated) freed += PS5_MEM_UNIT;
            }
            if (result) sdk_ok("release zero backing", at, PS5_MEM_UNIT, result);
        }
        unit_unlock(u);
    }
    return freed;
}

void plat_mem_back_causes(uint64_t out[PLAT_BACK_N]) {
    for (int i = 0; i < PLAT_BACK_N; i++)
        out[i] = (uint64_t)__atomic_load_n(&backed_by_cause[i], __ATOMIC_RELAXED) * PS5_MEM_UNIT;
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_install();
    __atomic_store_n(&g_fault_hook, fn, __ATOMIC_RELEASE);
}
void plat_mem_watch_hook(int (*fn)(uint64_t addr, uint64_t rip)) {
    fault_install();
    __atomic_store_n(&g_watch_hook, fn, __ATOMIC_RELEASE);
}
void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    fault_install();
    __atomic_store_n(&g_crash_hook, fn, __ATOMIC_RELEASE);
}

static bool unmap(uint64_t address, uint64_t end) {
    return address == end || sdk_ok("shutdown unmap", address, end - address,
                                    sceKernelMunmap((void *)(uintptr_t)address, end - address));
}

static bool release_ranges(VmRange *range) {
    bool ok = true;
    while (range) {
        VmRange *next = range->next;
        uint64_t at = range->base;
        if (!range->host && range->chunks) {
            for (size_t c = 0; c < range->chunk_count; ++c) {
                Ps5MixedBlock *chunk = range->chunks[c];
                if (!chunk) continue;
                for (size_t i = 0; i < CHUNK_UNITS; ++i) {
                    Ps5MemBlock *unit = &chunk[i].backing;
                    uint64_t address = unit->address;
                    if (!unit->reserved && !unit->committed && !address) break;
                    if (unit->committed) __atomic_sub_fetch(&backed_units[range->kind], 1, __ATOMIC_RELAXED);
                    ok &= unmap(at, address);
                    // A slot lost on decommit has reserved=false. Release only its
                    // remaining physical or relocated ownership, never the hole.
                    ok &= sdk_ok("shutdown unit", address, PS5_MEM_UNIT, ps5_mem_release(unit));
                    at = address + PS5_MEM_UNIT;
                }
                free(chunk);
            }
            free((void *)range->chunks);
        }
        ok &= unmap(at, range->end);
        free(range);
        range = next;
    }
    return ok;
}

bool ps5_guest_memory_shutdown(void) {
    pthread_mutex_lock(&vm_lock);
    if (stopped) {
        pthread_mutex_unlock(&vm_lock);
        return false;
    }
    stopped = true;
    bool ok = release_ranges(ranges);
    ok &= release_ranges(cleanup_pending);
    ranges = cleanup_pending = NULL;
    prot_watch_shutdown();
    pthread_mutex_unlock(&vm_lock);
    return ok;
}
