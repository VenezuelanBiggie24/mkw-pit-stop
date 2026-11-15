// Page-protection write watch shared by PS5 (16 KiB native pages) and Linux (MKW_PROT_WATCH=1, 4 KiB pages).
// Tracks committed RW pages in watched reservations using a 2-level 16 MiB chunk directory with atomic per-page
// state words so fault handling is lock-free and async-signal-safe.
//
// Hold ordering (kernel writes vs. reset):
//   Kernel writes (pread/recv) and GPU imports cannot fault on PROT_READ pages and fail with EFAULT instead.
//   Before the kernel write, prot_watch_hold() waits for any concurrent reset's mprotect(READ) on that page
//   (PW_PROTECTING) to finish, atomically publishes PW_HOLD_ONE | PW_WRITTEN (clearing PW_ARMED) with SEQ_CST,
//   and then ensures the page is RW. While PW_HOLD_MASK != 0, prot_watch_written(reset=1) never arms the page
//   and leaves PW_WRITTEN set. prot_watch_written(reset=1) also sets PW_ARMED | PW_PROTECTING before calling
//   protect(READ), then rechecks PW_HOLD_MASK after protect(READ) and unprotects if a hold appeared.
//   Thus a kernel write between hold() and release() can never observe a page made PROT_READ by the watch.
#include "prot_watch.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__PROSPERO__)

enum {
    PW_GUEST_PAGE = 4096,
    PW_CHUNK_SHIFT = 24, // 16 MiB per chunk
    PW_CHUNK_SIZE = 1u << PW_CHUNK_SHIFT,
    PW_LIMIT_SHIFT = 39,
    PW_CHUNKS = 1u << (PW_LIMIT_SHIFT - PW_CHUNK_SHIFT), // 32768
    PW_MAX_RANGES = 16,
    PW_WRITTEN = 1u << 0,
    PW_ARMED = 1u << 1,
    PW_PROTECTING = 1u << 2,
    PW_EARLY = 1u << 3,
    PW_REP_SHIFT = 4,   // consecutive arm cycles the page was written in (0-15): a page the game writes every cycle
    PW_REP_ONE = 1u << PW_REP_SHIFT,
    PW_REP_MASK = 0xfu << PW_REP_SHIFT,
    PW_REP_HOT = 15,   // a second fault in a row sets the count here: the queries left before it is armed again
    PW_HOLD_SHIFT = 8,
    PW_HOLD_ONE = 1u << PW_HOLD_SHIFT,
    PW_HOLD_MASK = 0xffu << PW_HOLD_SHIFT,
    PW_GPU_SHIFT = 16,
    PW_GPU_ONE = 1u << PW_GPU_SHIFT,
    PW_GPU_MASK = 0xffu << PW_GPU_SHIFT,
    PW_FAULT_SHIFT = 24,
    PW_FAULT_ONE = 1u << PW_FAULT_SHIFT,
    PW_FAULT_MASK = 0xffu << PW_FAULT_SHIFT,
    PW_CH_WRITTEN = 1u << 0,
    PW_CH_UNARMED = 1u << 1,
    PW_CH_SEQ_ONE = 1u << 8,
};

typedef struct {
    uint64_t base;
    uint64_t end;
} PwRange;

static size_t pw_page_size;
static size_t pw_pages_per_chunk;
static ProtWatchProtectFn pw_protect_fn;
static ProtWatchStateFn pw_state_fn;

static uint32_t *pw_chunks[PW_CHUNKS];
static uint32_t pw_chunk_hint[PW_CHUNKS];
static PwRange pw_ranges[PW_MAX_RANGES];
static int pw_nranges;
static volatile uint64_t pw_fault_count;
static volatile uint64_t pw_repeat_count;
static uint64_t pw_hot_skips;   // arms skipped for hot pages (racy count: a diagnostic)   // faults on a page that faulted in its previous arm cycle too
static volatile int pw_batch_on;
static volatile int pw_hot_on;   // plat_pw_hot_mode (MKW_PW_HOT, perf.c)
static volatile uint64_t pw_protect_calls, pw_protect_pages;

// MKW_PW_HOT: a page written in two arm cycles in a row is "hot": the next queries leave it writable and report it
// written, instead of arming it again (one mprotect, 44 us on the PS5) only for the game's next write to fault it
// (another 44 us plus the signal, on the game's thread). Race of 15/11/2026: 141-185 fault unprotects a frame, 6-8 ms,
// half of them on pages that had faulted in their previous cycle too. A page left writable is always reported written,
// so a consumer copies it more often, never less. It is armed again after PW_REP_HOT - 1 queries, to see if it cooled.
int plat_pw_hot_mode(int on) {
    __atomic_store_n(&pw_hot_on, on ? 1 : 0, __ATOMIC_RELEASE);
    return on ? 1 : 0;
}

int plat_pw_batch_mode(int on) {
    __atomic_store_n(&pw_batch_on, on ? 1 : 0, __ATOMIC_RELEASE);
    return on ? 1 : 0;
}

void plat_mem_pw_stats(uint64_t *calls, uint64_t *pages, int reset) {
    uint64_t c = reset ? __atomic_exchange_n(&pw_protect_calls, 0, __ATOMIC_RELAXED)
                       : __atomic_load_n(&pw_protect_calls, __ATOMIC_RELAXED);
    uint64_t p = reset ? __atomic_exchange_n(&pw_protect_pages, 0, __ATOMIC_RELAXED)
                       : __atomic_load_n(&pw_protect_pages, __ATOMIC_RELAXED);
    if (calls) *calls = c;
    if (pages) *pages = p;
}

void prot_watch_init(size_t page_size, ProtWatchProtectFn protect_fn, ProtWatchStateFn state_fn) {
    pw_page_size = page_size;
    pw_pages_per_chunk = PW_CHUNK_SIZE / page_size;
    pw_protect_fn = protect_fn;
    pw_state_fn = state_fn;
}

int prot_watch_reserve(uint64_t addr, uint64_t n) {
    if (!n || addr >= (1ull << PW_LIMIT_SHIFT) || n > (1ull << PW_LIMIT_SHIFT) - addr) return 0;
    int idx = __atomic_load_n(&pw_nranges, __ATOMIC_ACQUIRE);
    if ((unsigned)idx >= PW_MAX_RANGES) return 0;
    pw_ranges[idx] = (PwRange){.base = addr, .end = addr + n};
    __atomic_store_n(&pw_nranges, idx + 1, __ATOMIC_RELEASE);
    return 1;
}

static int pw_range_covered(uint64_t lo, uint64_t hi) {
    int n = __atomic_load_n(&pw_nranges, __ATOMIC_ACQUIRE);
    uint64_t at = lo;
    while (at < hi) {
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (at >= pw_ranges[i].base && at < pw_ranges[i].end) {
                at = pw_ranges[i].end;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

static inline void pw_chunk_dirty(uint64_t addr, uint32_t bits) {
    if (addr >= (1ull << PW_LIMIT_SHIFT)) return;
    size_t cidx = (size_t)(addr >> PW_CHUNK_SHIFT);
    uint32_t cur = __atomic_load_n(&pw_chunk_hint[cidx], __ATOMIC_RELAXED);
    for (;;) {
        uint32_t next = (cur + PW_CH_SEQ_ONE) | bits;
        if (__atomic_compare_exchange_n(&pw_chunk_hint[cidx], &cur, next, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
    }
}

static uint32_t *pw_chunk(uint64_t addr, int create) {
    if (addr >= (1ull << PW_LIMIT_SHIFT)) return NULL;
    size_t cidx = (size_t)(addr >> PW_CHUNK_SHIFT);
    uint32_t *c = __atomic_load_n(&pw_chunks[cidx], __ATOMIC_ACQUIRE);
    if (c || !create) return c;
    uint32_t *fresh = calloc(pw_pages_per_chunk, sizeof *fresh);
    if (!fresh) return NULL;
    uint32_t *expected = NULL;
    if (__atomic_compare_exchange_n(&pw_chunks[cidx], &expected, fresh, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    free(fresh);
    return expected;
}

static inline size_t pw_slot_idx(uint64_t addr) {
    return (size_t)((addr & (PW_CHUNK_SIZE - 1)) / pw_page_size);
}

void prot_watch_touch(uint64_t addr, uint64_t n) {
    if (!n || !pw_page_size) return;
    uint64_t lo = addr & ~(uint64_t)(pw_page_size - 1);
    uint64_t hi = (addr + n + pw_page_size - 1) & ~(uint64_t)(pw_page_size - 1);
    if (!pw_range_covered(lo, hi)) return;
    for (uint64_t p = lo; p < hi; p += pw_page_size) {
        uint32_t *c = pw_chunk(p, 1);
        if (!c) continue;
        if ((p & (PW_CHUNK_SIZE - 1)) == 0 || p == lo)
            pw_chunk_dirty(p, PW_CH_WRITTEN | PW_CH_UNARMED);
        size_t idx = pw_slot_idx(p);
        uint32_t cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
        while (!(cur & PW_ARMED) && !(cur & PW_WRITTEN)) {
            if (__atomic_compare_exchange_n(&c[idx], &cur, cur | PW_WRITTEN, 1, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
                break;
        }
    }
}

void prot_watch_mark_written(uint64_t addr, uint64_t n) {
    if (!n || !pw_page_size) return;
    uint64_t lo = addr & ~(uint64_t)(pw_page_size - 1);
    uint64_t hi = (addr + n + pw_page_size - 1) & ~(uint64_t)(pw_page_size - 1);
    if (!pw_range_covered(lo, hi)) return;
    for (uint64_t p = lo; p < hi; p += pw_page_size) {
        uint32_t *c = pw_chunk(p, 1);
        if (!c) continue;
        pw_chunk_dirty(p, PW_CH_WRITTEN | PW_CH_UNARMED);
        size_t idx = pw_slot_idx(p);
        uint32_t cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
        for (;;) {
            uint32_t next = (cur & ~PW_ARMED) | PW_WRITTEN;
            if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
                break;
        }
    }
}

void prot_watch_hold(uint64_t addr, uint64_t n) {
    if (!n || !pw_page_size) return;
    uint64_t lo = addr & ~(uint64_t)(pw_page_size - 1);
    uint64_t hi = (addr + n + pw_page_size - 1) & ~(uint64_t)(pw_page_size - 1);
    for (uint64_t p = lo; p < hi; p += pw_page_size) {
        if (!pw_range_covered(p, p + pw_page_size)) continue;
        uint32_t *c = pw_chunk(p, 1);
        if (!c) continue;
        pw_chunk_dirty(p, PW_CH_WRITTEN | PW_CH_UNARMED);
        size_t idx = pw_slot_idx(p);
        uint32_t cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
        for (;;) {
            while (cur & PW_PROTECTING)
                cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
            uint32_t next = ((cur & ~PW_ARMED) | PW_WRITTEN) + PW_HOLD_ONE;
            if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE))
                break;
        }
        if ((cur & PW_ARMED) && pw_state_fn(p) == PLAT_PROT_RW)
            pw_protect_fn(p, pw_page_size, 1);
    }
}

void prot_watch_release(uint64_t addr, uint64_t n) {
    if (!n || !pw_page_size) return;
    uint64_t lo = addr & ~(uint64_t)(pw_page_size - 1);
    uint64_t hi = (addr + n + pw_page_size - 1) & ~(uint64_t)(pw_page_size - 1);
    for (uint64_t p = lo; p < hi; p += pw_page_size) {
        if (!pw_range_covered(p, p + pw_page_size)) continue;
        uint32_t *c = pw_chunk(p, 0);
        if (!c) continue;
        pw_chunk_dirty(p, PW_CH_WRITTEN | PW_CH_UNARMED);
        size_t idx = pw_slot_idx(p);
        uint32_t cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
        for (;;) {
            uint32_t next = (cur & PW_HOLD_MASK) ? ((cur - PW_HOLD_ONE) | PW_WRITTEN) : (cur | PW_WRITTEN);
            if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE))
                break;
        }
    }
}

void prot_watch_unprotect_and_mark(uint64_t addr, uint64_t n) {
    prot_watch_hold(addr, n);
    prot_watch_release(addr, n);
}

int prot_watch_page_info(uint64_t addr) {
    if (!pw_page_size || addr >= (1ull << PW_LIMIT_SHIFT)) return -1;
    uint32_t *c = pw_chunk(addr, 0);
    if (!c) return 0;
    return (int)__atomic_load_n(&c[pw_slot_idx(addr)], __ATOMIC_ACQUIRE);
}

int prot_watch_is_armed(uint64_t addr) {
    if (!pw_page_size) return 0;
    uint32_t *c = pw_chunk(addr, 0);
    if (!c) return 0;
    return (__atomic_load_n(&c[pw_slot_idx(addr)], __ATOMIC_ACQUIRE) & PW_ARMED) != 0;
}

void prot_watch_set_early(uint64_t addr, int on) {
    if (!pw_page_size || addr >= (1ull << PW_LIMIT_SHIFT)) return;
    uint32_t *c = pw_chunk(addr, on != 0);
    if (!c) return;
    size_t idx = pw_slot_idx(addr);
    uint32_t cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
    for (;;) {
        while (cur & (PW_PROTECTING | PW_FAULT_MASK))
            cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
        uint32_t next = on ? (cur | PW_EARLY) : (cur & ~PW_EARLY);
        if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE))
            return;
    }
}

int prot_watch_is_early(uint64_t addr) {
    if (!pw_page_size || addr >= (1ull << PW_LIMIT_SHIFT)) return 0;
    uint32_t *c = pw_chunk(addr, 0);
    if (!c) return 0;
    return (__atomic_load_n(&c[pw_slot_idx(addr)], __ATOMIC_ACQUIRE) & PW_EARLY) != 0;
}

int prot_watch_fault(uint64_t addr) {
    if (!pw_page_size || addr >= (1ull << PW_LIMIT_SHIFT)) return 0;
    uint32_t *c = pw_chunk(addr, 0);
    if (!c) return 0;
    uint64_t p = addr & ~(uint64_t)(pw_page_size - 1);
    size_t idx = pw_slot_idx(p);
    uint32_t prev = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
    if (prev & PW_EARLY) return 0;
    if (!(prev & (PW_ARMED | PW_WRITTEN | PW_PROTECTING | PW_FAULT_MASK))) return 0;
    if (pw_state_fn(p) != PLAT_PROT_RW) return 0;
    pw_chunk_dirty(p, PW_CH_WRITTEN | PW_CH_UNARMED);
    uint32_t cur = prev;
    for (;;) {
        while (cur & PW_PROTECTING)
            cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
        uint32_t next = ((cur & ~PW_ARMED) | PW_WRITTEN) + PW_FAULT_ONE;
        if (cur & PW_ARMED) {
            uint32_t rep = (cur & PW_REP_MASK) >> PW_REP_SHIFT;
            next = (next & ~PW_REP_MASK) | ((rep ? PW_REP_HOT : 1u) << PW_REP_SHIFT);
        }
        if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE))
            break;
    }
    int ok = pw_protect_fn(p, pw_page_size, 1);
    cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t next = (cur & PW_FAULT_MASK) ? (cur - PW_FAULT_ONE) : cur;
        if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE))
            break;
    }
    if (!ok) return 0;
    if (prev & PW_ARMED) {
        __atomic_fetch_add(&pw_fault_count, 1, __ATOMIC_RELAXED);
        if (prev & PW_REP_MASK) __atomic_fetch_add(&pw_repeat_count, 1, __ATOMIC_RELAXED);
    }
    return 1;
}

static void pw_flush_arm_run(uint32_t *c, uint64_t run_start, uint64_t run_end, int *saw_held, int *did_protect) {
    if (run_end <= run_start) return;
    size_t n_pages = (size_t)((run_end - run_start) / pw_page_size);
    __atomic_fetch_add(&pw_protect_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&pw_protect_pages, n_pages, __ATOMIC_RELAXED);
    int ok = pw_protect_fn(run_start, (size_t)(run_end - run_start), 0);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (uint64_t q = run_start; q < run_end; q += pw_page_size) {
        size_t idx = pw_slot_idx(q);
        uint32_t after = __atomic_load_n(&c[idx], __ATOMIC_SEQ_CST);
        uint32_t next;
        int unprotect = 0;
        if (!ok || (after & PW_HOLD_MASK)) {
            next = (after & ~(PW_ARMED | PW_PROTECTING)) | PW_WRITTEN;
            unprotect = ok && (after & PW_HOLD_MASK);
            if (after & PW_HOLD_MASK) *saw_held = 1;
        } else {
            next = after & ~PW_PROTECTING;
            if (did_protect) *did_protect = 1;
        }
        for (;;) {
            if (__atomic_compare_exchange_n(&c[idx], &after, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)) break;
            if (!ok || (after & PW_HOLD_MASK)) {
                next = (after & ~(PW_ARMED | PW_PROTECTING)) | PW_WRITTEN;
                unprotect = ok && (after & PW_HOLD_MASK);
            } else {
                next = after & ~PW_PROTECTING;
            }
        }
        if (unprotect) pw_protect_fn(q, pw_page_size, 1);
    }
}

uint64_t prot_watch_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset, int *did_protect) {
    if (did_protect) *did_protect = 0;
    if (!max || !n) return 0;
    if (!pw_page_size || n > UINT64_MAX - addr) return UINT64_MAX;
    uint64_t lo = addr & ~(uint64_t)(PW_GUEST_PAGE - 1);
    uint64_t hi = (addr + n + PW_GUEST_PAGE - 1) & ~(uint64_t)(PW_GUEST_PAGE - 1);
    if (!pw_range_covered(lo, hi)) return UINT64_MAX;

    int batch = __atomic_load_n(&pw_batch_on, __ATOMIC_RELAXED);
    int hot = __atomic_load_n(&pw_hot_on, __ATOMIC_RELAXED);
    uint64_t got = 0;
    uint64_t p = lo & ~(uint64_t)(pw_page_size - 1);
    while (p < hi) {
        size_t cidx = (size_t)(p >> PW_CHUNK_SHIFT);
        uint32_t *c = __atomic_load_n(&pw_chunks[cidx], __ATOMIC_ACQUIRE);
        uint64_t chunk_start = p & ~(uint64_t)(PW_CHUNK_SIZE - 1);
        uint64_t chunk_end = chunk_start + PW_CHUNK_SIZE;
        uint64_t scan_end = chunk_end < hi ? chunk_end : hi;
        uint32_t ch0 = __atomic_load_n(&pw_chunk_hint[cidx], __ATOMIC_ACQUIRE);
        if (!c || (!(ch0 & PW_CH_WRITTEN) && (!reset || !(ch0 & PW_CH_UNARMED)))) {
            p = scan_end;
            continue;
        }
        int full_chunk = (p == chunk_start) && (scan_end == chunk_end);
        int saw_held = 0;
        int stopped_early = 0;
        uint64_t run_start = 0, run_end = 0;
        uint32_t run_gpu = 0;
        while (p < scan_end) {
            size_t idx = pw_slot_idx(p);
            uint32_t v = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
            uint64_t sub_lo = p > lo ? p : lo;
            uint64_t sub_hi = (p + pw_page_size) < hi ? (p + pw_page_size) : hi;
            uint64_t sub_cnt = (sub_hi - sub_lo) / PW_GUEST_PAGE;
            int partial = 0;
            if (v & PW_WRITTEN) {
                if (got + sub_cnt > max && got > 0) {
                    stopped_early = 1;
                    break;
                }
                uint64_t s = sub_lo;
                for (; s < sub_hi && got < max; s += PW_GUEST_PAGE) pages[got++] = s;
                if (s < sub_hi) partial = 1;
            }
            if (reset && (v & PW_ARMED) && !(v & PW_WRITTEN) && (v & PW_REP_MASK)) {   // a cycle without a write
                uint32_t cur = v;
                while (!(cur & PW_WRITTEN) && (cur & PW_REP_MASK) &&
                       !__atomic_compare_exchange_n(&c[idx], &cur, cur & ~PW_REP_MASK, 1, __ATOMIC_ACQ_REL,
                                                    __ATOMIC_ACQUIRE)) {}
            }
            int sub_partial = (sub_lo > p) || (sub_hi < p + pw_page_size);
            int armed_cas = 0;
            uint32_t gpu_bits = 0;
            if (reset && !partial && !sub_partial) {
                if (v & PW_HOLD_MASK) {
                    saw_held = 1;
                } else {
                    int st = pw_state_fn(p);
                    uint32_t rep = (v & PW_REP_MASK) >> PW_REP_SHIFT;
                    if (st == PLAT_PROT_RW && hot && (v & PW_WRITTEN) && !(v & PW_ARMED) && rep >= 2) {
                        // Hot: stays writable and written (reported again by the next query); one query less to go.
                        uint32_t cur = v;
                        while (((cur & PW_REP_MASK) >> PW_REP_SHIFT) >= 2 && !(cur & (PW_ARMED | PW_HOLD_MASK)) &&
                               !__atomic_compare_exchange_n(&c[idx], &cur, cur - PW_REP_ONE, 1, __ATOMIC_ACQ_REL,
                                                            __ATOMIC_ACQUIRE)) {}
                        saw_held = 1;   // the chunk keeps its written hint: the page is still to report
                        pw_hot_skips++;
                    } else if (st == PLAT_PROT_RW) {
                        if (!(v & PW_ARMED) || (v & PW_WRITTEN)) {
                            uint32_t cur = v;
                            for (;;) {
                                while (cur & PW_FAULT_MASK)
                                    cur = __atomic_load_n(&c[idx], __ATOMIC_ACQUIRE);
                                if (cur & (PW_HOLD_MASK | PW_PROTECTING)) {
                                    if (cur & PW_HOLD_MASK) saw_held = 1;
                                    break;
                                }
                                gpu_bits = cur & PW_GPU_MASK;
                                uint32_t early_bit = cur & PW_EARLY;
                                uint32_t want = gpu_bits | early_bit | (cur & PW_REP_MASK) | PW_ARMED |
                                                (early_bit ? 0 : PW_PROTECTING);
                                if (__atomic_compare_exchange_n(&c[idx], &cur, want, 1,
                                                                __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)) {
                                    armed_cas = !early_bit;
                                    break;
                                }
                            }
                        }
                    } else if (v & PW_WRITTEN) {
                        uint32_t cur = v;
                        for (;;) {
                            if (cur & PW_HOLD_MASK) { saw_held = 1; break; }
                            uint32_t next = cur & ~PW_WRITTEN;
                            if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1,
                                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                                break;
                        }
                    }
                }
            }
            if (armed_cas) {
                if (batch) {
                    if (run_end == p && run_gpu == gpu_bits) {
                        run_end = p + pw_page_size;
                    } else {
                        pw_flush_arm_run(c, run_start, run_end, &saw_held, did_protect);
                        run_start = p;
                        run_end = p + pw_page_size;
                        run_gpu = gpu_bits;
                    }
                } else {
                    pw_flush_arm_run(c, p, p + pw_page_size, &saw_held, did_protect);
                }
            } else if (run_end > run_start) {
                pw_flush_arm_run(c, run_start, run_end, &saw_held, did_protect);
                run_start = run_end = 0;
            }
            if (partial || got >= max) {
                stopped_early = 1;
                break;
            }
            p += pw_page_size;
        }
        if (run_end > run_start) pw_flush_arm_run(c, run_start, run_end, &saw_held, did_protect);
        if (reset && full_chunk && !stopped_early && !saw_held) {
            uint32_t clean = ch0 & ~(PW_CH_WRITTEN | PW_CH_UNARMED);
            __atomic_compare_exchange_n(&pw_chunk_hint[cidx], &ch0, clean, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
        if (stopped_early) break;
    }
    return got;
}

void prot_watch_gpu_import(uint64_t addr, uint64_t n, int on) {
    if (!pw_page_size || !n || addr > UINT64_MAX - n) return;
    uint64_t lo = addr & ~(uint64_t)(pw_page_size - 1);
    uint64_t hi = (addr + n + pw_page_size - 1) & ~(uint64_t)(pw_page_size - 1);
    for (uint64_t p = lo; p < hi; p += pw_page_size) {
        size_t cidx = (size_t)(p >> PW_CHUNK_SHIFT);
        if (cidx >= PW_CHUNKS) break;
        uint32_t *c = pw_chunk(p, on);
        if (!c) { p = ((uint64_t)(cidx + 1) << PW_CHUNK_SHIFT) - pw_page_size; continue; }
        size_t idx = pw_slot_idx(p);
        uint32_t cur = __atomic_load_n(&c[idx], __ATOMIC_RELAXED);
        for (;;) {
            uint32_t gpu = (cur & PW_GPU_MASK) >> PW_GPU_SHIFT;
            if (on ? (gpu == 0xffu) : (gpu == 0)) break;
            uint32_t next = (cur & ~PW_GPU_MASK) | ((on ? gpu + 1 : gpu - 1) << PW_GPU_SHIFT);
            if (__atomic_compare_exchange_n(&c[idx], &cur, next, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) break;
        }
    }
}

int prot_watch_has_gpu(uint64_t addr, uint64_t n) {
    if (!pw_page_size || !n || addr > UINT64_MAX - n) return 0;
    uint64_t lo = addr & ~(uint64_t)(pw_page_size - 1);
    uint64_t hi = (addr + n + pw_page_size - 1) & ~(uint64_t)(pw_page_size - 1);
    for (uint64_t p = lo; p < hi; p += pw_page_size) {
        size_t cidx = (size_t)(p >> PW_CHUNK_SHIFT);
        if (cidx >= PW_CHUNKS) break;
        uint32_t *c = __atomic_load_n(&pw_chunks[cidx], __ATOMIC_ACQUIRE);
        if (!c) { p = ((uint64_t)(cidx + 1) << PW_CHUNK_SHIFT) - pw_page_size; continue; }
        if (__atomic_load_n(&c[pw_slot_idx(p)], __ATOMIC_ACQUIRE) & PW_GPU_MASK) return 1;
    }
    return 0;
}

uint64_t prot_watch_hot_skips(int reset) {
    uint64_t v = __atomic_load_n(&pw_hot_skips, __ATOMIC_RELAXED);
    if (reset) __atomic_store_n(&pw_hot_skips, 0, __ATOMIC_RELAXED);
    return v;
}

uint64_t prot_watch_repeat_faults(int reset) {
    return reset ? __atomic_exchange_n(&pw_repeat_count, 0, __ATOMIC_RELAXED)
                 : __atomic_load_n(&pw_repeat_count, __ATOMIC_RELAXED);
}

uint64_t prot_watch_faults(int reset) {
    if (reset) return __atomic_exchange_n(&pw_fault_count, 0, __ATOMIC_ACQ_REL);
    return __atomic_load_n(&pw_fault_count, __ATOMIC_RELAXED);
}

void prot_watch_shutdown(void) {
    for (size_t i = 0; i < PW_CHUNKS; i++) {
        free(pw_chunks[i]);
        pw_chunks[i] = NULL;
        pw_chunk_hint[i] = 0;
    }
    __atomic_store_n(&pw_nranges, 0, __ATOMIC_RELEASE);
}

#endif
