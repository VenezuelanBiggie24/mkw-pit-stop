// Guest heap census (MKW_HEAP_CENSUS=1, game 1.8.0): how much of the guest memory the runtime backs is free inside the
// game's own heaps, by reading the game's heap tree. The Knockout Tour runs of 27/11/2026 ran out of direct memory with
// the guest heap (8,226 MiB, backed lazily) at 3,495 MiB backed and growing ~66-94 MiB/min; memgrow2 (16/11/2026) had
// found ~1.2 GiB of one pool to be data of courses already left. Freed blocks stay backed: the game's allocator keeps
// them. Before giving any of it back (to MKW_PACK, losslessly, first), this measures how much there is.
//
// Read only, on its own thread, every CENSUS_S: nothing runs on the game's or the replay's threads, and no guest code
// is called. The layout, from the 1.8.0 code (it is sead's Heap/ExpHeap layout, as main's pead library keeps it):
//   roots      main+0x37be5f0: int count, int capacity, Heap **array at +8 (RootHeap's creator main+0xbeb470 appends)
//   Heap       +0x00 vtable, +0x20 const char *name, +0x28 start, +0x30 size, +0x38 parent,
//              +0x40 children (OffsetList: sentinel {next, prev}, +0x50 int count, +0x54 int node offset = 0x58),
//              +0xc8 flags (bit 0: locked); base constructor main+0x5dd360
//   ExpHeap    vtable main+0x363a300 (constructor main+0x5dd2e0); free blocks: OffsetList at +0xd0 (count +0xe0, node
//              offset +0xe4 = 0), used blocks at +0xe8; the first block at heap+0x100 (main+0xbeb540)
//   block      0x20-byte header: list node at +0, u16 tag at +0x10, u64 payload size at +0x18; payload at +0x20
// The game changes these lists while it runs and the census takes no lock: every pointer is checked (8-aligned, in
// backed guest memory, inside its heap), every walk is bounded by the list's own count, and a walk that sees a broken
// list says so and skips that heap for the pass. A read is made only of memory the runtime has backed
// (plat_mem_backed_bytes, lock-free): the census never backs a page nor faults one in.
// Other heap types (Frame, TLSF, Unit, Separate) are listed with their sizes; only ExpHeaps' free blocks are counted.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "heap_census.h"
#include "host.h"
#include "version.h"
#include "platform/platform.h"

#define CENSUS_S 120
#define MAX_HEAPS 4096
#define MAX_DEPTH 16
#define TOP 15
#define ROOTS_180 0x37be5f0
#define EXPHEAP_VT_180 0x363a300

typedef struct {
    uint64_t addr, vt, size, parent;
    char name[40];
    int depth, exp, broken;
    uint64_t free_bytes, free_blocks, largest, free_backed;
} HeapRow;

static HeapRow rows[MAX_HEAPS];
static int nrows;
static uint64_t exp_vt, main_base;

// A guest word, only from memory the runtime has backed (heap objects) or from main's image (the root table).
static int rd(uint64_t a, uint64_t *out) {
    if (a & 7) return 0;
    if (a < main_base || a >= main_base + 0x10000000ull)
        if (plat_mem_backed_bytes(a, 8) != 8) return 0;
    *out = *(const volatile uint64_t *)(uintptr_t)a;
    return 1;
}

static void rd_name(uint64_t p, char *out, size_t cap) {
    snprintf(out, cap, "?");
    if (!p) return;
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        uint64_t w;
        if (!rd((p + i) & ~7ull, &w)) break;
        char ch = (char)(w >> (((p + i) & 7) * 8));
        if (!ch) break;
        out[i] = (ch >= 32 && ch < 127) ? ch : '.';
    }
    out[i ? i : 1] = 0;
}

// The free blocks of the ExpHeap at h: their payload bytes, how many, the largest, and how much of them is backed.
static void exp_free(HeapRow *r) {
    uint64_t h = r->addr, end = h + r->size, head = h + 0xd0, cnt_off = 0, n;
    if (!rd(h + 0xe0, &cnt_off)) { r->broken = 1; return; }
    int32_t count = (int32_t)(uint32_t)cnt_off, off = (int32_t)(uint32_t)(cnt_off >> 32);
    if (count < 0 || off < 0 || off >= 0x20) { r->broken = 1; return; }
    if (!rd(head, &n)) { r->broken = 1; return; }
    for (int32_t i = 0; n != head; i++) {
        uint64_t blk = n - (uint64_t)off, size;
        if (i > count + 16 || blk < h + 0x100 || blk >= end || !rd(blk + 0x18, &size) || size > r->size) {
            r->broken = 1;
            return;
        }
        r->free_bytes += size, r->free_blocks++;
        if (size > r->largest) r->largest = size;
        r->free_backed += plat_mem_backed_bytes(blk + 0x20, size);
        if (!rd(n, &n)) { r->broken = 1; return; }
    }
}

static void visit(uint64_t h, int depth) {
    if (nrows >= MAX_HEAPS || depth > MAX_DEPTH) return;
    for (int i = 0; i < nrows; i++)
        if (rows[i].addr == h) return;   // a broken list looping back
    HeapRow *r = &rows[nrows];
    memset(r, 0, sizeof *r);
    uint64_t name_p = 0, cl = 0, n;
    if (!rd(h, &r->vt) || !rd(h + 0x20, &name_p) || !rd(h + 0x30, &r->size) || !rd(h + 0x38, &r->parent)) return;
    if (!r->size || r->size > (64ull << 30)) return;
    nrows++;
    r->addr = h, r->depth = depth, r->exp = r->vt == exp_vt;
    rd_name(name_p, r->name, sizeof r->name);
    if (r->exp) exp_free(r);
    // children: sentinel at +0x40, count and node offset at +0x50
    uint64_t head = h + 0x40;
    if (!rd(h + 0x50, &cl) || !rd(head, &n)) return;
    int32_t count = (int32_t)(uint32_t)cl, off = (int32_t)(uint32_t)(cl >> 32);
    if (count < 0 || off <= 0 || off > 0x200) return;
    for (int32_t i = 0; n != head && i <= count + 4; i++) {
        uint64_t child = n - (uint64_t)off;
        visit(child, depth + 1);
        if (!rd(n, &n)) break;
    }
}

static int cmp_backed(const void *a, const void *b) {
    const HeapRow *x = *(HeapRow *const *)a, *y = *(HeapRow *const *)b;
    return (x->free_backed < y->free_backed) - (x->free_backed > y->free_backed);
}

static void census(int pass) {
    uint64_t t0 = plat_now_ns(), roots = rt_main_addr(ROOTS_180), cnt = 0, arr = 0;
    nrows = 0;
    if (rd(roots, &cnt) && rd(roots + 8, &arr)) {
        int32_t n = (int32_t)(uint32_t)cnt;
        for (int32_t i = 0; i < n && i < 64; i++) {
            uint64_t h;
            if (rd(arr + (uint64_t)i * 8, &h) && h) visit(h, 0);
        }
    }
    uint64_t exp_n = 0, exp_size = 0, free_b = 0, free_n = 0, backed = 0, largest = 0, all = 0, broken = 0;
    static HeapRow *by[MAX_HEAPS];
    int nb = 0;
    for (int i = 0; i < nrows; i++) {
        HeapRow *r = &rows[i];
        if (!r->depth) all += r->size;
        if (!r->exp) continue;
        exp_n++, exp_size += r->size, broken += r->broken;
        if (r->broken) continue;
        free_b += r->free_bytes, free_n += r->free_blocks, backed += r->free_backed;
        if (r->largest > largest) largest = r->largest;
        by[nb++] = r;
    }
    const double M = 1048576.0;
    rt_log("census: pass %d: %d heaps under %.0f MiB of roots (%llu ExpHeaps, %.0f MiB); ExpHeap free %.1f MiB in %llu "
           "blocks (largest %.1f MiB), of it backed %.1f MiB; %llu ExpHeaps changed during the walk; %.1f ms",
           pass, nrows, all / M, (unsigned long long)exp_n, exp_size / M, free_b / M, (unsigned long long)free_n,
           largest / M, backed / M, (unsigned long long)broken, (plat_now_ns() - t0) / 1e6);
    qsort(by, (size_t)nb, sizeof *by, cmp_backed);
    for (int i = 0; i < nb && i < TOP && by[i]->free_backed; i++) {
        const HeapRow *r = by[i];
        rt_log("census:   %-38s %9.1f MiB, free %8.1f MiB in %6llu blocks (largest %7.1f), backed free %7.1f MiB "
               "(depth %d, %#llx)", r->name, r->size / M, r->free_bytes / M, (unsigned long long)r->free_blocks,
               r->largest / M, r->free_backed / M, r->depth, (unsigned long long)r->addr);
    }
    if (pass == 1)   // the tree once: every heap with its type and size
        for (int i = 0; i < nrows; i++)
            rt_log("census: heap %*s%s (%s, %.1f MiB at %#llx)", rows[i].depth * 2, "", rows[i].name,
                   rows[i].exp ? "ExpHeap" : "other", rows[i].size / M, (unsigned long long)rows[i].addr);
}

// agl::TextureDataMgr's compactible heap over its texture pool (1.8.0; nvn_api_core.c finds the pool). The game's
// guest growth in a Knockout Tour is this pool (2,372 MiB, 1.0 -> 1.9 GiB backed per tour, koCEN 27/11/2026). Layout,
// from its setup main+0xb2d8c0 -> main+0xa92170 -> main+0xa922a0 and its reset (vtable main+0x363d120 slot 6,
// main+0xa92470): the control object lies right after the storage (vtable main+0x363d030, its base's main+0x363d120, +0x1c18 managed base,
// +0x1c20 size, +0x1bc8 head and +0x1bd0 tail sentinel descriptors). A block is a 0x48-byte descriptor: +0 u32 magic
// (0xad000d1e on the sentinels, 0xd15ab1ed on the free block a reset makes), +4 / +8 s32 next / previous block in
// address order as a distance in 8-byte units, +0x18 address, +0x20 size. The walk counts blocks by magic (logged, so
// which one marks a free block is checked on the console, not assumed) and how much of each is backed.
extern uint64_t nvn_agl_tex_pool_base, nvn_agl_tex_pool_size;
#define TEXHEAP_VT_180 0x363d030       // the heap's own class (main+0xa92170 stores it after its base's)
#define TEXHEAP_BASE_VT_180 0x363d120  // its base class's (main+0xa922a0)
#define TEXHEAP_FREE_MAGIC 0xd15ab1edu
#define TEXHEAP_SENTINEL 0xad000d1eu
#define TEXHEAP_MAX_BLOCKS 1000000

// The free blocks of the texture heap as [lo, hi) pairs, at most max (vk_mem_upload.c pack_step: MKW_PACK_TEXFREE);
// -1 when the heap is not known or not found. Read only and racy as the census is: a block may be allocated by the
// time the caller uses it, which packing (lossless) tolerates.
int texheap_free_ranges(uint64_t (*out)[2], int max) {
    uint64_t base = __atomic_load_n(&nvn_agl_tex_pool_base, __ATOMIC_ACQUIRE), size = nvn_agl_tex_pool_size;
    if (!base || rt_version()->number != 180) return -1;
    if (!main_base) main_base = rt_main_addr(1) - 1;
    uint64_t h = (base + size + 7) & ~7ull, vt = 0, mbase = 0, msize = 0, d = 0, tail = 0;
    if (!rd(h, &vt) || (vt != rt_main_addr(TEXHEAP_VT_180) && vt != rt_main_addr(TEXHEAP_BASE_VT_180)) ||
        !rd(h + 0x1c18, &mbase) || !rd(h + 0x1c20, &msize) || !rd(h + 0x1bc8, &d) || !rd(h + 0x1bd0, &tail))
        return -1;
    int n = 0;
    uint64_t lo = mbase, hi = mbase + msize, last_end = mbase;
    for (uint64_t blocks = 0; d != tail && n < max; blocks++) {
        uint64_t w0, addr, bsize;
        if (blocks > TEXHEAP_MAX_BLOCKS || !rd(d, &w0) || !rd(d + 0x18, &addr) || !rd(d + 0x20, &bsize)) break;
        uint32_t magic = (uint32_t)w0;
        int32_t next = (int32_t)(w0 >> 32);
        if (magic == TEXHEAP_FREE_MAGIC) {
            if (addr < lo || addr > hi || bsize > hi - addr || addr < last_end) break;
            out[n][0] = addr, out[n][1] = addr + bsize, n++;
        }
        if (magic != TEXHEAP_SENTINEL) last_end = addr + bsize;
        if (!next) break;
        d += (uint64_t)((int64_t)next * 8);
    }
    return n;
}

static void texheap_census(void) {
    uint64_t base = __atomic_load_n(&nvn_agl_tex_pool_base, __ATOMIC_ACQUIRE), size = nvn_agl_tex_pool_size;
    if (!base) return;
    uint64_t h = (base + size + 7) & ~7ull, vt = 0, mbase = 0, msize = 0, d = 0, tail = 0, t0 = plat_now_ns();
    if (!rd(h, &vt) || (vt != rt_main_addr(TEXHEAP_VT_180) && vt != rt_main_addr(TEXHEAP_BASE_VT_180)) || !rd(h + 0x1c18, &mbase) || !rd(h + 0x1c20, &msize) ||
        !rd(h + 0x1bc8, &d) || !rd(h + 0x1bd0, &tail)) {
        rt_log("census: texture heap: no control object at %#llx (vtable %#llx): not walked", (unsigned long long)h,
               (unsigned long long)vt);
        return;
    }
    struct { uint32_t magic; uint64_t n, bytes, backed; } m[8] = {0};
    int nm = 0, broken = 0;
    uint64_t blocks = 0, largest_free = 0, lo = mbase, hi = mbase + msize, last_end = mbase;
    for (; d != tail; blocks++) {
        uint64_t w0, addr, bsize;
        if (blocks > TEXHEAP_MAX_BLOCKS || !rd(d, &w0) || !rd(d + 0x18, &addr) || !rd(d + 0x20, &bsize)) { broken = 1; break; }
        uint32_t magic = (uint32_t)w0;
        int32_t next = (int32_t)(w0 >> 32);
        if (magic != TEXHEAP_SENTINEL) {
            if (addr < lo || addr > hi || bsize > hi - addr || addr < last_end) { broken = 1; break; }
            last_end = addr + bsize;
            int k = 0;
            while (k < nm && m[k].magic != magic) k++;
            if (k == nm && nm < 8) m[nm++].magic = magic;
            if (k < nm) {
                m[k].n++, m[k].bytes += bsize, m[k].backed += plat_mem_backed_bytes(addr, bsize);
                if (magic == TEXHEAP_FREE_MAGIC && bsize > largest_free) largest_free = bsize;
            }
        }
        if (!next) { broken = 1; break; }
        d += (uint64_t)((int64_t)next * 8);
    }
    const double M = 1048576.0;
    rt_log("census: texture heap %#llx (agl::TextureDataMgr): %.1f MiB managed from %#llx, %llu blocks%s, %.1f ms; "
           "largest block marked %#x: %.1f MiB", (unsigned long long)h, msize / M, (unsigned long long)mbase,
           (unsigned long long)blocks, broken ? " (changed during the walk: partial)" : "", (plat_now_ns() - t0) / 1e6,
           TEXHEAP_FREE_MAGIC, largest_free / M);
    for (int k = 0; k < nm; k++)
        rt_log("census:   blocks marked %#010x: %llu, %.1f MiB, of it backed %.1f MiB%s", m[k].magic,
               (unsigned long long)m[k].n, m[k].bytes / M, m[k].backed / M,
               m[k].magic == TEXHEAP_FREE_MAGIC ? " (the mark a reset gives the free block)" : "");
}

static void census_thread(void *arg) {
    (void)arg;
    for (int pass = 1;; pass++) {
        census(pass);
        texheap_census();
        plat_sleep_ms(CENSUS_S * 1000);
    }
}

void heap_census_start(void) {
    static int started;
    const char *v = cfg_get("MKW_HEAP_CENSUS");
    if (started || !v || atoi(v) <= 0) return;
    started = 1;
    if (rt_version()->number != 180) {
        rt_log("census: MKW_HEAP_CENSUS knows the heap layout of game 1.8.0 only: no census");
        return;
    }
    main_base = rt_main_addr(1) - 1;
    exp_vt = rt_main_addr(EXPHEAP_VT_180);
    PlatThread *t = plat_thread_new(census_thread, NULL, "heap-census");
    if (!t) {
        rt_log("census: no thread: no census");
        return;
    }
    plat_thread_set_priority(t, PLAT_PRIO_BELOW_NORMAL);
    rt_log("census: the game's heaps read every %d s (MKW_HEAP_CENSUS)", CENSUS_S);
}
