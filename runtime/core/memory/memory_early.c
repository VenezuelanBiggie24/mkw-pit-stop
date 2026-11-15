// MKW_EARLY_READ=1 (diagnosis): does the game read memory the GPU has yet to write? With the replay ~3 submits behind
// the game, what the GPU writes (copies into buffers, counters, cleared buffers, storage buffers a shader may write)
// reaches memory later than on the console, and the game decides what it draws with some of it (menus:
// trees and cacti dark for a frame). MKW_SUBMIT_CATCHUP makes every submit wait for its GPU work instead, at ~10 ms a
// frame. Here the destinations a submit lists (nvn_api_cmd.c resolve_deferred) are made inaccessible at the submit, with
// that submit's fence; the first access to such a page puts its protection back and is counted: early when the GPU has
// not finished the submit, late when it has, by a game thread or by the runtime. Nothing waits: it only measures where a
// wait would be needed. Linux and PS5 (read traps, memory.c). One summary line every 600 frames with the first early reads.
// MKW_EARLY_READ=2/3 wait, but only the game's own code (kern_fault_in_game): a guest thread at a lifted function's
// rip with no host call in progress, which holds no host lock. Runtime threads, and host code on guest threads, release
// the watch and go on: they order on the GPU's writes through the runtime's own fences. A wait for a fence the replay has
// not handed to the queue is bounded (ISSUE_WAIT_NS) and named in the hang watch (run 25 hung with waits by host code).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "hangwatch.h"
#include "host.h"
#include "kernel_int.h"
#include "memory_int.h"
#include "prof.h"

#define PAGE 0x1000ull
#define EW_KINDS 6
#define EW_FIRST 12
#define EW_CHUNK_SHIFT 24
#define EW_CHUNK_SIZE (1ull << EW_CHUNK_SHIFT)
#define EW_CHUNK_PAGES (EW_CHUNK_SIZE / PAGE)
#define EW_CHUNKS (1u << (39 - EW_CHUNK_SHIFT))

static const char *const kind_name[EW_KINDS] = {"counter", "copy", "texture to buffer", "clear", "storage buffer", "uniform update"};
int64_t mem_early_pages;
// MKW_EARLY_READ=3: per guest page, the last submit listed as writing it and the one before. On the console a read
// right after submit N finds N-1's results (the GPU has only begun N): the console-like wait is for the previous writer
// when it has not finished, not for N (=2 waited for N: ~1 wait a frame of 7-16 ms, ews5).
int mem_fence_early;
uint64_t mem_early_rel;
// Per guest page, the 64-byte pieces the two newest recorded writers write (fence, mask): which bytes an unfinished
// writer leaves pending, for mem_early_invalidate. wm_lock.
typedef struct { uint64_t f[2], m[2]; } WMask;
typedef struct {
    uint64_t meta[EW_CHUNK_PAGES];
    uint64_t gpu_wrote[EW_CHUNK_PAGES];
    uint64_t lastf[EW_CHUNK_PAGES];
    uint64_t prevf[EW_CHUNK_PAGES];
    WMask wmask[EW_CHUNK_PAGES];
} EarlyChunk;
static EarlyChunk *early_chunks[EW_CHUNKS];
static PlatMutex chunk_lock = PLAT_MUTEX_INIT;
static PlatMutex wm_lock = PLAT_MUTEX_INIT;
uint64_t (*mem_fence_done)(void), (*mem_fence_queued)(void);
static int (*fence_wait)(uint64_t value, uint64_t timeout_ns);
static int (*fence_issued_wait)(uint64_t value, uint64_t timeout_ns);   // value handed to the queue whole, at most that
// A wait for a fence the replay has not handed to the queue yet: at most this long for it to be (the replay is behind),
// then the read goes on without the GPU's bytes. The waiting game thread holds no host lock (kern_fault_in_game) and
// the replay needs no guest core, but a wait that depends on the replay's progress is bounded all the same.
#define ISSUE_WAIT_NS 50000000ull
static int64_t not_game, not_issued;   // since the last log line: touches by host code on guest threads (no wait); waits
                                       // given up because the fence was not handed to the queue in ISSUE_WAIT_NS
static volatile uint64_t hw_fence;   // the hang watch's line: the newest wait's fence
static volatile int hw_waiting;
static int wait_mode;                 // MKW_EARLY_READ=2: an early read of bytes the GPU writes waits for its submit
static int early_batch = -1;          // MKW_EARLY_BATCH (default 1): coalesce contiguous protect calls
static int early_wait_away = -1;      // MKW_EARLY_WAIT_AWAY (default 0): release virtual core while waiting on GPU
static int64_t waits, wait_ns;        // since the last log line
static int64_t range_skips;           // frz_lock: watches released by mem_early_invalidate since the last log line
// The wait the fault on this thread asked for, logged once it ends when it was long: which page, who read it and how
// far behind the GPU was (lwab1: ~6 waits of ~7.5 ms every 600 frames, each a one-frame hitch).
static _Thread_local struct { uint64_t page, pc, done, last, frame, addr, watched; int diff; } wq;
static int nwait_logged;
static int64_t cnt[EW_KINDS][4];   // frz_lock: early read by the game, early write by the game, late by the game, by
                                   // the runtime
static struct { uint64_t page, fence, done, frame, pc; int kind, write; } first[EW_FIRST];
static int nfirst;
uint64_t mem_early_frame;   // the last frame mem_early_log saw
// A bound storage buffer is listed whether or not its shader writes it. What the GPU does write the replay reports
// later (mem_early_gpu_wrote, from vk_mem_gpu_pending), so the game's early reads are kept and told apart at the log:
// of a page the GPU writes (a real early read) or of one it only reads (no wait needed).
#define EW_RING (1u << 16)
static struct { uint64_t addr, pc; } ring[EW_RING];   // the byte read: told against the pieces the GPU writes
static uint32_t nring;
static struct { uint64_t pc, n; } rt_pc[8];   // frz_lock: where the runtime's own touches come from (a fault each)

static EarlyChunk *early_chunk(uint64_t a, int create) {
    if (a >= RT_ADDRESS_LIMIT) return NULL;
    size_t cidx = (size_t)(a >> EW_CHUNK_SHIFT);
    EarlyChunk *c = __atomic_load_n(&early_chunks[cidx], __ATOMIC_ACQUIRE);
    if (c || !create) return c;
    plat_mutex_lock(&chunk_lock);
    c = early_chunks[cidx];
    if (!c) {
        c = calloc(1, sizeof *c);
        if (c) __atomic_store_n(&early_chunks[cidx], c, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&chunk_lock);
    return c;
}

static inline size_t early_idx(uint64_t a) { return (size_t)((a & (EW_CHUNK_SIZE - 1)) / PAGE); }

void mem_early_page_fences(uint64_t addr, uint64_t *lastf, uint64_t *prevf) {
    EarlyChunk *c = early_chunk(addr, 0);
    if (!c) { *lastf = *prevf = 0; return; }
    size_t idx = early_idx(addr);
    *lastf = c->lastf[idx];
    *prevf = c->prevf[idx];
}

// MKW_EARLY_READ: 3 by default on Linux and PS5, where with MKW_SHADOW_CPU_WINS it stands for MKW_SUBMIT_CATCHUP;
// off on Windows, which has no read traps (win32.c).
static int early_cfg(void) {
    const char *e = cfg_get("MKW_EARLY_READ");
#ifndef _WIN32
    if (!e) e = "3";
#endif
    return e ? atoi(e) : 0;
}

int mem_early_wanted(void) { return early_cfg(); }

// The hang watch's line (hangwatch.c): a game thread waiting in the early-read handler, and for which fence.
static void hang_line(char *b, int n) {
    int w = __atomic_load_n(&hw_waiting, __ATOMIC_RELAXED);
    uint64_t f = hw_fence;
    if (w) snprintf(b, n, "early-read: %d game threads waiting, the newest for fence %llu (%s the queue; GPU done %llu)",
                    w, (unsigned long long)f, fence_issued_wait && fence_issued_wait(f, 0) ? "handed to" : "not yet in",
                    (unsigned long long)(mem_fence_done ? mem_fence_done() : 0));
    else b[0] = 0;
}

int mem_early_on(void) {
    static int on = -1;
    if (on < 0) {
        on = early_cfg() && mem_trapped_init();
        wait_mode = on ? early_cfg() - 1 : 0;   // 1 for N, 2 for the previous writer
        if (wait_mode < 0 || wait_mode > 2) wait_mode = 0;
        if (on) {
            hang_reporter(hang_line);
            prof_names_init();   // the wait line names the reading function: the table is built here, not in the fault
        }
        if (on) rt_log("mem: early-read watch on (MKW_EARLY_READ=%d, %zu KiB): what a submit's GPU work writes is watched until touched%s",
                       wait_mode + 1, plat_mem_early_page_size() >> 10,
                       wait_mode == 1 ? "; an early read of bytes the GPU writes waits for that submit"
                                      : wait_mode == 2 ? "; an early read waits for the previous submit writing them" : "");
    }
    return on;
}

// The runtime's reads of guest memory that may hold watched pages (the shadow sync, the uniform snapshot): a watched page
// is read through its protection, so the watch stays for the game (ews4: 96 641 faults by shadow_copy in 600 frames,
// each lifting a watch the game's early read would then pass).
void mem_copy_out(void *dst, uint64_t a, uint64_t n) {
    if (mem_early_on() <= 0 || !watched(a, n)) {
        memcpy(dst, (const void *)(uintptr_t)a, n);
        return;
    }
    for (uint64_t x = a, y; x < a + n; x = y) {
        int w = __atomic_load_n(&trapped[x / PAGE], __ATOMIC_RELAXED) == 2;
        for (y = (x & ~(PAGE - 1)) + PAGE; y < a + n && (__atomic_load_n(&trapped[y / PAGE], __ATOMIC_RELAXED) == 2) == w; y += PAGE) {}
        if (y > a + n) y = a + n;
        uint8_t *d = (uint8_t *)dst + (x - a);
        if (!w) memcpy(d, (const void *)(uintptr_t)x, y - x);
        else if (!plat_mem_peek(d, x, y - x)) memcpy(d, (const void *)(uintptr_t)x, y - x);   // falls back to the fault
    }
}

void mem_early_set_done(uint64_t (*done)(void), int (*wait)(uint64_t, uint64_t), uint64_t (*queued)(void),
                        int (*issued_wait)(uint64_t, uint64_t)) {
    mem_fence_done = done, fence_wait = wait, mem_fence_queued = queued, fence_issued_wait = issued_wait;
}

// The fault's wait, after frz_lock is released (the GPU's bytes come back under it, on the completion thread, before
// the fence counts as done): the read is then retried and finds them, or a read trap that fills it.
void mem_early_wait(uint64_t fence) {
    static uint8_t before[PAGE], after[PAGE];   // MKW_EARLY_READ_DRY=3: the page without and with the wait
    static PlatMutex diff_lock = PLAT_MUTEX_INIT;
    int diff = wq.diff && plat_mutex_trylock(&diff_lock);
    if (diff) plat_mem_peek(before, wq.page, PAGE);
    uint64_t t0 = plat_now_ns();
    // Only the game's own code reaches here (kern_fault_in_game), holding no host lock. A fence the replay has not
    // handed to the queue yet is waited for ISSUE_WAIT_NS at most: past that the read goes on without the wait.
    if (early_wait_away < 0) early_wait_away = rt_knob("EARLY_WAIT_AWAY", "0");
    int away = RT_KNOB_ON(early_wait_away) ? kern_core_away() : 0;
    __atomic_add_fetch(&hw_waiting, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&hw_fence, fence, __ATOMIC_RELAXED);
    const char *prev = hang_mark_val("early-read wait: the replay to hand this fence to the queue", fence);
    int issued = !fence_issued_wait || fence_issued_wait(fence, ISSUE_WAIT_NS);
    if (issued && fence_wait) {
        hang_mark_val("early-read wait: the GPU to finish this fence", fence);
        fence_wait(fence, 500000000ull);
    } else if (!issued) {
        __atomic_add_fetch(&not_issued, 1, __ATOMIC_RELAXED);
    }
    hang_mark(prev);
    __atomic_sub_fetch(&hw_waiting, 1, __ATOMIC_RELAXED);
    kern_core_back(&away);
    uint64_t ns = plat_now_ns() - t0;
    if (diff) {   // the page as the game reads it now: a trap left by the write-back is filled through the fault
        mem_copy_out(after, wq.page, PAGE);
        int nd = 0, lo = -1, hi = -1;
        for (int i = 0; i < (int)PAGE; i++)
            if (before[i] != after[i]) nd++, hi = i, lo = lo < 0 ? i : lo;
        int o = (int)(wq.addr & (PAGE - 1) & ~15ull);
        rt_log("mem: early-read wait at frame %llu changed %d bytes of page %#llx (offsets %#x-%#x); at %#llx without "
               "the wait %02x%02x%02x%02x %02x%02x%02x%02x, with it %02x%02x%02x%02x %02x%02x%02x%02x",
               (unsigned long long)wq.frame, nd, (unsigned long long)wq.page, lo, hi, (unsigned long long)(wq.page + o),
               before[o], before[o + 1], before[o + 2], before[o + 3], before[o + 4], before[o + 5], before[o + 6],
               before[o + 7], after[o], after[o + 1], after[o + 2], after[o + 3], after[o + 4], after[o + 5],
               after[o + 6], after[o + 7]);
        plat_mutex_unlock(&diff_lock);
    }
    __atomic_add_fetch(&waits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wait_ns, (int64_t)ns, __ATOMIC_RELAXED);
    if (ns > 1000000 && __atomic_fetch_add(&nwait_logged, 1, __ATOMIC_RELAXED) < 60) {   // the close criterion: 1 ms
        char who[48] = "?", code[192];
        plat_sample_self_name(who, sizeof who);
        prof_pc_describe(wq.pc, code, sizeof code);
        rt_log("mem: early-read wait %.1f ms at frame %llu: address %#llx (watched page %#llx) for fence %llu%s (GPU at "
               "%llu, the page's newest writer %llu), by %s thread %#x in %s", (double)ns / 1e6,
               (unsigned long long)wq.frame, (unsigned long long)wq.addr, (unsigned long long)wq.watched,
               (unsigned long long)fence, issued ? "" : ", not handed to the queue in time: not waited",
               (unsigned long long)wq.done, (unsigned long long)wq.last, who, tcur ? tcur->handle : 0, code);
    }
}

// The 64-byte pieces of page p that [a, end) covers, a bit each.
static uint64_t page_bits(uint64_t p, uint64_t a, uint64_t end) {
    uint64_t lo = p > a ? p : a, hi = p + PAGE < end ? p + PAGE : end;
    unsigned first = (unsigned)(lo % PAGE / 64), last = (unsigned)((hi - 1) % PAGE / 64);
    return (~0ull >> (63 - last)) & (~0ull << first);
}

// One atomic a page, and none for a page already all marked: one a 64-byte piece cost the replay thread ~3 % of its
// samples (def3, 08/11/2026: the 1-4 MiB ranges the shadow binds are 16-64 K pieces each).
void mem_early_gpu_wrote(uint64_t a, uint64_t n, uint64_t fence) {
    if (!n || mem_early_on() <= 0 || a >= RT_ADDRESS_LIMIT) return;
    uint64_t end = a + n < RT_ADDRESS_LIMIT ? a + n : RT_ADDRESS_LIMIT;
    for (uint64_t p = a & ~(PAGE - 1); p < end; p += PAGE) {
        EarlyChunk *c = early_chunk(p, 1);
        if (!c) return;
        size_t idx = early_idx(p);
        uint64_t bits = page_bits(p, a, end);
        if ((__atomic_load_n(&c->gpu_wrote[idx], __ATOMIC_RELAXED) & bits) != bits)
            __atomic_fetch_or(&c->gpu_wrote[idx], bits, __ATOMIC_RELAXED);
    }
    if (!fence) return;
    plat_mutex_lock(&wm_lock);
    for (uint64_t p = a & ~(PAGE - 1); p < end; p += PAGE) {
        EarlyChunk *c = early_chunk(p, 0);
        if (!c) continue;
        WMask *w = &c->wmask[early_idx(p)];
        uint64_t bits = page_bits(p, a, end);
        if (w->f[0] != fence) {
            if (fence < w->f[0]) continue;   // recorded out of order: left unknown
            w->f[1] = w->f[0], w->m[1] = w->m[0], w->f[0] = fence, w->m[0] = 0;
        }
        w->m[0] |= bits;
    }
    plat_mutex_unlock(&wm_lock);
}

static int arm_watch(uint64_t lo, size_t n) {
    PlatEarlyResult result = plat_mem_early_protect(lo, n, 1, 0);
    if (result == PLAT_EARLY_UNRESTORED)
        rt_fatal("memory: early-read watch: rollback failed at %#llx (%zu bytes)", (unsigned long long)lo, n);
    return result == PLAT_EARLY_OK;
}

static void flush_watch_run(uint64_t *lo, uint64_t *hi, size_t ep) {
    if (*lo >= *hi) return;
    if (!arm_watch(*lo, (size_t)(*hi - *lo))) {
        for (uint64_t np = *lo; np < *hi; np += ep) {
            if (!arm_watch(np, ep)) {
                for (uint64_t q = np; q < np + ep && q < RT_ADDRESS_LIMIT; q += PAGE)
                    if (trapped[q / PAGE] == 2) {
                        trapped[q / PAGE] = 0;
                        mem_early_pages--;
                    }
            }
        }
    }
    *lo = *hi = 0;
}

static void flush_unprotect_run(uint64_t *lo, uint64_t *hi, int *prot) {
    if (*lo >= *hi) return;
    if (plat_mem_early_protect(*lo, (size_t)(*hi - *lo), 0, *prot) != PLAT_EARLY_OK)
        rt_fatal("memory: early-read watch: page %#llx kept no access", (unsigned long long)*lo);
    for (uint64_t p = *lo; p < *hi && p < RT_ADDRESS_LIMIT; p += PAGE)
        if (trapped[p / PAGE] == 2) {
            trapped[p / PAGE] = 0;
            mem_early_pages--;
        }
    *lo = *hi = 0;
    *prot = 0;
}

void mem_early_invalidate(uint64_t a, uint64_t n) {
    static int on = -1;
    if (on < 0) on = rt_knob("EARLY_READ_RANGE", "1");
    if (early_batch < 0) early_batch = rt_knob("EARLY_BATCH", "1");
    // (MKW_FENCE_EARLY: the writer a read waits for may be the newest one, whose bytes this rule does not look at)
    if (!n || wait_mode != 2 || !RT_KNOB_ON(on) || !watched(a, n) || !mem_fence_done || mem_fence_early) return;
    uint64_t done = mem_fence_done();
    size_t ep = plat_mem_early_page_size();
    int batch = RT_KNOB_ON(early_batch);
    uint64_t rel_lo = 0, rel_hi = 0;
    int rel_prot = 0;
    frz_take();
    for (uint64_t p = a & ~(PAGE - 1); p < a + n; p += PAGE) {
        EarlyChunk *c = early_chunk(p, 0);
        if (!c || trapped[p / PAGE] != 2) continue;
        size_t idx = early_idx(p);
        uint64_t f = c->prevf[idx];
        if (f <= done) continue;
        uint64_t lo = p > a ? p : a, hi = p + PAGE < a + n ? p + PAGE : a + n, want = 0;
        for (uint64_t x = lo & ~63ull; x < hi; x += 64) want |= 1ull << (x % PAGE / 64);
        plat_mutex_lock(&wm_lock);
        const WMask *w = &c->wmask[idx];
        int known = w->f[0] == f || w->f[1] == f;
        uint64_t m = w->f[0] == f ? w->m[0] : w->m[1];
        plat_mutex_unlock(&wm_lock);
        if (!known || (m & want)) continue;
        if (ep > PAGE) {
            // Do not drop native protection if another 4 KiB page in the same native page still needs it.
            uint64_t np = p & ~(uint64_t)(ep - 1);
            int other_live = 0;
            for (uint64_t q = np; q < np + ep && q < RT_ADDRESS_LIMIT; q += PAGE) {
                if (q != p && trapped[q / PAGE] == 2) { other_live = 1; break; }
            }
            if (other_live) {
                trapped[p / PAGE] = 0;
                mem_early_pages--;
                range_skips++;
                continue;
            }
        }
        if (!batch) {
            mem_early_release_locked(p, 0);
        } else {
            uint64_t np = p & ~(uint64_t)(ep - 1);
            int rp = (int)(c->meta[idx] & 0xf);
            if (rel_lo && (np != rel_hi || (ep == PAGE && rp != rel_prot)))
                flush_unprotect_run(&rel_lo, &rel_hi, &rel_prot);
            if (!rel_lo) {
                rel_lo = np;
                rel_hi = np + ep;
                rel_prot = rp;
            } else {
                rel_hi += ep;
            }
        }
        range_skips++;
    }
    flush_unprotect_run(&rel_lo, &rel_hi, &rel_prot);
    frz_drop();
}

void mem_early_watch(uint64_t a, uint64_t n, uint64_t fence, int kind) {
    if (!n || !mem_early_on() || !watched(a, n)) return;
    static int skip = -1;   // MKW_EARLY_READ_SKIP (default 1; "ab"): the rule below
    if (skip < 0) skip = rt_knob("EARLY_READ_SKIP", "1");
    if (early_batch < 0) early_batch = rt_knob("EARLY_BATCH", "1");
    uint64_t done = wait_mode == 2 && mem_fence_done && RT_KNOB_ON(skip) ? mem_fence_done() : 0;
    size_t ep = plat_mem_early_page_size();
    int batch = RT_KNOB_ON(early_batch);
    uint64_t w_lo = 0, w_hi = 0;
    frz_take();
    for (uint64_t p = a & ~(PAGE - 1); p < a + n; p += PAGE) {
        EarlyChunk *c = early_chunk(p, 1);
        if (!c) continue;
        size_t idx = early_idx(p);
        if (c->lastf[idx] != fence) c->prevf[idx] = c->lastf[idx], c->lastf[idx] = fence;
        // =3 waits only for the previous writer: with it finished, no read of the page can wait, and a watch would only
        // cost faults (cwb3: ~37,000 touches and 12 waits every 600 frames; the kernel went from 13 to 16-19 % of the
        // game thread).
        // (MKW_FENCE_EARLY: this fence's own bytes may be read after its early release)
        if (done && c->prevf[idx] <= done && !mem_fence_early) continue;
        uint8_t t = trapped[p / PAGE];
        if (t == 1) continue;   // the GPU's bytes of an earlier fence are still pending: a read trap already
        if (t == 3) mem_base_fill_locked(p), t = 0;   // a base's page: its bytes go to the base, then it is watched
        // With waits, only pages the GPU has written in earlier frames (a frame repeats the last one): a bound storage
        // buffer the shader only reads costs a fault per touch, by the game and by the shadow sync (ews1: ~270 a frame).
        if (wait_mode && !c->gpu_wrote[idx]) continue;
        if (t == 2) {           // watched already: the newer submit's fence
            c->meta[idx] = fence << 8 | (c->meta[idx] & 0xff);
            continue;
        }
        uint64_t end;
        int st = plat_mem_state(p, p + PAGE, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) continue;   // a hole or a guest protection
        uint64_t np = p & ~(uint64_t)(ep - 1);
        int already_np = 0;
        if (ep > PAGE) {
            for (uint64_t q = np; q < np + ep && q < RT_ADDRESS_LIMIT; q += PAGE)
                if (trapped[q / PAGE] == 2) { already_np = 1; break; }
        }
        trapped[p / PAGE] = 2;
        c->meta[idx] = fence << 8 | (uint64_t)kind << 4 | (uint64_t)st;
        mem_early_pages++;
        if (!already_np) {
            if (!batch) {
                if (!arm_watch(np, ep)) {
                    trapped[p / PAGE] = 0;
                    mem_early_pages--;
                    continue;
                }
            } else {
                if (w_lo && np != w_hi) flush_watch_run(&w_lo, &w_hi, ep);
                if (!w_lo) {
                    w_lo = np;
                    w_hi = np + ep;
                } else {
                    w_hi += ep;
                }
            }
        }
    }
    flush_watch_run(&w_lo, &w_hi, ep);
    frz_drop();
}

static uint64_t siblings_released;   // MKW_EARLY_SIBLING_WAIT=0: sibling-page watches given up without a wait

static uint64_t early_release_page_locked(uint64_t p, uint64_t access_a, int access, int is_fault_page,
                                          int do_protect, size_t ep) {
    EarlyChunk *c = early_chunk(p, 0);
    if (!c) {
        trapped[p / PAGE] = 0;
        mem_early_pages--;
        return 0;
    }
    size_t idx = early_idx(p);
    uint64_t m = c->meta[idx], fence = m >> 8;
    int kind = (int)(m >> 4 & 0xf);
    if (do_protect) {
        uint64_t np = p & ~(uint64_t)(ep - 1);
        if (plat_mem_early_protect(np, ep, 0, (int)(m & 0xf)) != PLAT_EARLY_OK)
            rt_fatal("memory: early-read watch: page %#llx kept no access", (unsigned long long)np);
    }
    trapped[p / PAGE] = 0;
    mem_early_pages--;
    if (!access || kind >= EW_KINDS) return 0;
    uint64_t done = mem_fence_done ? mem_fence_done() : 0;
    // The game's own code (kern_fault_in_game) is counted as the game; a guest thread in host code as the runtime.
    int game = tcur && kern_fault_in_game(plat_fault_pc);
    int early = fence > done, who = !game ? 3 : !early ? 2 : plat_fault_write;
    if (is_fault_page && tcur && !game) not_game++;
    if (is_fault_page) {
        cnt[kind][who]++;
        if (early && game && !plat_fault_write && nring < EW_RING) ring[nring].addr = access_a, ring[nring++].pc = plat_fault_pc;
    }
    uint64_t at = plat_fault_pc - plat_exe_base() < (1ull << 32) ? plat_fault_pc : plat_fault_ret;
    if (is_fault_page && !game)
        for (int j = 0; j < 8; j++)
            if (!rt_pc[j].n || rt_pc[j].pc == at) {
                rt_pc[j].pc = at, rt_pc[j].n++;
                break;
            }
    if (is_fault_page && early && game && !plat_fault_write && nfirst < EW_FIRST) {
        first[nfirst].page = p, first[nfirst].fence = fence, first[nfirst].done = done, first[nfirst].kind = kind;
        first[nfirst].frame = mem_early_frame, first[nfirst].pc = plat_fault_pc, first[nfirst].write = 0;
        nfirst++;
    }
    uint64_t gw = c->gpu_wrote[idx];
    int hit = is_fault_page ? (int)(gw >> (access_a % PAGE / 64) & 1) : (gw != 0);
    // MKW_EARLY_SIBLING_WAIT (default 1, "ab" to compare; 0: off): an access to another 4 KiB page of the watched page's
    // 16 KiB native page waits for the watched page's writer too. On the PS5 every wait of a race was such a sibling
    // (15/11/2026, wake-20261115-185222: address 0x251ee85252, watched page 0x251ee87000), ~1 a frame of ~8 ms on the
    // game's job threads (4.5-4.8 s a 600-frame window), while "storage buffer 0 early reads 0": no game read touched
    // bytes the GPU had yet to write. Off, the sibling's watch is given up without a wait (siblings_released counts
    // them): a later early read of that page would no longer be caught.
    static int sib = -1;
    if (sib < 0) sib = rt_knob("EARLY_SIBLING_WAIT", "1");
    if (!is_fault_page && hit && !RT_KNOB_ON(sib)) hit = 0, siblings_released++;
    // Only the game's own code waits (kern_fault_in_game): a runtime thread, or host code on a guest thread, releases the
    // watch and goes on. With 16 KiB native pages the wait may come from another 4 KiB page of the faulting one
    // (is_fault_page 0): the line still names the fault (run 25 logged page 0, frame 0, exe-0x400000 from that case).
    if (wait_mode && early && game && !plat_fault_write && hit) {
        uint64_t w = wait_mode == 1 ? fence : c->prevf[idx] > done ? c->prevf[idx] : 0;
        uint64_t last = c->lastf[idx];
        if (wait_mode == 2 && mem_fence_early && last > done && last <= __atomic_load_n(&mem_early_rel, __ATOMIC_ACQUIRE))
            w = last;
        if (w) wq.page = access_a & ~(PAGE - 1), wq.watched = p, wq.pc = plat_fault_pc, wq.done = done, wq.last = last,
               wq.frame = mem_early_frame, wq.addr = access_a, wq.diff = 0;
        if (is_fault_page) {
            wq.page = p, wq.watched = p, wq.pc = plat_fault_pc, wq.done = done, wq.last = last, wq.frame = mem_early_frame;
            static int dry = -1;
            if (dry < 0) {
                const char *e = cfg_get("MKW_EARLY_READ_DRY");
                dry = e && !strcmp(e, "diff") ? 3 : rt_knob("EARLY_READ_DRY", "0");
            }
            if (w && dry < 3 && RT_KNOB_ON(dry)) {
                mem_early_ev_note(0, access_a, w, done, at);
                return 0;
            }
            wq.addr = access_a, wq.diff = dry == 3;
        }
        return w;
    }
    return 0;
}

uint64_t mem_early_release_locked(uint64_t a, int access) {
    size_t ep = plat_mem_early_page_size();
    uint64_t p = a & ~(PAGE - 1);
    if (ep == PAGE) return early_release_page_locked(p, a, access, 1, 1, PAGE);
    uint64_t np = a & ~(uint64_t)(ep - 1);
    uint64_t max_w = 0;
    int did_protect = 0;
    for (uint64_t q = np; q < np + ep && q < RT_ADDRESS_LIMIT; q += PAGE) {
        if (trapped[q / PAGE] != 2) continue;
        uint64_t w = early_release_page_locked(q, a, access, q == p, !did_protect, ep);
        did_protect = 1;
        if (w > max_w) max_w = w;
    }
    return max_w;
}

void mem_early_resolve_hold(uint64_t a, size_t n) {
    if (!n || mem_early_on() <= 0 || !__atomic_load_n(&mem_early_pages, __ATOMIC_RELAXED) || !watched(a, n))
        return;
    if (early_batch < 0) early_batch = rt_knob("EARLY_BATCH", "1");
    int batch = RT_KNOB_ON(early_batch);
    size_t ep = plat_mem_early_page_size();
    uint64_t lo = a & ~(uint64_t)(ep - 1);
    uint64_t hi = n > UINT64_MAX - a ? RT_ADDRESS_LIMIT : (a + n + ep - 1) & ~(uint64_t)(ep - 1);
    if (hi > RT_ADDRESS_LIMIT) hi = RT_ADDRESS_LIMIT;
    uint64_t rel_lo = 0, rel_hi = 0;
    int rel_prot = 0;
    frz_take();
    for (uint64_t p = lo; p < hi && mem_early_pages; p += ep) {
        int has = 0, rp = 0;
        for (uint64_t q = p; q < p + ep && q < RT_ADDRESS_LIMIT; q += PAGE)
            if (trapped[q / PAGE] == 2) {
                if (!has) {
                    EarlyChunk *c = early_chunk(q, 0);
                    rp = c ? (int)(c->meta[early_idx(q)] & 0xf) : 0;
                    has = 1;
                }
                break;
            }
        if (!has) continue;
        if (!batch) {
            mem_early_release_locked(p, 0);
            continue;
        }
        if (rel_lo && (p != rel_hi || (ep == PAGE && rp != rel_prot)))
            flush_unprotect_run(&rel_lo, &rel_hi, &rel_prot);
        if (!rel_lo) {
            rel_lo = p;
            rel_hi = p + ep;
            rel_prot = rp;
        } else {
            rel_hi += ep;
        }
    }
    flush_unprotect_run(&rel_lo, &rel_hi, &rel_prot);
    frz_drop();
}

void mem_early_log(uint64_t frame) {   // every frame
    mem_early_frame = frame;
    mem_early_ev_log();
    if (mem_early_on() <= 0 || frame % 600) return;
    char b[1024];
    int k = 0;
    frz_take();
    for (int i = 0; i < EW_KINDS && k < (int)sizeof b - 96; i++)
        if (cnt[i][0] || cnt[i][1] || cnt[i][2] || cnt[i][3])
            k += snprintf(b + k, sizeof b - (size_t)k, "; %s %lld early reads %lld early writes %lld late %lld runtime",
                          kind_name[i], (long long)cnt[i][0], (long long)cnt[i][1], (long long)cnt[i][2], (long long)cnt[i][3]);
    char ev[160];
    mem_early_ev_counts(ev, sizeof ev);
    rt_log("mem: early reads at frame %llu (%lld pages watched now)%s; waits %lld, %.1f ms in all, %lld given up (fence "
           "not handed to the queue in %llu ms); touches by host code on game threads %lld (no wait); released by an "
           "invalidate of other bytes %lld; sibling-page watches given up without a wait %llu; %s",
           (unsigned long long)frame, (long long)mem_early_pages, k ? b : ": none", (long long)waits,
           (double)wait_ns / 1e6, (long long)not_issued, (unsigned long long)(ISSUE_WAIT_NS / 1000000),
           (long long)not_game, (long long)range_skips, (unsigned long long)siblings_released, ev);
    waits = wait_ns = range_skips = not_game = not_issued = 0;
    siblings_released = 0;
    // The early reads by whether the GPU writes the page (as far as the replay has reported), with the code making them.
    uint32_t wrote = 0, page_only = 0;
    uint64_t pcs[8] = {0}, pcn[8] = {0};
    for (uint32_t i = 0; i < nring; i++) {
        EarlyChunk *c = early_chunk(ring[i].addr, 0);
        uint64_t m = c ? c->gpu_wrote[early_idx(ring[i].addr)] : 0;
        if (!(m >> (ring[i].addr % PAGE / 64) & 1)) {
            page_only += m != 0;
            continue;
        }
        wrote++;
        for (int j = 0; j < 8; j++)
            if (!pcn[j] || pcs[j] == ring[i].pc) {
                pcs[j] = ring[i].pc, pcn[j]++;
                break;
            }
    }
    char w[512];
    int o = 0;
    for (int j = 0; j < 8 && pcn[j] && o < (int)sizeof w - 48; j++)
        o += snprintf(w + o, sizeof w - (size_t)o, " exe+%#llx x%llu", (unsigned long long)(pcs[j] - plat_exe_base()),
                      (unsigned long long)pcn[j]);
    rt_log("mem: early reads of bytes the GPU writes: %u of %u (%u more of other bytes in pages it writes)%s%s", wrote,
           nring, page_only, wrote ? ", by" : "", o ? w : "");
    nring = 0;
    o = 0;
    for (int j = 0; j < 8 && rt_pc[j].n && o < (int)sizeof w - 48; j++)
        o += snprintf(w + o, sizeof w - (size_t)o, " exe+%#llx x%llu", (unsigned long long)(rt_pc[j].pc - plat_exe_base()),
                      (unsigned long long)rt_pc[j].n);
    if (o) rt_log("mem: early-read watch: the runtime's touches by%s", w);
    memset(rt_pc, 0, sizeof rt_pc);
    for (int i = 0; i < nfirst; i++)
        rt_log("mem: early read: page %#llx (%s) at frame %llu, fence %llu, GPU at %llu, by exe+%#llx", (unsigned long long)first[i].page,
               kind_name[first[i].kind], (unsigned long long)first[i].frame, (unsigned long long)first[i].fence,
               (unsigned long long)first[i].done, (unsigned long long)(first[i].pc - plat_exe_base()));
    memset(cnt, 0, sizeof cnt);
    nfirst = 0;
    frz_drop();
}

int mem_early_waits(void) { return mem_early_on() > 0 && wait_mode; }
