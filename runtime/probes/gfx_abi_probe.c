// --probe-gfx-abi: every host object of nvn_obj.h fits the storage nvn.h reserves
// for its NVN type, the layouts the game reads directly (NVNtextureView, the command buffer's inline fast-path
// fields) sit where TotK's NVN 55 code expects them, and the GPU address window lies above every guest region.
// The same facts are static assertions in nvn_obj.h; this mode prints the sizes so a change shows its margin.
// Needs no game files.
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "platform/platform.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

typedef struct { const char *host, *nvn; size_t size, reserved; } Fit;
#define FIT(T, H) {#T, #H, sizeof(T), sizeof(H)}
static const Fit fits[] = {
    FIT(dev_builder_t, NVNdeviceBuilder),       FIT(dev_t_, NVNdevice),
    FIT(queue_builder_t, NVNqueueBuilder),      FIT(queue_t, NVNqueue),
    FIT(pool_builder_t, NVNmemoryPoolBuilder),  FIT(pool_t, NVNmemoryPool),
    FIT(dpool_t, NVNtexturePool),               FIT(dpool_t, NVNsamplerPool),
    FIT(tex_builder_t, NVNtextureBuilder),      FIT(tex_t, NVNtexture),
    FIT(view_t, NVNtextureView),
    FIT(samp_builder_t, NVNsamplerBuilder),     FIT(samp_t, NVNsampler),
    FIT(prog_t, NVNprogram),
    FIT(sync_t, NVNsync),
    FIT(ev_builder_t, NVNeventBuilder),         FIT(ev_t, NVNevent),
    FIT(win_builder_t, NVNwindowBuilder),       FIT(win_t, NVNwindow),
    FIT(cmd_t, NVNcommandBuffer),
    FIT(color_st, NVNcolorState),               FIT(chmask_st, NVNchannelMaskState),
    FIT(blend_st, NVNblendState),               FIT(ms_st, NVNmultisampleState),
    FIT(poly_st, NVNpolygonState),              FIT(ds_st, NVNdepthStencilState),
    FIT(va_st, NVNvertexAttribState),           FIT(vs_st, NVNvertexStreamState),
};

_Static_assert(sizeof(NVNtextureView) == 16, "NVNtextureView is 16 bytes in TotK's NVN 55");
_Static_assert(offsetof(cmd_t, inline_cur) == 0x20 && offsetof(cmd_t, inline_end) == 0x28, "cmd_t inline fields");
_Static_assert(RT_GPU_VA_BASE >= RT_ADDRESS_LIMIT, "GPU address window overlaps the guest address space");

// Write watch: a committed heap page and an alias page, written once, are reported once.
#if defined(__linux__)
typedef struct { uint64_t addr; PlatFile *f; int done, bad_rc, bad_data; } WwStressCtx;

enum { WW_MT_THREADS = 3, WW_MT_PAGES = 64, WW_MT_ITERS = 4000 };
typedef struct {
    uint64_t base;
    int tid;
    uint64_t last[WW_MT_PAGES];
    volatile int done;
} WwMtCtx;

static void ww_mt_fn(void *arg) {
    WwMtCtx *c = arg;
    uint32_t rng = 0x1234567u + (uint32_t)c->tid * 0x9e3779b9u;
    for (int i = 1; i <= WW_MT_ITERS; i++) {
        rng = rng * 1664525u + 1013904223u;
        uint32_t pg = (rng >> 16) % WW_MT_PAGES;
        volatile uint64_t *slot = (volatile uint64_t *)(uintptr_t)(c->base + (uint64_t)pg * 4096u + (uint64_t)c->tid * 8u);
        __atomic_store_n(slot, (uint64_t)i, __ATOMIC_RELEASE);
        c->last[pg] = (uint64_t)i;
    }
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
}
static void ww_stress_fn(void *arg) {
    WwStressCtx *c = arg;
    uint8_t *b = (uint8_t *)(uintptr_t)c->addr;
    for (int i = 0; i < 5000; i++) {
        memset(b, 0xA5, 0x4000);
        plat_mem_back(c->addr, 0x4000);
        int64_t r = plat_file_pread(c->f, b, 0x4000, 0);
        if (r != 0x4000) { c->bad_rc++; break; }
        for (int j = 0; j < 0x4000; j++)
            if (b[j] != 0) { c->bad_data++; break; }
        if (c->bad_data) break;
    }
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
}
#endif

static void check_write_watch(void) {
    const RtGuestLayout *g = mem_layout();
    uint64_t heap = 0, pages[8], n;
    CHECK(mem_set_heap_size(0x200000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 2 MiB");
    while (mem_written_pages(heap, 0x200000, pages, 8) > 0) {}   // drop whatever the commit itself marked
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)heap;
    p[0x5000] = 1;
    n = mem_written_pages(heap, 0x200000, pages, 8);
    CHECK(n == 1 && pages[0] == heap + 0x5000, "heap write: %llu pages reported (want 1 at %#llx)",
          (unsigned long long)n, (unsigned long long)(heap + 0x5000));
    printf("write-watch: heap page written once: %llu reported at +%#llx\n", (unsigned long long)n,
           n ? (unsigned long long)(pages[0] - heap) : 0ull);
    n = mem_written_pages(heap, 0x200000, pages, 8);
    CHECK(n == 0, "second call reported %llu pages (want 0)", (unsigned long long)n);
    printf("write-watch: second call: %llu reported\n", (unsigned long long)n);
    p[0x7000] = 2;
    p[0x9000] = 3;
    p[0xA000] = 4;
    n = mem_written_pages(heap, 0x200000, pages, 2);
    uint64_t m = mem_written_pages(heap, 0x200000, pages + 2, 6);
    printf("write-watch: three pages written, max 2: %llu then %llu\n", (unsigned long long)n, (unsigned long long)m);
    CHECK(n == 2 && m == 1, "max 2 reported %llu, then %llu (want 2, 1)", (unsigned long long)n, (unsigned long long)m);

    // Frozen region: read-only until the first write thaws it (the write lands and is reported); mem_thaw (a file
    // read) thaws it and holds it thawed; SetHeapSize below decommits it.
    mem_written_pages(heap, 0x200000, pages, 8);
    CHECK(mem_freeze(heap) == 1 && mem_frozen(heap), "freeze of an idle committed 2 MiB region");
    uint64_t end;
    CHECK(plat_mem_state(heap, heap + 1, &end) == PLAT_PROT_READ, "frozen region is not read-only");
    p[0x3000] = 7;
    CHECK(!mem_frozen(heap) && p[0x3000] == 7, "a write into a frozen region did not thaw it");
    n = mem_written_pages(heap, 0x200000, pages, 8);
    CHECK(n == 1 && pages[0] == heap + 0x3000, "write after a thaw: %llu pages reported (want 1)", (unsigned long long)n);
    CHECK(mem_freeze(heap) == 1, "refreeze after a write thaw");
    mem_thaw(heap + 0x6000, 0x1000);
    CHECK(!mem_frozen(heap) && mem_freeze(heap) == 0, "mem_thaw did not thaw and hold the region");
    p[0x6000] = 8;
    printf("write-watch: frozen region thawed by a write (reported once) and by mem_thaw (held)\n");

    uint64_t a = g->alias.base + 0x10000;
    CHECK(mem_map_physical(a, 0x4000) == 0, "MapPhysicalMemory");
    mem_written_pages(a, 0x4000, pages, 8);
    *(volatile uint8_t *)(uintptr_t)(a + 0x1000) = 1;
    n = mem_written_pages(a, 0x4000, pages, 8);
    CHECK(n == 1 && pages[0] == a + 0x1000, "alias write: %llu pages reported (want 1)", (unsigned long long)n);
    CHECK(mem_written_pages(a, 0x4000, pages, 8) == 0, "alias: second call reported pages");

    // A region with reserved holes freezes its committed pages only, and a write thaws them without committing a hole.
    uint64_t ra = a & ~0x1fffffull;
    CHECK(mem_freeze(ra) == 1 && mem_frozen(ra), "freeze of a 2 MiB region with reserved holes");
    CHECK(plat_mem_state(a, a + 1, &end) == PLAT_PROT_READ, "its committed pages are not read-only");
    *(volatile uint8_t *)(uintptr_t)(a + 0x2000) = 9;
    CHECK(!mem_frozen(ra) && *(volatile uint8_t *)(uintptr_t)(a + 0x2000) == 9, "a write did not thaw the region with holes");
    CHECK(plat_mem_state(a, a + 0x10000, &end) == PLAT_PROT_RW && end == a + 0x4000, "the thawed pages are not read-write");
    CHECK(plat_mem_state(a + 0x4000, a + 0x4001, &end) == PLAT_MEM_RESERVED, "a hole got committed");
    printf("write-watch: region with reserved holes frozen and thawed by a write\n");
    CHECK(mem_unmap_physical(a, 0x4000) == 0, "UnmapPhysicalMemory");
    CHECK(mem_written_pages(RT_KERNEL_BASE, 0x1000, pages, 8) == UINT64_MAX, "kernel region is not write-watched");

#if defined(__linux__)
    {
        WwStressCtx ctx = { heap + 0x10000, plat_file_open("/dev/zero"), 0, 0, 0 };
        if (ctx.f) {
            int ok = plat_thread_spawn(ww_stress_fn, &ctx, "ww_stress", 0);
            if (ok) {
                while (!__atomic_load_n(&ctx.done, __ATOMIC_ACQUIRE))
                    mem_written_pages(heap + 0x10000, 0x4000, pages, 8);
            }
            plat_file_close(ctx.f);
            CHECK(ok, "ww_stress thread spawn");
            CHECK(ctx.bad_rc == 0 && ctx.bad_data == 0,
                  "write-watch pread race: bad_rc=%d bad_data=%d", ctx.bad_rc, ctx.bad_data);
            printf("write-watch: 5000 concurrent pread + reset iterations ok\n");
        }
        {
            uint64_t mt_base = heap + 0x20000;
            uint64_t mt_size = (uint64_t)WW_MT_PAGES * 4096u;
            WwMtCtx wctx[WW_MT_THREADS] = {0};
            uint64_t obs[WW_MT_PAGES][WW_MT_THREADS] = {0};
            // Arm all pages before starting writers.
            for (uint64_t i = 0; i < WW_MT_PAGES; i++)
                *(volatile uint64_t *)(uintptr_t)(mt_base + i * 4096u) = 0;
            while (mem_written_pages(mt_base, mt_size, pages, 8) > 0) {}
            for (int t = 0; t < WW_MT_THREADS; t++) {
                wctx[t].base = mt_base;
                wctx[t].tid = t;
                CHECK(plat_thread_spawn(ww_mt_fn, &wctx[t], "ww_mt", 0), "ww_mt thread spawn");
            }
            for (;;) {
                int all_done = 1;
                for (int t = 0; t < WW_MT_THREADS; t++)
                    if (!__atomic_load_n(&wctx[t].done, __ATOMIC_ACQUIRE)) all_done = 0;
                uint64_t off = 0;
                while (off < mt_size) {
                    uint64_t chunk = mt_size - off;
                    uint64_t qn = mem_written_pages(mt_base + off, chunk, pages, 8);
                    for (uint64_t k = 0; k < qn; k++) {
                        uint64_t pg = (pages[k] - mt_base) / 4096u;
                        if (pg < WW_MT_PAGES) {
                            for (int t = 0; t < WW_MT_THREADS; t++) {
                                uint64_t v = __atomic_load_n((volatile uint64_t *)(uintptr_t)(pages[k] + (uint64_t)t * 8u), __ATOMIC_ACQUIRE);
                                if (v > obs[pg][t]) obs[pg][t] = v;
                            }
                        }
                    }
                    if (qn < 8) break;
                    off = (pages[qn - 1] - mt_base) + 4096u;
                }
                if (all_done) break;
            }
            // One final drain after all writers finished.
            uint64_t off = 0;
            while (off < mt_size) {
                uint64_t qn = mem_written_pages(mt_base + off, mt_size - off, pages, 8);
                for (uint64_t k = 0; k < qn; k++) {
                    uint64_t pg = (pages[k] - mt_base) / 4096u;
                    if (pg < WW_MT_PAGES) {
                        for (int t = 0; t < WW_MT_THREADS; t++) {
                            uint64_t v = __atomic_load_n((volatile uint64_t *)(uintptr_t)(pages[k] + (uint64_t)t * 8u), __ATOMIC_ACQUIRE);
                            if (v > obs[pg][t]) obs[pg][t] = v;
                        }
                    }
                }
                if (qn < 8) break;
                off = (pages[qn - 1] - mt_base) + 4096u;
            }
            int missed = 0;
            for (int pg = 0; pg < WW_MT_PAGES; pg++)
                for (int t = 0; t < WW_MT_THREADS; t++)
                    if (obs[pg][t] != wctx[t].last[pg]) missed++;
            CHECK(missed == 0, "write-watch mt stress: %d page/thread pairs missed a write", missed);
            printf("write-watch: %d-thread random write + reset stress (%d iters each): missed=%d\n",
                   WW_MT_THREADS, WW_MT_ITERS, missed);
        }
    }
#endif

    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("write-watch: alias page written once: reported once, then not\n");
}

int probe_gfx_abi(void) {
    size_t tightest = (size_t)-1;
    const char *tight = "";
    for (size_t i = 0; i < sizeof fits / sizeof *fits; i++) {
        const Fit *f = &fits[i];
        CHECK(f->size <= f->reserved, "%s is %zu bytes, %s reserves %zu", f->host, f->size, f->nvn, f->reserved);
        if (f->reserved - f->size < tightest) tightest = f->reserved - f->size, tight = f->host;
    }
    CHECK(sizeof(NVNtextureView) == 16 && sizeof(view_t) == 16, "NVNtextureView %zu bytes, view_t %zu (want 16)",
          sizeof(NVNtextureView), sizeof(view_t));
    CHECK(offsetof(cmd_t, guest_flags) == 0x10, "cmd_t.guest_flags at %#zx (want 0x10)", offsetof(cmd_t, guest_flags));
    CHECK(offsetof(cmd_t, inline_cur) == 0x20, "cmd_t.inline_cur at %#zx (want 0x20)", offsetof(cmd_t, inline_cur));
    CHECK(offsetof(cmd_t, inline_end) == 0x28, "cmd_t.inline_end at %#zx (want 0x28)", offsetof(cmd_t, inline_end));
    CHECK(sizeof(samp_par) == 44, "samp_par is %zu bytes (want 44)", sizeof(samp_par));
    CHECK(RT_GPU_VA_BASE >= RT_ADDRESS_LIMIT && RT_GPU_VA_SIZE > 0, "GPU address window %#llx overlaps the guest space",
          RT_GPU_VA_BASE);
    check_write_watch();
    printf("gfx-abi: %d layouts fit (tightest %s, %zu bytes spare), cmd_t inline fields at +0x20/+0x28, "
           "NVNtextureView %zu bytes\n", (int)(sizeof fits / sizeof *fits), tight, tightest, sizeof(NVNtextureView));
    if (failures) {
        printf("probe-gfx-abi: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-gfx-abi: passed\n");
    return 0;
}
