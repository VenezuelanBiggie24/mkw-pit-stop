#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "hangwatch.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"
#ifdef __PROSPERO__
#include "platform/ps5_guest_memory.h"
#endif

uint64_t vk_sparse_bytes(void);

// Atomic snapshot of Vulkan/VMA memory totals so signal-context OOM logging
// can report them without taking VMA or texture mutexes.
static struct {
    uint64_t vma_block[4], vma_used[4], drv_usage[4];
    uint32_t n_heaps;
    uint64_t tex_own, tex_rel, sparse, shadow, placed;
} ps5_vk_snap;

static void ps5_vk_snap_refresh(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    const char *prev = hang_mark("census: vmaGetHeapBudgets");
    vmaGetHeapBudgets(c->vma, vb);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
    hang_mark("census: vkGetPhysicalDeviceMemoryProperties2");
    if (c->has_memory_budget) vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    hang_mark(prev);
    uint32_t nh = c->mem_props.memoryHeapCount;
    if (nh > 4) nh = 4;
    for (uint32_t h = 0; h < nh; h++) {
        __atomic_store_n(&ps5_vk_snap.vma_block[h], vb[h].statistics.blockBytes, __ATOMIC_RELAXED);
        __atomic_store_n(&ps5_vk_snap.vma_used[h], vb[h].statistics.allocationBytes, __ATOMIC_RELAXED);
        __atomic_store_n(&ps5_vk_snap.drv_usage[h], c->has_memory_budget ? b.heapUsage[h] : 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&ps5_vk_snap.n_heaps, nh, __ATOMIC_RELAXED);
    VkTexCensus t = {0};
    const char *prev2 = hang_mark("census: vk_texture_census");
    vk_texture_census(&t);
    hang_mark(prev2);
    __atomic_store_n(&ps5_vk_snap.tex_own, t.bytes_live, __ATOMIC_RELAXED);
    __atomic_store_n(&ps5_vk_snap.tex_rel, t.bytes_released, __ATOMIC_RELAXED);
    __atomic_store_n(&ps5_vk_snap.sparse, vk_sparse_bytes(), __ATOMIC_RELAXED);
    __atomic_store_n(&ps5_vk_snap.shadow, (uint64_t)__atomic_load_n(&s_live_bytes, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&ps5_vk_snap.placed, (uint64_t)__atomic_load_n(&placed_bytes, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
}

// Reads the snapshot only: vk_mem_census refreshes it every 15 frames. The census is logged from
// vk_sparse.c under the queue lock, and a refresh there took VMA's mutex and the texture lock while the guest's main
// thread held one of them and waited for the queue lock (console run 7: a hang after "before-sparse-bind").
void vk_mem_ps5_census(char *out, size_t cap, int in_signal) {
    (void)in_signal;
    uint32_t nh = __atomic_load_n(&ps5_vk_snap.n_heaps, __ATOMIC_RELAXED);
    if (!nh) {
        snprintf(out, cap, "vk=uninit");
        return;
    }
    size_t k = (size_t)snprintf(out, cap, "vk=vma(");
    for (uint32_t h = 0; h < nh && k < cap; h++) {
        uint64_t blk = __atomic_load_n(&ps5_vk_snap.vma_block[h], __ATOMIC_RELAXED) >> 20;
        uint64_t drv = __atomic_load_n(&ps5_vk_snap.drv_usage[h], __ATOMIC_RELAXED) >> 20;
        k += (size_t)snprintf(out + k, k < cap ? cap - k : 0, "%sh%u:%llu/%llu", h ? " " : "", h,
                              (unsigned long long)blk, (unsigned long long)drv);
    }
    snprintf(out + k, k < cap ? cap - k : 0, ") sparse=%llu tex=%llu(+%llurel) shadow=%llu placed=%llu MiB",
             (unsigned long long)(__atomic_load_n(&ps5_vk_snap.sparse, __ATOMIC_RELAXED) >> 20),
             (unsigned long long)(__atomic_load_n(&ps5_vk_snap.tex_own, __ATOMIC_RELAXED) >> 20),
             (unsigned long long)(__atomic_load_n(&ps5_vk_snap.tex_rel, __ATOMIC_RELAXED) >> 20),
             (unsigned long long)(__atomic_load_n(&ps5_vk_snap.shadow, __ATOMIC_RELAXED) >> 20),
             (unsigned long long)(__atomic_load_n(&ps5_vk_snap.placed, __ATOMIC_RELAXED) >> 20));
}

// ---------------------------------------------------------------- census
static volatile int64_t traffic[VK_MCLS_N][VK_MUSE_N];

// Every caller is the replay (vk_exec*, under vk_sync_submit's lock), about ten adds per draw: one writer at a time, so
// a plain add, not a locked one. An add racing the census's reset may be lost.
void vk_mem_count(int cls, int use, uint64_t bytes) {
    if ((unsigned)cls < VK_MCLS_N && (unsigned)use < VK_MUSE_N)
        __atomic_store_n(&traffic[cls][use], __atomic_load_n(&traffic[cls][use], __ATOMIC_RELAXED) + (int64_t)bytes,
                         __ATOMIC_RELAXED);
}

void flag_names(uint32_t f, char *buf, size_t n) {
    static const char *const names[] = {"CPU_NO_ACCESS", "CPU_UNCACHED", "CPU_CACHED", "GPU_NO_ACCESS", "GPU_UNCACHED",
                                        "GPU_CACHED", "SHADER_CODE", "COMPRESSIBLE", "PHYSICAL", "VIRTUAL"};
    size_t k = 0;
    buf[0] = 0;
    for (int i = 0; i < 10; i++)
        if (f >> i & 1) k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%s%s", k ? "|" : "", names[i]);
    if (!k) snprintf(buf, n, "none");
}

// One line per (kind, flags) group of the live pools, then the bytes the replay bound or copied per frame by pool
// class since the previous census (the first one: since frame 1200). Logged at frame 1800 (in the world from the
// run-game saves), at frame 600, and with MKW_TRACE=perf every 1800 frames.



#define GROW_TOP 8

static void log_mem_census(uint64_t frame, int force) {
    static uint64_t window_start, window_ns;
    if (!force) {
        if (frame == 1200) {   // loading is over: the first window starts here
            for (int c = 0; c < VK_MCLS_N; c++)
                for (int u = 0; u < VK_MUSE_N; u++) plat_xchg64(&traffic[c][u], 0);
            vk_pool_fini_stats(NULL, NULL, NULL, NULL, NULL, NULL, NULL, 1);
            window_start = frame;
            window_ns = plat_now_ns();
            return;
        }
        // Frame 600 as well: runs on slow machines end before 1800.
        if (frame != 600 && (frame % 1800 || !(frame == 1800 || (rt_trace & RT_TRACE_PERF)))) return;
    }
    typedef struct { int kind; uint32_t flags; uint64_t n, bytes, backed; } Group;
    Group g[64];
    int ng = 0;
    uint64_t total[4] = {0}, count[4] = {0}, cna_total = 0, cna_backed = 0;
    // The pools whose backed bytes grew most since the previous census, and the growth of all of them: the guest's
    // backed memory rose by ~870 MiB over three races in run nocache (16/11/2026), most of it in CPU_CACHED|GPU_CACHED
    // pools, and these lines name which.
    typedef struct { uint64_t lo, size, before, after; uint32_t flags; int used; } Grow;
    Grow top[GROW_TOP];
    int ntop = 0;
    int64_t grew = 0, shrank = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        PoolRec *r = pool_index.e[i].buf;
        uint64_t b = r->kind != POOL_VIRT ? plat_mem_backed_bytes(r->lo, r->size) : 0;
        uint64_t was = r->census_backed;
        r->census_backed = b;
        if (b > was) grew += (int64_t)(b - was);
        else shrank += (int64_t)(was - b);
        int k = -1;
        if (b > was && ntop < GROW_TOP) k = ntop++;
        else if (b > was && b - was > top[GROW_TOP - 1].after - top[GROW_TOP - 1].before) k = GROW_TOP - 1;
        if (k >= 0) {
            top[k] = (Grow){r->lo, r->size, was, b, r->flags, r->used != 0};
            for (; k > 0 && top[k].after - top[k].before > top[k - 1].after - top[k - 1].before; k--) {
                Grow t = top[k];
                top[k] = top[k - 1], top[k - 1] = t;
            }
        }
        int j = 0;
        while (j < ng && (g[j].kind != r->kind || g[j].flags != r->flags)) j++;
        if (j == ng && ng < 64) g[ng++] = (Group){r->kind, r->flags, 0, 0, 0};
        if (j < ng) g[j].n++, g[j].bytes += r->size, g[j].backed += b;
        total[r->kind] += r->size, count[r->kind]++;
        if (r->kind != POOL_VIRT && (r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS))
            cna_total += r->size, cna_backed += b;
    }
    plat_mutex_unlock_shared(&pool_lock);
    char gb[384];   // with MKW_PACK's ", packed ..." note
    plat_mem_backed_summary(gb, sizeof gb);
    if (gb[0]) rt_log("mem: guest %s", gb);
    char pk[1024];   // MKW_PACK: what is packed, and the packs, unpacks and skips since the last census
    plat_mem_pack_summary(pk, sizeof pk);
    if (pk[0]) rt_log("mem: pack %s", pk);
    rt_log("mem: pools' backed bytes since the last census: +%.1f MiB, -%.1f MiB; most grown:", grew / 1048576.0,
           shrank / 1048576.0);
    for (int k = 0; k < ntop; k++) {
        char f[160];
        flag_names(top[k].flags, f, sizeof f);
        rt_log("mem:   pool %#llx %.1f MiB %s%s: backed %.1f -> %.1f MiB", (unsigned long long)top[k].lo,
               top[k].size / 1048576.0, f, top[k].used ? "" : " (never resolved by the GPU)", top[k].before / 1048576.0,
               top[k].after / 1048576.0);
        // What a big pool holds: run memgrow (16/11/2026) found one 2427 MiB pool behind half the guest's growth.
        if (top[k].size >= (64ull << 20)) {
            VkTexRange t;
            vk_texture_range_census(top[k].lo, top[k].lo + top[k].size, &t);
            const double M = 1048576.0;
            rt_log("mem:     its textures: %llu storages %.1f MiB (backed %.1f MiB of the pool's %.1f); entries %llu live "
                   "(%llu idle) %llu released, %llu never uploaded, %llu sparse; device memory %.1f MiB: ASTC->BC7 %llu, "
                   "%.1f MiB from %.1f stored; ASTC->RGBA8 %llu, %.1f MiB from %.1f; other %llu, %.1f MiB from %.1f",
                   (unsigned long long)t.n, t.storage / M, t.backed / M, top[k].after / M, (unsigned long long)t.live,
                   (unsigned long long)t.idle, (unsigned long long)t.released, (unsigned long long)t.never_up,
                   (unsigned long long)t.sparse_n, t.vk_bytes / M, (unsigned long long)t.astc_bc7_n, t.astc_bc7_vk / M,
                   t.astc_bc7_storage / M, (unsigned long long)t.astc_rgba_n, t.astc_rgba_vk / M,
                   t.astc_rgba_storage / M, (unsigned long long)t.other_n, t.other_vk / M, t.other_storage / M);
        }
    }
    rt_log("mem: census at frame %llu: import %llu pools %.1f MiB (%d without a GPU buffer), physical %llu pools %.1f MiB, "
           "virtual %llu pools %.1f MiB of address space",
           (unsigned long long)frame, (unsigned long long)count[POOL_IMPORT], total[POOL_IMPORT] / 1048576.0,
           n_import_failures, (unsigned long long)count[POOL_PHYS], total[POOL_PHYS] / 1048576.0,
           (unsigned long long)count[POOL_VIRT], total[POOL_VIRT] / 1048576.0);
    static const char *const kinds[] = {"", "import", "physical", "virtual"};
    for (int j = 0; j < ng; j++) {
        char f[160];
        flag_names(g[j].flags, f, sizeof f);
        rt_log("mem:   %s %#x %s: %llu pools, %.1f MiB (backed %.1f MiB)", kinds[g[j].kind], g[j].flags, f,
               (unsigned long long)g[j].n, g[j].bytes / 1048576.0, g[j].backed / 1048576.0);
    }
    uint64_t cna_n = 0, cna_b = 0, cna_r = 0;
    vk_mem_cna_stats(&cna_n, &cna_b, &cna_r);
    rt_log("mem: CPU_NO_ACCESS backed: %.1f MiB of %.1f MiB (GPU-owned textures released %.1f MiB of %.1f MiB, %llu "
           "textures; MKW_GPU_ONLY_RELEASE)",
           cna_backed / 1048576.0, cna_total / 1048576.0, cna_r / 1048576.0, cna_b / 1048576.0,
           (unsigned long long)cna_n);
    uint64_t now_ns = plat_now_ns();
    double mins = window_ns && now_ns > window_ns ? (double)(now_ns - window_ns) / 60e9 : 0.0;
    uint64_t pf_n = 0, pf_b = 0, pf_bk = 0, pf_rel = 0, pf_ov = 0, pf_wb = 0, pf_wbk = 0;
    vk_pool_fini_stats(&pf_n, &pf_b, &pf_bk, &pf_rel, &pf_ov, &pf_wb, &pf_wbk, !force);
    rt_log("mem: pool fini: %llu pools %.1f MiB (%.1f MiB backed; window %.1f MiB/min, %.1f MiB/min backed; "
           "%llu overlap skipped, %.1f MiB released)",
           (unsigned long long)pf_n, pf_b / 1048576.0, pf_bk / 1048576.0,
           mins > 0.0 ? (pf_wb / 1048576.0) / mins : 0.0, mins > 0.0 ? (pf_wbk / 1048576.0) / mins : 0.0,
           (unsigned long long)pf_ov, pf_rel / 1048576.0);
    int cb_lp = 0, cb_sp = 0, cb_lc = 0, cb_sc = 0, dc_lp = 0, dc_rp = 0;
    vk_exec_cb_dpool_stats(&cb_lp, &cb_sp, &cb_lc, &cb_sc);
    vk_dcache_dpool_stats(&dc_lp, &dc_rp);
    uint64_t drv_sum = 0, vma_sum = 0;
    uint32_t nh = __atomic_load_n(&ps5_vk_snap.n_heaps, __ATOMIC_RELAXED);
    for (uint32_t h = 0; h < nh; h++) {
        drv_sum += __atomic_load_n(&ps5_vk_snap.drv_usage[h], __ATOMIC_RELAXED);
        vma_sum += __atomic_load_n(&ps5_vk_snap.vma_block[h], __ATOMIC_RELAXED);
    }
    uint64_t sp_b = __atomic_load_n(&ps5_vk_snap.sparse, __ATOMIC_RELAXED);
    int64_t radv_est = (int64_t)drv_sum - (int64_t)vma_sum - (int64_t)sp_b - (int64_t)total[POOL_IMPORT];
    if (radv_est < 0) radv_est = 0;
    rt_log("mem: radv/exec: cb_dpools=%d (+%d spare) cb_chunks=%d (+%d spare), dcache_dpools=%d (+%d retiring), "
           "drv_usage=%.0f MiB, vma=%.0f MiB, sparse=%.0f MiB, import=%.0f MiB, radv_est=%.0f MiB",
           cb_lp, cb_sp, cb_lc, cb_sc, dc_lp, dc_rp, drv_sum / 1048576.0, vma_sum / 1048576.0,
           sp_b / 1048576.0, total[POOL_IMPORT] / 1048576.0, radv_est / 1048576.0);
    if (!force) {
        uint64_t frames = window_start && frame > window_start ? frame - window_start : frame;
        static const char *const cls[] = {"", "host CPU_NO_ACCESS", "host CPU_UNCACHED", "host CPU_CACHED", "device", "scratch"};
        rt_log("mem: traffic per frame over %llu frames, MiB (bound ranges, not bytes read): class: vertex index uniform "
               "storage indirect copy-src gpu-write cpu-read", (unsigned long long)frames);
        for (int c = 1; c < VK_MCLS_N; c++) {
            double v[VK_MUSE_N];
            for (int u = 0; u < VK_MUSE_N; u++) v[u] = plat_xchg64(&traffic[c][u], 0) / 1048576.0 / (double)(frames ? frames : 1);
            rt_log("mem:   %s: %.2f %.2f %.2f %.2f %.3f %.2f %.2f %.3f", cls[c], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
        window_start = frame;
        window_ns = now_ns;
    }
}

void vk_mem_census_now(void) { vram_log_now(); log_mem_census(0, 1); }   // with the texture census line (vram_log)

void vk_oom_census(const char *what) {
    static volatile int32_t once;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) return;
    rt_log("vk: === OOM census (%s) ===", what ? what : "alloc");
    ps5_vk_snap_refresh();
    vram_log_now();
    log_mem_census(0, 1);
    plat_mem_census_log("oom");
}

void vk_mem_census(uint64_t frame) {
    if (frame == 1 || frame % 15 == 0) ps5_vk_snap_refresh();
    mem_early_log(frame);   // MKW_EARLY_READ
    if ((rt_trace & RT_TRACE_PERF) && frame % 150 == 0) vram_log(frame);
    if (vk_dirty_census && !vk_mirror_on) dirty_frame(frame);
    if ((rt_trace & RT_TRACE_PERF) ? frame % 60 == 0 : frame == 600 || frame == 1800)
        mirror_log(frame), shadow_log(frame);   // mirror_log also logs the write-watch line
    log_mem_census(frame, 0);
}

static int g_discrete;   // vk_mem_class_log: the device class is discrete

// MKW_VRAM_USED (default: vma on a discrete GPU, driver otherwise; "ab"): what counts as VRAM in use against the
// driver's budget. The driver's heap usage on NVIDIA's Windows driver includes the host memory the runtime imports
// (VK_EXT_external_memory_host): in a race on a GTX 1050 Ti it said 7 657 MiB of device-local memory in use for a
// 3 437 MiB budget, while Windows counted 3 517 MiB of dedicated memory and VMA held 3 335 (wrace4, wcnt3,
// 06/11/2026); no mirror could fit and 700 pools stayed in host memory, read over PCIe. "vma": VMA's blocks on the
// device-local heaps, what the runtime itself put there. On a UMA device (one memory) the driver's figure is kept.
static int vram_used_vma(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_get("MKW_VRAM_USED");
        mode = e && !strcmp(e, "driver") ? 0 : e && !strcmp(e, "vma") ? 1 : e && !strcmp(e, "ab") ? 2 : g_discrete;
        rt_log("mem: VRAM in use counted from %s (MKW_VRAM_USED)",
               mode == 2 ? "VMA in the B blocks, the driver in the A blocks" : mode ? "VMA's device-local blocks" : "the driver's heap usage");
    }
    return RT_KNOB_ON(mode);
}

int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact) {
    VkCtx *c = vk_ctx();
    *used = *budget = 0, *exact = 0;
    if (!c) return 0;
    const VkPhysicalDeviceMemoryProperties *mp = &c->mem_props;
    if (c->has_memory_budget && vram_used_vma()) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
        VmaBudget vb[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(c->vma, vb);
        for (uint32_t h = 0; h < p2.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if (p2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                *used += vb[h].statistics.blockBytes, *budget += b.heapBudget[h];
        *exact = 1;
        return *budget != 0;
    }
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
        for (uint32_t h = 0; h < p2.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if (p2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                *used += b.heapUsage[h], *budget += b.heapBudget[h];
        *exact = 1;
        return *budget != 0;
    }
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    for (uint32_t h = 0; h < mp->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            *used += vb[h].statistics.blockBytes, *budget += mp->memoryHeaps[h].size;
    return *budget != 0;
}

// With MKW_TRACE=perf every 600 frames: what holds device-local memory, to see what grows over a long session.
// VMA's blocks and allocations on the device-local heaps, texture entries with their own memory, the VRAM mirrors and
// the objects waiting for their last submission to retire.
void vram_log_now(void) { vram_log(0); }

void vram_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t blocks = 0, block_bytes = 0, allocs = 0, alloc_bytes = 0;
    char heaps[160] = "";
    for (uint32_t h = 0, o = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            blocks += vb[h].statistics.blockCount, block_bytes += vb[h].statistics.blockBytes,
                allocs += vb[h].statistics.allocationCount, alloc_bytes += vb[h].statistics.allocationBytes;
        if (o < sizeof heaps - 40)   // every heap, also the host one (UMA: both are system RAM)
            o += (uint32_t)snprintf(heaps + o, sizeof heaps - o, " heap%u %.0f/%.0f MiB", h,
                                    vb[h].statistics.blockBytes / 1048576.0, vb[h].usage / 1048576.0);
    }
    VkTexCensus t;
    vk_texture_census(&t);
    int cb_lp = 0, cb_sp = 0, cb_lc = 0, cb_sc = 0, dc_lp = 0, dc_rp = 0;
    vk_exec_cb_dpool_stats(&cb_lp, &cb_sp, &cb_lc, &cb_sc);
    vk_dcache_dpool_stats(&dc_lp, &dc_rp);
    const double M = 1048576.0;
    rt_log("mem: vram frame %llu: vma %llu blocks %.0f MiB, %llu allocations %.0f MiB; textures live %llu (own memory %llu, "
           "%.0f MiB), released %llu (own memory %llu, %.0f MiB); mirrors %lld %.0f MiB; releases pending %d; vma/driver usage%s; "
           "placed pools %lld %.0f MiB, shadow %lld %.0f MiB, physical chunks %lld %.0f MiB (%lld failed); "
           "dpools cb %d(+%dspare) dc %d(+%dret); own texture "
           "memory: ASTC decoded %llu %.0f MiB (RGBA8 %.0f), targets %llu %.0f MiB, images over 64 MiB %.0f MiB",
           (unsigned long long)frame, (unsigned long long)blocks, block_bytes / M, (unsigned long long)allocs,
           alloc_bytes / M, (unsigned long long)t.live, (unsigned long long)t.own_live, t.bytes_live / M,
           (unsigned long long)t.released, (unsigned long long)t.own_released, t.bytes_released / M, (long long)m_live_n,
           m_live_bytes / M, dead_pending(), heaps, (long long)placed_n, placed_bytes / M, (long long)s_live_n,
           s_live_bytes / M, (long long)phys_backed_n, phys_backed_bytes / M, (long long)phys_failed_n,
           cb_lp, cb_sp, dc_lp, dc_rp,
           (unsigned long long)t.astc_n, t.astc_bytes / M, t.astc_rgba_bytes / M, (unsigned long long)t.target_n, t.target_bytes / M,
           t.big_bytes / M);
    uint64_t stg_bytes = vk_upload_stg_bytes(), usnap_n = 0, usnap_bytes = 0;
    vk_usnap_stats(&usnap_n, &usnap_bytes);
    double h0_blk = c->mem_props.memoryHeapCount ? vb[0].statistics.blockBytes / M : 0.0;
    double h0_use = c->mem_props.memoryHeapCount ? vb[0].usage / M : 0.0;
    rt_log("mem: heap0 frame %llu: shadow %lld %.0f MiB, staging %.0f MiB, cb_chunks %d(+%d spare) %.0f MiB, "
           "usnap %llu %.0f MiB (vma heap0 %.0f/%.0f MiB)",
           (unsigned long long)frame, (long long)s_live_n, s_live_bytes / M, stg_bytes / M,
           cb_lc, cb_sc, (double)cb_lc * 4.0, (unsigned long long)usnap_n, usnap_bytes / M, h0_blk, h0_use);
    rt_log("mem: tex frame %llu: live own %llu %.0f MiB (gpu_owned %llu %.0f, mirrored_ro %llu %.0f, astc_bc %llu %.0f, "
           "astc_rgba %.0f [arr %.0f], other_gpu %llu %.0f); dup_addr swz %llu %.0f MiB other %llu %.0f MiB; "
           "idle_evict %llu %.0f MiB",
           (unsigned long long)frame, (unsigned long long)t.own_live, t.bytes_live / M,
           (unsigned long long)t.gpu_owned_n, t.gpu_owned_bytes / M,
           (unsigned long long)t.mirrored_ro_n, t.mirrored_ro_bytes / M,
           (unsigned long long)t.astc_n, (t.astc_bytes - t.astc_rgba_bytes) / M,
           t.astc_rgba_bytes / M, t.astc_rgba_arr_bytes / M,
           (unsigned long long)t.other_gpu_n, t.other_gpu_bytes / M,
           (unsigned long long)t.dup_swz_n, t.dup_swz_bytes / M,
           (unsigned long long)t.dup_other_n, t.dup_other_bytes / M,
           (unsigned long long)t.idle_evict_n, t.idle_evict_bytes / M);
    vk_sparse_vram_log(frame);
}

// MKW_MEMORY and the knobs of every part of the memory layer, once the device class is known.
static void mirror_mode(int discrete) {
    const char *e = cfg_value("MKW_MEMORY");
    int mode = discrete ? 1 : 0;   // 0 import, 1 mirror, 2 verify
    const char *why = discrete ? "auto, discrete gpu" : "auto, uma";
    if (strcmp(e, "auto")) {
        if (!strcmp(e, "import")) mode = 0, why = "MKW_MEMORY=import";
        else if (!strcmp(e, "mirror")) mode = 1, why = "MKW_MEMORY=mirror";
        else if (!strcmp(e, "verify")) mode = 2, why = "MKW_MEMORY=verify";
        else rt_log("mem: MKW_MEMORY=%s is not import, mirror, verify or auto: auto", e);
    }
    vk_mirror_on = mode > 0;
    mirror_verify = mode == 2;
    const char *g = cfg_get("MKW_MIRROR_GAP");
    mirror_gap = g && *g ? atoi(g) : 2;
    if (mirror_gap < 0) mirror_gap = 0;
    // Submissions a region stays unwritten before it is frozen. 60 rather than 300: in a fight 100 of the 257 MiB
    // queried a frame lay in regions idle for 30-299 submissions, and a thaw by a write costs about 60 us.
    const char *fi = cfg_get("MKW_FREEZE_IDLE");
    freeze_idle = fi && *fi ? atoi(fi) : 60;
    if (freeze_idle < 0) freeze_idle = 0;
    const char *sh = cfg_get("MKW_SHADOW");
#ifdef _WIN32
    shadow_on = sh && *sh && strcmp(sh, "0");
#else
    shadow_on = !sh || !*sh || strcmp(sh, "0");
#endif
    if (shadow_on) rt_log("mem: shadow storage: CPU-visible pools are host-visible copies of guest memory, not imports%s",
                          sh && *sh ? " (MKW_SHADOW)" : "");
    const char *dc = cfg_get("MKW_DIRTY_CENSUS");
    vk_dirty_census = (rt_trace & RT_TRACE_PERF) && dc && *dc && strcmp(dc, "0");
    const char *rt = cfg_get("MKW_READ_TRAPS");
    read_traps = !rt || !*rt || strcmp(rt, "0");
    wb_defer = rt_knob("SHADOW_WB_DEFER", "1");
    if (shadow_on) rt_log("mem: fence write-backs %s shadow_lock (MKW_SHADOW_WB_DEFER)",
                          wb_defer == 2 ? "queued while another thread holds it in the B blocks; else wait for" :
                          wb_defer ? "queued while another thread holds" : "wait for");
    rt_log("mem: mode %s (%s): %s", mode == 2 ? "verify" : mode ? "mirror" : "import", why,
           mode ? "pools used as buffers get a device-local mirror" : "every CPU-visible pool is imported host memory");
}

// Device class: INTEGRATED -> uma; a discrete GPU with a DEVICE_LOCAL heap of 1 GiB or more beside a
// heap that is not device-local (host memory) -> discrete; otherwise uma (import everywhere). Sets the mirror mode.
void vk_mem_class_log(const VkPhysicalDeviceProperties *p, const VkPhysicalDeviceMemoryProperties *m) {
    int big_local = -1, host_heap = -1;
    for (uint32_t h = 0; h < m->memoryHeapCount; h++) {
        if (m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            if (m->memoryHeaps[h].size >= (1ull << 30) && big_local < 0) big_local = (int)h;
        } else if (host_heap < 0) host_heap = (int)h;
    }
    static const char *const types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    int t = (int)p->deviceType;
    const char *tn = t >= 0 && t <= 4 ? types[t] : "unknown";
    const char *cls, *why;
    if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) cls = "uma", why = "integrated gpu";
    else if (p->deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) cls = "uma", why = "not a discrete gpu";
    else if (big_local < 0) cls = "uma", why = "no device-local heap of 1 GiB or more";
    else if (host_heap < 0) cls = "uma", why = "every heap is device-local";
    else cls = "discrete", why = "device-local heap separate from host memory";
    int discrete = cls[0] == 'd';
    g_discrete = discrete;
    char line[2048];
    size_t k = (size_t)snprintf(line, sizeof line, "mem: class %s (%s; device type %d %s, %s)", cls, why, t, tn, p->deviceName);
    for (uint32_t h = 0; h < m->memoryHeapCount; h++)
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; heap %u %.0f MiB flags %#x%s%s", h,
                              m->memoryHeaps[h].size / 1048576.0, m->memoryHeaps[h].flags,
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ? " MULTI_INSTANCE" : "");
    rt_log("%s", line);
    k = (size_t)snprintf(line, sizeof line, "mem: types");
    for (uint32_t i = 0; i < m->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = m->memoryTypes[i].propertyFlags;
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; %u heap %u flags %#x%s%s%s%s%s", i,
                              m->memoryTypes[i].heapIndex, f, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
                              f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " HOST_COHERENT" : "",
                              f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " HOST_CACHED" : "",
                              f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " LAZY" : "");
    }
    rt_log("%s", line);
    mirror_mode(discrete);
#ifdef __PROSPERO__
    ps5_mem_set_vk_census(vk_mem_ps5_census);
#endif
}
