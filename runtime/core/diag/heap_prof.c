// C heap profile (MKW_HEAP_PROFILE=1, PS5 only): what the runtime's own allocations hold, by call site, so a long run
// can say where the title heap's direct memory goes. The Knockout Tour runs of 27/11/2026 found the title heap at
// ~1.2 GiB of direct memory from the race's start, growing 7-14 MiB/min (bench_mem.c's c_heap), with nothing saying
// what it is.
//
// - The SDK's title heap (ps5platform/heap.h: malloc and its family wrapped to dlmalloc mspaces in direct memory)
//   calls the functions ps5_heap_trace sets with every block it hands out and takes back (mihawk-99/PS5_PayloadSDK
//   00c7889d). Each live block is kept in a table of SHARDS shards under their own spinlock: pointer, size, site.
//   A block allocated before the profile starts (right after mem_init) is not in it: the report says how much of the
//   mapped heap the table accounts for.
// - The site: the runtime is built without frame pointers (build_game.py), so the caller is found as sigsample.c's
//   fallback finds return addresses: the first SITE_DEPTH words of the stack that point into the executable's code
//   (the code mapping's bounds, from the kernel). The first is usually the SDK's wrap, the next
//   the caller; a stale word can slip in, so each site prints all its addresses (tools/run/sym.py --ps5 names them).
// - The cost lands on every malloc and free of every thread (a stack scan and a locked table update): a diagnostic,
//   off by default and never in the PS5 profile. Its tables are plat_mem_alloc memory, outside the C heap it measures.
// - "heap: ..." every REPORT_S on its own thread: the table's total against the heap's mapped bytes, the top sites by
//   live bytes and by growth since the previous report, and live blocks by size class.
// With an SDK without ps5_heap_trace, the weak definition below stands in (the PS5 link refuses a weak reference) and
// the profile says it cannot run.
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "heap_prof.h"
#include "host.h"
#include "platform/platform.h"

#ifdef MKW_PS5_NATIVE
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>

typedef void (*heap_alloc_fn)(void *pointer, size_t bytes);
typedef void (*heap_free_fn)(void *pointer);
static volatile int trace_missing;
// The SDK's (00c7889d) when it has one; it is in heap_wrap.o, which the title links for __wrap_malloc anyway.
__attribute__((weak, noinline)) void ps5_heap_trace(heap_alloc_fn allocated, heap_free_fn freed) {
    (void)allocated, (void)freed;
    trace_missing = 1;
}

#define SHARDS 256
#define SHARD_SLOTS 8192            // 2M blocks in all, kept under 75 % full
#define SITES 8192
#define SITE_DEPTH 5                // code addresses kept per site
#define SCAN_WORDS 48               // stack words searched for them
#define REPORT_S 60
#define TOP 20

typedef struct { uintptr_t ptr; uint32_t size; uint16_t site; uint16_t pad; } Slot;   // ptr 0: empty
typedef struct { atomic_flag lock; uint32_t n; Slot *slot; } Shard;
typedef struct {
    uint64_t key;                   // 0: empty
    uintptr_t ra[SITE_DEPTH];
    _Atomic int64_t live_bytes, live_n;
    _Atomic uint64_t allocs;
    int64_t reported;               // live_bytes at the previous report (reporter thread only)
} Site;

static Shard shards[SHARDS];
static Site *sites;
static atomic_flag site_lock = ATOMIC_FLAG_INIT;
static uintptr_t text_lo, text_hi;
static _Atomic uint64_t n_full, n_untracked_free, n_site_full, n_big;
static _Atomic int64_t tracked_bytes, tracked_n;
static uint64_t size_hist_n[24];    // filled by the reporter from the table

static void lock(atomic_flag *f) {
    while (atomic_flag_test_and_set_explicit(f, memory_order_acquire)) __builtin_ia32_pause();
}
static void unlock(atomic_flag *f) { atomic_flag_clear_explicit(f, memory_order_release); }

static uint64_t mix(uint64_t x) {
    x ^= x >> 33, x *= 0xff51afd7ed558ccdull, x ^= x >> 33, x *= 0xc4ceb9fe1a85ec53ull, x ^= x >> 33;
    return x;
}

// The executable's code: the mapping that holds this function and the one at the load address, as the kernel reports
// them (sceKernelQueryMemoryProtection: bounds only, nothing read). Not its ELF header: the PS5 maps the code
// execute-only and reading the header killed the title with SYSTEM_XO_VIOLATION at eboot.bin+0x38 (koHP, 27/11/2026);
// the title's own link script defines no etext or __text_end either.
static void find_text(void) {
    void *start, *end;
    uint32_t prot;
    const void *probe[2] = {(const void *)(uintptr_t)find_text, (const void *)plat_exe_base()};
    for (int i = 0; i < 2; i++)
        if (!sceKernelQueryMemoryProtection((void *)(uintptr_t)probe[i], &start, &end, &prot)) {
            if (!text_lo || (uintptr_t)start < text_lo) text_lo = (uintptr_t)start;
            if ((uintptr_t)end > text_hi) text_hi = (uintptr_t)end;
        }
}

// from: on_alloc's own return address (into the SDK's wrap), so the scan skips the profile's frames; a host harness
// (27/11/2026) scanning from site_of's frame filled every slot with the profile's own addresses and merged all sites.
// The top of this thread's stack, once per thread: the end of the mapping that holds it (sceKernelQueryMemoryProtection,
// bounds only). The scan stops there: 48 words up from a shallow frame read past a thread's stack and killed the title
// (access violation in on_alloc, koHP2, 27/11/2026).
static _Thread_local uintptr_t stack_top;
static uintptr_t this_stack_top(const void *sp) {
    if (!stack_top) {
        void *start, *end;
        uint32_t prot;
        stack_top = sceKernelQueryMemoryProtection((void *)(uintptr_t)sp, &start, &end, &prot) ? (uintptr_t)sp + 8
                                                                                                  : (uintptr_t)end;
    }
    return stack_top;
}

static uint16_t site_of(const uintptr_t *from) {
    uintptr_t ra[SITE_DEPTH] = {0};
    int k = 0;
    uintptr_t top = this_stack_top(from);
    int words = top > (uintptr_t)from ? (int)((top - (uintptr_t)from) / sizeof *from) : 0;
    if (words > SCAN_WORDS) words = SCAN_WORDS;
    for (int i = 0; i < words && k < SITE_DEPTH; i++)
        if (from[i] >= text_lo && from[i] < text_hi && (!k || ra[k - 1] != from[i])) ra[k++] = from[i];
    uint64_t key = 1;
    for (int i = 0; i < SITE_DEPTH; i++) key = mix(key ^ ra[i]);
    key |= 1;
    for (uint32_t h = (uint32_t)key % SITES, probes = 0; probes < 64; h = (h + 1) % SITES, probes++) {
        Site *s = &sites[h];
        uint64_t have = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
        if (have == key) return (uint16_t)h;
        if (!have) {
            lock(&site_lock);
            if (!s->key) {
                memcpy(s->ra, ra, sizeof ra);
                __atomic_store_n(&s->key, key, __ATOMIC_RELEASE);
            }
            unlock(&site_lock);
            if (s->key == key) return (uint16_t)h;
        }
    }
    atomic_fetch_add_explicit(&n_site_full, 1, memory_order_relaxed);
    return 0;   // site 0 stands for "no room": its addresses are those of the first site made
}

__attribute__((noinline)) static void on_alloc(void *pointer, size_t bytes) {
    uint16_t site = site_of((const uintptr_t *)__builtin_frame_address(0) + 1);   // frame + 1: the return address
    uint32_t size = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
    if (bytes > UINT32_MAX) atomic_fetch_add_explicit(&n_big, 1, memory_order_relaxed);
    uint64_t h = mix((uintptr_t)pointer);
    Shard *sh = &shards[h % SHARDS];
    lock(&sh->lock);
    if (sh->n >= SHARD_SLOTS * 3 / 4) {
        unlock(&sh->lock);
        atomic_fetch_add_explicit(&n_full, 1, memory_order_relaxed);
        return;
    }
    uint32_t i = (uint32_t)(h >> 8) % SHARD_SLOTS;
    while (sh->slot[i].ptr) i = (i + 1) % SHARD_SLOTS;
    sh->slot[i] = (Slot){(uintptr_t)pointer, size, site, 0};
    sh->n++;
    unlock(&sh->lock);
    atomic_fetch_add_explicit(&sites[site].live_bytes, size, memory_order_relaxed);
    atomic_fetch_add_explicit(&sites[site].live_n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&sites[site].allocs, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&tracked_bytes, size, memory_order_relaxed);
    atomic_fetch_add_explicit(&tracked_n, 1, memory_order_relaxed);
}

static void on_free(void *pointer) {
    uint64_t h = mix((uintptr_t)pointer);
    Shard *sh = &shards[h % SHARDS];
    lock(&sh->lock);
    uint32_t i = (uint32_t)(h >> 8) % SHARD_SLOTS;
    while (sh->slot[i].ptr && sh->slot[i].ptr != (uintptr_t)pointer) i = (i + 1) % SHARD_SLOTS;
    if (!sh->slot[i].ptr) {
        unlock(&sh->lock);
        atomic_fetch_add_explicit(&n_untracked_free, 1, memory_order_relaxed);
        return;
    }
    Slot gone = sh->slot[i];
    // backward-shift deletion: linear probing stays valid without tombstones
    for (uint32_t j = (i + 1) % SHARD_SLOTS; sh->slot[j].ptr; j = (j + 1) % SHARD_SLOTS) {
        uint32_t home = (uint32_t)(mix(sh->slot[j].ptr) >> 8) % SHARD_SLOTS;
        if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j)) sh->slot[i] = sh->slot[j], i = j;
    }
    sh->slot[i].ptr = 0;
    sh->n--;
    unlock(&sh->lock);
    atomic_fetch_sub_explicit(&sites[gone.site].live_bytes, gone.size, memory_order_relaxed);
    atomic_fetch_sub_explicit(&sites[gone.site].live_n, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&tracked_bytes, gone.size, memory_order_relaxed);
    atomic_fetch_sub_explicit(&tracked_n, 1, memory_order_relaxed);
}

static int cmp_desc(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x < y) - (x > y);
}

static void log_site(const char *what, uint32_t h, int64_t bytes, int64_t delta) {
    const Site *s = &sites[h];
    char ra[200];
    int o = 0;
    for (int i = 0; i < SITE_DEPTH && s->ra[i] && o < (int)sizeof ra; i++)
        o += snprintf(ra + o, sizeof ra - o, " exe+%#lx", (unsigned long)(s->ra[i] - plat_exe_base()));
    rt_log("heap: %s site %u live %.1f MiB in %lld blocks (%+.1f MiB since the last report; %llu allocations):%s", what,
           h, bytes / 1048576.0, (long long)atomic_load(&s->live_n), delta / 1048576.0,
           (unsigned long long)atomic_load(&s->allocs), o ? ra : " (no code address found)");
}

static void report(int n_report) {
    struct ps5_heap_stats st = {0};
    ps5_heap_stats(&st);
    int64_t tb = atomic_load(&tracked_bytes), tn = atomic_load(&tracked_n);
    rt_log("heap: report %d: title heap maps %.1f MiB (peak %.1f, %u segments, %u arenas, %llu libc fallbacks); the "
           "profile tracks %.1f MiB in %lld live blocks (%llu not tracked: table full; %llu frees of blocks from before "
           "the profile; %llu over 4 GiB; %llu without a site)",
           n_report, st.mapped_bytes / 1048576.0, st.peak_bytes / 1048576.0, st.segments, st.arenas,
           st.libc_fallbacks, tb / 1048576.0, (long long)tn, (unsigned long long)atomic_load(&n_full),
           (unsigned long long)atomic_load(&n_untracked_free), (unsigned long long)atomic_load(&n_big),
           (unsigned long long)atomic_load(&n_site_full));
    // top sites by live bytes, then by growth since the last report: (value << 13 | site) sorted descending
    static int64_t key[SITES];
    int n = 0;
    for (uint32_t h = 0; h < SITES; h++)
        if (sites[h].key && atomic_load(&sites[h].live_bytes) > 0) key[n++] = atomic_load(&sites[h].live_bytes) << 13 | h;
    qsort(key, (size_t)n, sizeof *key, cmp_desc);
    for (int i = 0; i < n && i < TOP; i++) {
        uint32_t h = (uint32_t)(key[i] & (SITES - 1));
        int64_t b = atomic_load(&sites[h].live_bytes);
        log_site("top", h, b, b - sites[h].reported);
    }
    if (n_report > 1) {
        n = 0;
        for (uint32_t h = 0; h < SITES; h++) {
            int64_t d = sites[h].key ? atomic_load(&sites[h].live_bytes) - sites[h].reported : 0;
            if (d > (64 << 10)) key[n++] = d << 13 | h;
        }
        qsort(key, (size_t)n, sizeof *key, cmp_desc);
        for (int i = 0; i < n && i < TOP / 2; i++) {
            uint32_t h = (uint32_t)(key[i] & (SITES - 1));
            int64_t b = atomic_load(&sites[h].live_bytes);
            log_site("grew", h, b, b - sites[h].reported);
        }
    }
    for (uint32_t h = 0; h < SITES; h++)
        if (sites[h].key) sites[h].reported = atomic_load(&sites[h].live_bytes);
    // live blocks by size class (2^k bytes and up), read shard by shard under its lock
    memset(size_hist_n, 0, sizeof size_hist_n);
    uint64_t hist_b[24] = {0};
    for (int s = 0; s < SHARDS; s++) {
        lock(&shards[s].lock);
        for (uint32_t i = 0; i < SHARD_SLOTS; i++) {
            if (!shards[s].slot[i].ptr) continue;
            uint32_t z = shards[s].slot[i].size;
            int k = z ? 31 - __builtin_clz(z) : 0;
            k = k < 6 ? 0 : k - 6 > 23 ? 23 : k - 6;   // class 0: under 128 B, class k: 2^(k+6) B and up
            size_hist_n[k]++, hist_b[k] += z;
        }
        unlock(&shards[s].lock);
    }
    char line[640];
    int o = snprintf(line, sizeof line, "heap: live blocks by size (from, count, MiB):");
    for (int k = 0; k < 24 && o < (int)sizeof line; k++)
        if (size_hist_n[k])
            o += snprintf(line + o, sizeof line - o, " %s%llu:%llu/%.1f", k ? "" : "<", k ? 1ull << (k + 6) : 128ull,
                          (unsigned long long)size_hist_n[k], hist_b[k] / 1048576.0);
    rt_log("%s", line);
}

static void reporter(void *arg) {
    (void)arg;
    for (int n = 1;; n++) {
        plat_sleep_ms(REPORT_S * 1000);
        report(n);
    }
}

void heap_prof_init(void) {
    const char *v = cfg_get("MKW_HEAP_PROFILE");
    if (!v || atoi(v) <= 0) return;
    find_text();
    sites = plat_mem_alloc(sizeof *sites * SITES);
    int ok = sites && text_hi > text_lo;
    for (int s = 0; ok && s < SHARDS; s++) {
        atomic_flag_clear(&shards[s].lock);
        ok = (shards[s].slot = plat_mem_alloc(sizeof(Slot) * SHARD_SLOTS)) != NULL;
    }
    if (!ok) {
        rt_log("heap: MKW_HEAP_PROFILE: no memory for its tables or no code segment found: no profile");
        return;
    }
    // Written once now: plat_mem_alloc memory is backed at its first write, through the fault path, which must not
    // first run inside a malloc callback with a shard lock held.
    memset(sites, 0, sizeof *sites * SITES);
    for (int s = 0; s < SHARDS; s++) memset(shards[s].slot, 0, sizeof(Slot) * SHARD_SLOTS);
    sites[0].key = 1;   // the "no room" site
    trace_missing = 0;
    ps5_heap_trace(on_alloc, on_free);
    if (trace_missing) {
        rt_log("heap: MKW_HEAP_PROFILE: this SDK has no ps5_heap_trace (needs mihawk-99/PS5_PayloadSDK 00c7889d): no "
               "profile");
        return;
    }
    plat_thread_new(reporter, NULL, "heap-prof");
    rt_log("heap: MKW_HEAP_PROFILE on: every C heap block tracked by call site (code exe+%#lx-%#lx), a report every %d "
           "s; it slows every malloc and free", (unsigned long)(text_lo - plat_exe_base()),
           (unsigned long)(text_hi - plat_exe_base()), REPORT_S);
}
#else
void heap_prof_init(void) {
    const char *v = cfg_get("MKW_HEAP_PROFILE");
    if (v && atoi(v) > 0) rt_log("heap: MKW_HEAP_PROFILE is the PS5 title heap's (core/diag/heap_prof.c): ignored here");
}
#endif
