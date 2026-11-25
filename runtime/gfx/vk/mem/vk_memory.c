// Vulkan memory of the graphics layer: NVN memory pools as Vulkan buffers (the pool index, virtual-pool page table and
// GPU address resolution here). The parts, sharing vk_mem_internal.h: vk_mem_back.c (each pool's GPU storage),
// vk_mem_gen.c (page write generations for re-uploads), vk_mem_shadow.c, vk_mem_mirror.c, vk_mem_dirty.c,
// vk_mem_log.c (census, memory mode) and vk_mem_upload.c (the upload batch that vk_texture.c records into).
//
// Behaviour ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/totk_vpool.c (request checks, unmap
// semantics, 64 KiB pages) and nvn_dirty.c (generation stamps: a texture is stale when a covered page carries a
// generation newer than its upload), @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile); and from LAS
// Recomp runtime/nvn/vulkan/exec.c:2094-2131 (vk_pages_changed: every write-watch query resets the OS bits, so each
// query stamps the pages it saw with a new generation shared by all users) and textures.c:240-270 (deferred
// destruction) @ da35b83a (MIT, Copyright (c) 2026 Peiikai). MNG aliases host VM (mach_vm_remap) and protects pages
// (fault handler); here virtual pools are a GPU-side page table over device-local memory and dirty pages come from
// GetWriteWatch (mem_written_pages), so neither is needed.
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
#include "vk_reuse.h"
#include "platform/platform.h"

PlatMutex pool_lock = PLAT_MUTEX_INIT;
// Bumped under pool_lock (exclusive) by every change to the index or a page table: vk_mem_resolve's per-thread hits
// stay valid while it is unchanged.
uint64_t pool_gen = 1;
// Bumped when a buffer use may get another buffer for the same span (a mirror created, refused or forgotten) and at
// every submission's start (mirror syncs are per submission). vk_mem_gen: both together.
uint64_t mirror_gen;
pidx pool_index;                // POOL_IMPORT, POOL_PHYS and POOL_VIRT records by GPU address
static PoolRec **phys_list;            // physical pools by key, for MapVirtual requests
static int nphys, cap_phys;
int32_t n_imports, n_import_failures;

static void index_put(PoolRec *r) {
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    pidx_ent e = {.lo = r->lo, .hi = r->lo + r->size, .base = r->lo, .buf = r, .gpu = r->lo, .key = r->key};
    pidx_remove(&pool_index, r->key, NULL);   // an object initialized again without Finalize
    if (!pidx_insert(&pool_index, &e)) rt_fatal("vk: out of memory for the pool index");
}

static PoolRec *phys_find(const pool_t *key) {
    for (int i = 0; i < nphys; i++) if (phys_list[i]->key == key) return phys_list[i];
    return NULL;
}
// MKW_GPU_ONLY_RELEASE: the guest backing of a CPU_NO_ACCESS pool's ranges whose bytes the runtime never needs from
// guest memory. How data reaches those pools in MKW (14/11/2026):
// - the game writes a pool's contents (BNTX texture blocks, vertex and index data) into its storage and then
//   initializes the pool over it: the bytes are in guest memory at vk_pool_init, and nowhere else;
// - packaged textures (nvnTextureBuilderSetPackagedTextureLayout: tex_mirrored) are uploaded from that storage
//   (vk_tex_upload.c upload: deswizzle, ASTC decode) at first use, after any eviction, and by vk_texture_astc_copy;
// - buffers the GPU reads get their shadow (vk_mem_shadow.c shadow_pool) copied from it at the first resolve, and again
//   after a trim; GPU writes into the pool (copies, clears, storage buffers) are written back into it (vk_mem_wback.c);
// - GPU-owned textures (render targets, depth, multisample: not mirrored) are never read from it: the GPU writes their
//   images, and buffer-to-texture copies go straight into the image (vk_exec_xfer.c).
// So the release is per GPU-owned texture, at its image's creation (vk_mem_cna_gpu_texture), not per pool at init:
// releasing whole pools at init (task 25) read zeros for every packaged texture (run 28: magenta world and menu art).
// shadow_pool fills released ranges with zeros instead of reading them back into backing.
static int gpu_only_release_on(void) {
    static int v = -1;
    if (v < 0) {
#ifdef MKW_PS5_NATIVE
        v = rt_knob("GPU_ONLY_RELEASE", "1");
#else
        v = rt_knob("GPU_ONLY_RELEASE", "0");
#endif
    }
    return RT_KNOB_ON(v);
}

#define CNA_POOLS 256
#define CNA_RELS 4096
static PlatMutex cna_lock = PLAT_MUTEX_INIT;   // serializes release with publication and pool removal
static struct { uint64_t lo, hi; } cna_pool[CNA_POOLS], cna_rel[CNA_RELS];
static int ncna_pool, ncna_rel;
static uint64_t cna_n, cna_bytes, cna_released, cna_lost;

void vk_mem_cna_stats(uint64_t *n, uint64_t *bytes, uint64_t *released) {
    plat_mutex_lock(&cna_lock);
    if (n) *n = cna_n;
    if (bytes) *bytes = cna_bytes;
    if (released) *released = cna_released;
    plat_mutex_unlock(&cna_lock);
}

static void cna_pool_add(uint64_t lo, uint64_t size) {
    plat_mutex_lock(&cna_lock);
    if (ncna_pool < CNA_POOLS) cna_pool[ncna_pool].lo = lo, cna_pool[ncna_pool++].hi = lo + size;
    plat_mutex_unlock(&cna_lock);
}

static void cna_pool_remove(uint64_t lo, uint64_t size) {   // its released ranges go with it
    plat_mutex_lock(&cna_lock);
    for (int i = 0; i < ncna_pool; i++)
        if (cna_pool[i].lo == lo && cna_pool[i].hi == lo + size) { cna_pool[i] = cna_pool[--ncna_pool]; break; }
    for (int i = 0; i < ncna_rel;)
        if (cna_rel[i].lo >= lo && cna_rel[i].hi <= lo + size) cna_rel[i] = cna_rel[--ncna_rel];
        else i++;
    plat_mutex_unlock(&cna_lock);
}

// Called with cna_lock held. Adjacent native units of one release share an entry.
static int cna_note_discard(uint64_t lo, uint64_t hi) {
    if (ncna_rel && cna_rel[ncna_rel - 1].hi == lo)
        cna_rel[ncna_rel - 1].hi = hi;
    else
        cna_rel[ncna_rel].lo = lo, cna_rel[ncna_rel++].hi = hi;
    return ncna_rel < CNA_RELS;
}

// Publish the units whose contents were actually discarded. Imported and watched units can keep their backing,
// and partial native units at either end are never released. Shadows must copy the original bytes there.
void vk_mem_cna_gpu_texture(uint64_t lo, uint64_t size) {
    if (!lo || !size || size > UINT64_MAX - lo || !gpu_only_release_on()) return;
    int ok = 0;
    plat_mutex_lock(&cna_lock);
    for (int i = 0; i < ncna_pool && !ok; i++) ok = lo >= cna_pool[i].lo && lo + size <= cna_pool[i].hi;
    for (int i = 0; i < ncna_rel && ok; i++) ok = !(cna_rel[i].lo < lo + size && lo < cna_rel[i].hi);
    if (ok && ncna_rel < CNA_RELS) {
        cna_n++, cna_bytes += size;
        uint64_t freed = plat_mem_release_backing(lo, size, cna_note_discard);
        cna_released += freed;
        plat_mutex_unlock(&cna_lock);
        static volatile int32_t n_log;   // every candidate of a session (task 32 saw 6) and how much each gave back
        if ((rt_trace & RT_TRACE_PERF) && plat_inc32(&n_log) <= 64)
            rt_log("mem: GPU-only backing: candidate [%#llx,%#llx), reclaimed %#llx bytes", (unsigned long long)lo,
                   (unsigned long long)(lo + size), (unsigned long long)freed);
        return;
    }
    plat_mutex_unlock(&cna_lock);
}

// A packaged (CPU-provided) texture whose storage overlaps a released range: its bytes are gone. 1 when it does
// (counted; the caller logs it). It should never happen: GPU-owned and packaged textures do not share storage in MKW.
int vk_mem_cna_lost(uint64_t lo, uint64_t size) {
    int hit = 0;
    plat_mutex_lock(&cna_lock);
    for (int i = 0; i < ncna_rel && !hit; i++) hit = cna_rel[i].lo < lo + size && lo < cna_rel[i].hi;
    if (hit) cna_lost++;
    plat_mutex_unlock(&cna_lock);
    return hit;
}

// shadow_pool: [lo, hi) copied from guest memory into dst, the released ranges as zeros (reading them would back
// them again).
void vk_mem_cna_copy(uint8_t *dst, uint64_t lo, uint64_t hi) {
    uint64_t r[64][2];
    int n = 0;
    plat_mutex_lock(&cna_lock);
    for (int i = 0; i < ncna_rel && n < 64; i++)
        if (cna_rel[i].lo < hi && lo < cna_rel[i].hi) r[n][0] = cna_rel[i].lo, r[n++][1] = cna_rel[i].hi;
    plat_mutex_unlock(&cna_lock);
    if (!n) { memcpy(dst, (const void *)(uintptr_t)lo, hi - lo); return; }
    for (uint64_t a = lo; a < hi;) {   // runs: up to the next released range, then that range
        uint64_t next = hi, end = a;
        for (int i = 0; i < n; i++) {
            if (r[i][0] <= a && a < r[i][1]) end = r[i][1] < hi ? r[i][1] : hi;
            if (r[i][0] > a && r[i][0] < next) next = r[i][0];
        }
        if (end > a) { memset(dst + (a - lo), 0, end - a); a = end; continue; }
        memcpy(dst + (a - lo), (const void *)(uintptr_t)a, next - a);
        a = next;
    }
}

int vk_mem_cna_has_released(uint64_t lo, uint64_t hi) {
    int hit = 0;
    plat_mutex_lock(&cna_lock);
    for (int i = 0; i < ncna_rel && !hit; i++) hit = cna_rel[i].lo < hi && lo < cna_rel[i].hi;
    plat_mutex_unlock(&cna_lock);
    return hit;
}

int vk_pool_init(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) { rt_log("vk: MemoryPoolInitialize before vk_init"); return 0; }
    // The GPU never sees a GPU_NO_ACCESS pool, except a physical one (0x189; 1.4.3 also 0x18c, CPU-visible) through the
    // virtual pools that map it. Either gets device-local memory: 1.4.3's CPU never writes its 0x18c pool's storage
    // (unchanged over 3600 frames of the intro), so the guest storage and the GPU copy need no sync.
    if ((p->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS) && !(p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) return 1;
    PoolRec *r = calloc(1, sizeof *r);
    if (!r) rt_fatal("vk: out of memory for a pool record");
    r->key = p;
    r->flags = p->flags;
    r->lo = (uintptr_t)p->mem;
    r->size = p->size;
    r->born_ms = plat_now_ms();
    vk_mem_spill_start();
    int ok;
    if (p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        r->kind = POOL_VIRT;
        r->npages = p->size / NVN_VPOOL_PAGE;
        r->pt = calloc(r->npages ? r->npages : 1, sizeof *r->pt);
        ok = r->pt != NULL;
    } else if (p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) {
        r->kind = POOL_PHYS;   // device memory chunk by chunk at first use (phys_back)
        uint64_t ch = phys_chunk();
        r->nchunks = (uint32_t)(ch >= r->size ? 1 : (r->size + ch - 1) / ch);
        r->chunks = calloc(r->nchunks, sizeof *r->chunks);
        r->pseen = calloc(r->size / NVN_VPOOL_PAGE, 1);
        ok = r->chunks && r->pseen;
    } else if (lazy_on() && !cap_from_init(r)) {
        r->kind = POOL_IMPORT;   // GPU storage at the first resolve (lazy_back); never, if the GPU never reads it
        r->lazy = 1;
        ok = 1;
    } else {
        r->kind = POOL_IMPORT;
        mem_thaw((uintptr_t)p->mem, p->size);   // the import may need write access to the pages
        ok = import_pool(c, r, (uintptr_t)p->mem, p->size);
        if (!ok) {
            // Not fatal for the game: the pool works for the CPU, and vk_mem_resolve reports its addresses as unbacked.
            long n = plat_inc32(&n_import_failures);
            if (n <= 8) rt_log("vk: pool %p (flags %#x, %#llx bytes) has no GPU buffer (%ld so far)", (void *)p->mem,
                               p->flags, (unsigned long long)p->size, n);
            r->buf = VK_NULL_HANDLE;
            ok = 1;
        }
    }
    if (!ok) { free(r->pt); free(r->chunks); free(r->pseen); free(r); return 0; }
    plat_mutex_lock(&pool_lock);
    index_put(r);
    if (r->kind == POOL_PHYS) {
        if (nphys == cap_phys) {
            cap_phys = cap_phys ? cap_phys * 2 : 16;
            phys_list = realloc(phys_list, (size_t)cap_phys * sizeof *phys_list);
            if (!phys_list) rt_fatal("vk: out of memory for the physical pool list");
        }
        phys_list[nphys++] = r;
    }
    // A CPU_NO_ACCESS pool is eligible for the release of its GPU-owned textures' storage (vk_mem_cna_gpu_texture),
    // nothing at init: its bytes are the game's data (see gpu_only_release_on).
    int cna_release = (p->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) &&
                      !(p->flags & (NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_PHYSICAL |
                                    NVN_MEMORY_POOL_FLAGS_SHADER_CODE)) &&
                      gpu_only_release_on() && r->lo && r->size;
    if (cna_release) {
        for (int i = 0; i < pool_index.n; i++) {
            const PoolRec *q = pool_index.e[i].buf;
            if (q != r && q->kind != POOL_VIRT && q->lo < r->lo + r->size && r->lo < q->lo + q->size) {
                cna_release = 0;
                break;
            }
        }
    }
    plat_mutex_unlock(&pool_lock);
    if (cna_release) cna_pool_add(r->lo, r->size);
    if (r->kind == POOL_IMPORT && !r->lazy && r->smap && cap_from_init(r)) {   // (MKW_CAP_FROM_INIT, vk_mem_capture.c)
        plat_mutex_lock(&shadow_lock);
        gen_lock_x();
        cap_adopt(r);
        plat_mutex_unlock(&gen_lock);
        plat_mutex_unlock(&shadow_lock);
    }
    return 1;
}

// The last pools finalized (pool_lock): a replay that resolves an address in one finds no pool. With the asynchronous
// replay a finalize no longer drains the queue (vk_backend.c), on the NVN rule that the game frees a pool only after
// waiting for the GPU's last use of it; a queued submission that still names one is counted and logged here.
#define GONE_N 64
typedef struct { uint64_t lo, hi; } GoneRange;
static GoneRange gone[GONE_N];
static uint32_t gone_at;
static volatile int32_t n_gone_hits;

int vk_pool_needs_drain(const pool_t *p) {
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)p->mem, 0);
    const PoolRec *r = e && e->key == p ? e->buf : NULL;
    int need = r && r->kind == POOL_IMPORT && r->mem;   // imported or placed: its GPU storage is guest memory
    plat_mutex_unlock_shared(&pool_lock);
    return need;
}

static int pool_release_on(void) {
    static int v = -1;
    if (v < 0) {
#ifdef MKW_PS5_NATIVE
        v = rt_knob("POOL_RELEASE", "1");
#else
        v = rt_knob("POOL_RELEASE", "0");
#endif
    }
    return RT_KNOB_ON(v);
}

static uint64_t pf_n, pf_bytes, pf_backed, pf_released, pf_overlap, pf_win_bytes, pf_win_backed;

void vk_pool_fini_stats(uint64_t *n, uint64_t *bytes, uint64_t *backed, uint64_t *released, uint64_t *overlap,
                        uint64_t *win_bytes, uint64_t *win_backed, int reset) {
    if (n) *n = __atomic_load_n(&pf_n, __ATOMIC_RELAXED);
    if (bytes) *bytes = __atomic_load_n(&pf_bytes, __ATOMIC_RELAXED);
    if (backed) *backed = __atomic_load_n(&pf_backed, __ATOMIC_RELAXED);
    if (released) *released = __atomic_load_n(&pf_released, __ATOMIC_RELAXED);
    if (overlap) *overlap = __atomic_load_n(&pf_overlap, __ATOMIC_RELAXED);
    if (win_bytes)
        *win_bytes = reset ? __atomic_exchange_n(&pf_win_bytes, 0, __ATOMIC_RELAXED)
                           : __atomic_load_n(&pf_win_bytes, __ATOMIC_RELAXED);
    if (win_backed)
        *win_backed = reset ? __atomic_exchange_n(&pf_win_backed, 0, __ATOMIC_RELAXED)
                            : __atomic_load_n(&pf_win_backed, __ATOMIC_RELAXED);
}

// Task 32 (diagnosis): the last pool-finalize releases, for MKW_TEX_UP_LOG to tell whether a texture's storage was one.
#define PF_RING 1024
typedef struct { uint64_t lo, hi, freed, frame; uint32_t flags; } PfRel;
static PfRel pf_ring[PF_RING];
static uint32_t pf_ring_at;
static PlatMutex pf_ring_lock = PLAT_MUTEX_INIT;

static void pool_fini_note_and_release(uint64_t lo, uint64_t size, uint32_t flags) {
    if (!lo || !size) return;
    uint64_t backed = plat_mem_backed_bytes(lo, size);
    __atomic_add_fetch(&pf_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pf_bytes, size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pf_backed, backed, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pf_win_bytes, size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pf_win_backed, backed, __ATOMIC_RELAXED);
    if (!pool_release_on()) return;
    int overlap = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *q = pool_index.e[i].buf;
        if (q->kind == POOL_VIRT) continue;
        if (q->lo < lo + size && lo < q->lo + q->size) { overlap = 1; break; }
    }
    plat_mutex_unlock_shared(&pool_lock);
    if (overlap) {
        __atomic_add_fetch(&pf_overlap, 1, __ATOMIC_RELAXED);
        return;
    }
    // Only units that hold zeros: NVN gives a finalized pool's storage back to the game with its contents, and MKW keeps
    // them. Each character card has a pool of its own over its 144 KiB; the game finalizes it and samples the card again
    // from the same bytes. Releasing every whole unit (until 15/11/2026) zeroed [0xd000, 0x1d000) of each card: magenta
    // bands (task 32, run task32-poolrel1). A zero unit commits again zero-filled, so the game reads what it left.
    uint64_t freed = plat_mem_release_zero_backing(lo, size);
    __atomic_add_fetch(&pf_released, freed, __ATOMIC_RELAXED);
    if (freed) {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        plat_mutex_lock(&pf_ring_lock);
        pf_ring[pf_ring_at++ % PF_RING] = (PfRel){lo, lo + size, freed, fr, flags};
        plat_mutex_unlock(&pf_ring_lock);
    }
}

// Task 32 (diagnosis): the latest pool-finalize release (MKW_POOL_RELEASE) over [lo, lo + size), 1 when there is one.
int vk_pool_release_find(uint64_t lo, uint64_t size, uint64_t *rlo, uint64_t *rhi, uint64_t *freed, uint64_t *frame,
                         uint32_t *flags) {
    int hit = 0;
    plat_mutex_lock(&pf_ring_lock);
    uint32_t n = pf_ring_at < PF_RING ? pf_ring_at : PF_RING;
    for (uint32_t k = 1; k <= n && !hit; k++) {
        const PfRel *e = &pf_ring[(pf_ring_at - k) % PF_RING];
        if (e->lo < lo + size && lo < e->hi)
            hit = 1, *rlo = e->lo, *rhi = e->hi, *freed = e->freed, *frame = e->frame, *flags = e->flags;
    }
    plat_mutex_unlock(&pf_ring_lock);
    return hit;
}

void vk_pool_fini(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    pidx_ent e;
    plat_mutex_lock(&pool_lock);
    int had = pidx_remove(&pool_index, p, &e);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    PoolRec *r = had ? e.buf : NULL;
    if (r) gone[gone_at++ % GONE_N] = (GoneRange){r->lo, r->lo + r->size};
    if (r && r->mstate == MS_ON) mirror_forget(r);
    if (r && r->kind == POOL_PHYS) {
        for (int i = 0; i < nphys; i++)
            if (phys_list[i] == r) { phys_list[i] = phys_list[--nphys]; break; }
        for (int i = 0; i < pool_index.n; i++) {   // virtual pages that still map it become unmapped
            PoolRec *v = pool_index.e[i].buf;
            if (v->kind != POOL_VIRT) continue;
            for (uint64_t k = 0; k < v->npages; k++)
                if (v->pt[k].phys == r) v->pt[k].phys = NULL;
        }
    }
    plat_mutex_unlock(&pool_lock);
    if (!r) {
        if (!(p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) && p->mem && p->size)
            pool_fini_note_and_release((uintptr_t)p->mem, p->size, p->flags);
        return;
    }
    uint64_t r_lo = r->lo, r_size = r->size;
    int r_kind = r->kind;
    uint32_t r_flags = r->flags;
    cna_pool_remove(r->lo, r->size);   // MKW_GPU_ONLY_RELEASE: its ranges go with it (the guest storage is the game's again)
    if (r->smap) shadow_forget(r), mem_untrap(r->import_lo, r->import_hi - r->import_lo);   // the GPU's bytes, now
    if (r->kind == POOL_IMPORT) mem_thaw(r->lo, r->size);   // the storage goes back to the guest writable
    // Images cut from this range go first; a used buffer or memory waits until the GPU is done with it, since the
    // guest gets the storage back when this returns.
    vk_texture_range_gone(r->lo, r->size, NULL);
    for (uint32_t i = 0; i < r->nchunks; i++)
        if (r->chunks[i].alloc) vk_texture_range_gone(0, 0, r->chunks[i].alloc);
    // Only an import's GPU storage is guest memory. A shadow or a physical pool's storage is ours: it goes once the
    // submissions issued so far are done, without holding the game (the idle wait cost ~24 ms per finalize while
    // loading a course, live35).
    int guest_storage = r->kind == POOL_IMPORT && r->mem;   // not a shadow, nor a lazy pool never backed
    if (r->used && guest_storage) {
        vk_upload_flush(1);
        vk_queue_lock();
        const char *prev = hang_mark("pool free: vkQueueWaitIdle");
        vkQueueWaitIdle(c->gfx_queue);
        hang_mark(prev);
        vk_queue_unlock();
    } else if (r->used && r->kind != POOL_VIRT) {
        vk_upload_flush(1);
    }
    vk_mem_collect();
    // A mirrored pool's record and buffers outlive the submission being recorded, whose sync copies and write-backs may
    // name them (the game frees a pool only once the GPU is done with it, but its CPU writes may still be queued).
    // A shadow or physical pool's storage goes once the GPU is done with what was submitted: the game used the pool last
    // before its wait, so neither the submission being recorded nor a queued one names it. Waiting for the next
    // submission instead held freed storage while loading, and a staging buffer found the host heap full (rt-b0).
    if (r->mbuf) mirror_retiring(r), vk_completion_add(vk_sync_issued() + 1, mirror_retire, r);
    else if (r->used && !guest_storage && r->kind != POOL_VIRT) {
        if (vk_guard_on()) {
            uint64_t part, f = vk_sync_issued_pair(&part);
            vk_guard_tie("pool", (uint64_t)(uintptr_t)r->buf, r->lo, r->use, part);
            for (uint32_t i = 0; i < r->nchunks; i++)
                if (r->chunks[i].alloc)
                    vk_guard_tie("physical pool chunk", (uint64_t)(uintptr_t)r->chunks[i].buf, r->lo + i * phys_chunk(),
                                 r->chunks[i].use, part);
            mirror_retiring(r), vk_completion_add(f, mirror_retire, r);
        } else {
            mirror_retiring(r), vk_completion_add(vk_sync_issued(), mirror_retire, r);
        }
    }
    else destroy_rec(c, r);
    if (r_kind != POOL_VIRT) pool_fini_note_and_release(r_lo, r_size, r_flags);
}

// Distinct physical pages the game has mapped, against what the pools reserve (pool_lock exclusive).
static int64_t phys_seen_pages, map_bytes, unmap_bytes, map_calls;

static void phys_seen(PoolRec *ph, uint64_t off, uint64_t size) {
    for (uint64_t k = off / NVN_VPOOL_PAGE; k < (off + size) / NVN_VPOOL_PAGE; k++)
        if (!ph->pseen[k]) ph->pseen[k] = 1, phys_seen_pages++;
}

// Requests were checked by the front end (nvn_api_core.c, MNG mkw_vpool.c); apply them in order. The physical chunks
// they name get their memory first, outside pool_lock.
int vk_pool_map_virtual(pool_t *v, int n, const void *requests) {
    const nvn_mapping_t *rq = requests;
    static int sparse_trace = -1;
    if (sparse_trace < 0) sparse_trace = cfg_get("MKW_SPARSE_TRACE") != NULL;
    for (int i = 0; i < n; i++) {
        if (!rq[i].physical_pool) continue;
        plat_mutex_lock_shared(&pool_lock);
        PoolRec *ph = phys_find((const pool_t *)(const void *)rq[i].physical_pool);
        plat_mutex_unlock_shared(&pool_lock);
        if (!ph) continue;   // reported below
        uint64_t ch = phys_chunk(), off = (uint64_t)rq[i].physical_offset;
        if (!phys_lazy()) phys_back(ph, (uint32_t)(off / ch), (uint32_t)((off + rq[i].size - 1) / ch));
    }
    plat_mutex_lock(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)v->mem, 0);
    PoolRec *r = e && e->key == v ? e->buf : NULL;
    int ok = r && r->kind == POOL_VIRT;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    for (int i = 0; ok && i < n; i++) {
        PoolRec *ph = rq[i].physical_pool ? phys_find((const pool_t *)(const void *)rq[i].physical_pool) : NULL;
        if (rq[i].physical_pool && !ph) {
            rt_log("vk: MapVirtual request %d names physical pool %p that has no Vulkan memory", i,
                   (const void *)rq[i].physical_pool);
            ok = 0;
            break;
        }
        uint64_t first = (uint64_t)rq[i].virtual_offset / NVN_VPOOL_PAGE, count = rq[i].size / NVN_VPOOL_PAGE;
        if (first + count > r->npages) { ok = 0; break; }
        for (uint64_t k = 0; k < count; k++)
            r->pt[first + k] = (VPage){ph, ph ? (uint64_t)rq[i].physical_offset + k * NVN_VPOOL_PAGE : 0};
        if (ph) phys_seen(ph, (uint64_t)rq[i].physical_offset, rq[i].size), map_bytes += (int64_t)rq[i].size;
        else unmap_bytes += (int64_t)rq[i].size;
        if (sparse_trace)   // tools/gpu/sparse_fit.py: which pages the game maps inside its sparse textures
            rt_log("sparse: map %#llx +%#llx -> %#llx", (unsigned long long)(r->lo + (uint64_t)rq[i].virtual_offset),
                   (unsigned long long)rq[i].size,
                   ph ? (unsigned long long)(ph->lo + (uint64_t)rq[i].physical_offset) : 0ull);
    }
    int64_t calls = ++map_calls, seen = phys_seen_pages, mb = map_bytes, ub = unmap_bytes;
    uint64_t vlo = ok ? r->lo : 0;
    plat_mutex_unlock(&pool_lock);
    if (!ok) rt_log("vk: MapVirtual on pool %p failed", (void *)v->mem);
    for (int i = 0; ok && i < n; i++) vk_sparse_remap(vlo + (uint64_t)rq[i].virtual_offset, rq[i].size);
    if (!(calls & (calls - 1)) || !(calls % 4096)) {
        plat_mutex_lock(&phys_lock);
        int64_t backed = phys_backed_bytes;
        plat_mutex_unlock(&phys_lock);
        rt_log("vk: MapVirtual %lld calls: %.1f MiB mapped, %.1f MiB unmapped; %.1f MiB of physical pages named, "
               "%.1f MiB of chunks backed", (long long)calls, mb / 1048576.0, ub / 1048576.0,
               seen * (double)NVN_VPOOL_PAGE / 1048576.0, backed / 1048576.0);
    }
    return ok;
}

// Per-thread copies of resolved import and physical pools that overlap no other index entry (so pidx_find picks them for
// any range inside), valid while pool_gen is unchanged: the replay resolves a few dozen pools thousands of times per
// frame (16 vertex streams and every buffer binding of each draw). Direct-mapped by the 4 KiB page of the address, so
// a hit is one compare however many pools a frame touches. MKW_RESOLVE_CHECK=1 compares every hit with the locked
// lookup and stops on a difference.
int rec_class(const PoolRec *r) {
    if (r->kind != POOL_IMPORT) return VK_MCLS_DEVICE;
    return r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? VK_MCLS_HOST_NOACC
         : r->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED ? VK_MCLS_HOST_CACHED : VK_MCLS_HOST_UNC;
}

typedef struct { uint64_t gen, lo, hi, base, end; VkBuffer buf; VmaAllocation alloc; int cls; void *pool; } ResolveHit;
#define RHIT_SLOTS 512
static _Thread_local ResolveHit rhit[RHIT_SLOTS];

static ResolveHit *rhit_slot(uint64_t addr) { return &rhit[(addr >> 12 ^ addr >> 21) & (RHIT_SLOTS - 1)]; }

static int resolve_check_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_RESOLVE_CHECK");
        on = e && *e && strcmp(e, "0");
    }
    return on;
}

static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out);

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out) {
    memset(out, 0, sizeof *out);
    uint64_t g = __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE), end = addr + size;
    const ResolveHit *h = rhit_slot(addr);
    if (end >= addr && h->gen == g && addr >= h->lo && end <= h->hi) {
        out->buffer = h->buf;
        out->offset = addr - h->base;
        out->avail = h->end - addr;
        out->alloc = h->alloc;
        out->cls = h->cls;
        out->pool = h->pool;
        if (h->alloc) out->alloc_offset = out->offset;
        if (vk_guard_on() && h->pool) vk_guard_note(&((PoolRec *)h->pool)->use);
        if (shadow_live && h->pool && ((PoolRec *)h->pool)->smap && !((PoolRec *)h->pool)->stouched) shadow_touch(h->pool);
        if (race_on > 0 && h->pool && ((PoolRec *)h->pool)->smap) race_note(h->pool, addr, end);
        if (resolve_check_on()) {
            VkMemSpan s;
            if (!resolve_locked(addr, size, &s) || memcmp(&s, out, sizeof s))
                rt_fatal("vk: resolve cache of 0x%llx differs from the pool index", (unsigned long long)addr);
        }
        if (ru_cap) ru_note_mem(addr, size, out, 1);
        return 1;
    }
    int ok = resolve_locked(addr, size, out);
    if (ru_cap) ru_note_mem(addr, size, out, ok);   // MKW_REUSE: what a captured draw resolved
    return ok;
}

// The pool index lookup under the lock; a non-overlapped import or physical pool goes into the address's cache slot.
static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out) {
    memset(out, 0, sizeof *out);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, size);
    PoolRec *r = e ? e->buf : NULL;
    uint64_t sub_now = __atomic_load_n(&vk_replay_submit, __ATOMIC_RELAXED);
    if (r && r->lazy && r->lazy_retry <= sub_now) {   // first GPU use (or a retry): its storage under the exclusive lock
        plat_mutex_unlock_shared(&pool_lock);
        // Hang watch: with MKW_SHADOW_TRIM on, the replay and its chunk threads hung inside their recordings right after
        // the governor trimmed shadows (15/11/2026, five runs; none with it off); a trimmed pool comes back here.
        const char *prev = hang_mark_val("resolve: pool_lock exclusive for a lazy pool", r->lo);
        plat_mutex_lock(&pool_lock);
        e = pidx_find(&pool_index, (uintptr_t)addr, size);
        r = e ? e->buf : NULL;
        if (r && r->lazy && r->lazy_retry <= sub_now) hang_mark_val("resolve: lazy_back", r->lo), lazy_back(r);
        plat_mutex_unlock(&pool_lock);
        hang_mark(prev);
        plat_mutex_lock_shared(&pool_lock);
        e = pidx_find(&pool_index, (uintptr_t)addr, size);
        r = e ? e->buf : NULL;
    }
    if (r && r->kind == POOL_PHYS && !r->chunks[(addr - r->lo) / phys_chunk()].alloc) {   // a direct use: back its chunk
        uint32_t i = (uint32_t)((addr - r->lo) / phys_chunk());
        plat_mutex_unlock_shared(&pool_lock);
        phys_back(r, i, i);
        plat_mutex_lock_shared(&pool_lock);
        e = pidx_find(&pool_index, (uintptr_t)addr, size);
        r = e ? e->buf : NULL;
    }
    int ok = 0;
    if (r && r->kind == POOL_IMPORT && r->buf != VK_NULL_HANDLE) {
        int i = (int)(e - pool_index.e);
        if ((i == 0 || pool_index.maxhi[i - 1] <= e->lo) && (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= e->hi)) {
            ResolveHit *h = rhit_slot(addr);
            h->gen = __atomic_load_n(&pool_gen, __ATOMIC_RELAXED);   // stable: changed only under the exclusive lock
            h->lo = e->lo, h->hi = e->hi, h->buf = r->buf, h->cls = rec_class(r);
            h->base = r->import_lo, h->end = r->import_hi, h->alloc = NULL, h->pool = r;
        }
    }
    if (r && r->kind == POOL_IMPORT && r->buf) {
        out->cls = rec_class(r);
        out->pool = r;
        out->buffer = r->buf;
        out->offset = addr - r->import_lo;
        out->avail = r->import_hi - addr;
        if (r->smap && !r->stouched) shadow_touch(r);
        if (race_on > 0 && r->smap) race_note(r, addr, addr + size);
        if (vk_guard_on()) vk_guard_note(&r->use);
        ok = 1;
    } else if (r && r->kind == POOL_PHYS) {
        uint64_t rel = addr - r->lo, ch = phys_chunk(), i = rel / ch;
        PhysChunk *k = &r->chunks[i];
        if (k->alloc) {
            if (vk_guard_on()) vk_guard_note(&r->use), vk_guard_note(&k->use);
            out->buffer = k->buf;
            out->offset = out->alloc_offset = rel - i * ch;
            out->avail = chunk_len(r, (uint32_t)i) - out->offset;
            out->alloc = k->alloc;
            out->cls = VK_MCLS_DEVICE;
            ok = out->avail >= size;
        }
    } else if (r && r->kind == POOL_VIRT) {
        uint64_t rel = addr - r->lo, k = rel / NVN_VPOOL_PAGE;
        PoolRec *ph = k < r->npages ? r->pt[k].phys : NULL;
        uint64_t ch = phys_chunk(), ci = ph ? r->pt[k].off / ch : 0;
        static _Thread_local int retried;   // a chunk that did not fit resolves as unmapped, once
        if (ph && !ph->chunks[ci].alloc && phys_lazy() && !retried) {   // first GPU use of the chunk: back it, look again
            plat_mutex_unlock_shared(&pool_lock);
            phys_back(ph, (uint32_t)ci, (uint32_t)ci);
            retried = 1;
            int ok2 = resolve_locked(addr, size, out);
            retried = 0;
            return ok2;
        }
        if (ph && ph->chunks[ci].alloc) {   // contiguous pages of the same chunk
            uint64_t base = ci * ch, cend = base + chunk_len(ph, (uint32_t)ci);
            uint64_t off = r->pt[k].off + rel % NVN_VPOOL_PAGE, end = r->pt[k].off + NVN_VPOOL_PAGE;
            for (uint64_t j = k + 1; j < r->npages && end < cend && r->pt[j].phys == ph && r->pt[j].off == end; j++)
                end += NVN_VPOOL_PAGE;
            out->buffer = ph->chunks[ci].buf;
            out->offset = out->alloc_offset = off - base;
            out->avail = end - off;
            out->alloc = ph->chunks[ci].alloc;
            if (vk_guard_on()) vk_guard_note(&ph->use), vk_guard_note(&ph->chunks[ci].use);
            out->cls = VK_MCLS_DEVICE;
            ok = out->avail >= size;
            if (ok && !ph->used) plat_xchg32(&ph->used, 1);   // read first: the flag is only ever set
        }
    }
    if (ok && !r->used) plat_xchg32(&r->used, 1);
    if (!r)
        for (int i = 0; i < GONE_N; i++)
            if (addr >= gone[i].lo && addr < gone[i].hi) {
                if (vk_guard_on() && vk_guard_use())
                    rt_fatal("vk: MKW_GPU_GUARD: %#llx+%#llx resolved in a pool finalized with this submission still "
                             "queued", (unsigned long long)addr, (unsigned long long)size);
                if (plat_inc32(&n_gone_hits) <= 8)
                    rt_log("vk: %#llx+%#llx resolved in a pool finalized with the replay still queued: nothing bound",
                           (unsigned long long)addr, (unsigned long long)size);
                break;
            }
    plat_mutex_unlock_shared(&pool_lock);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

int vk_mem_virt_phys(uint64_t addr, uint64_t *phys) {
    *phys = 0;
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, 1);
    const PoolRec *r = e ? e->buf : NULL;
    if (r && r->kind == POOL_VIRT) {
        uint64_t k = (addr - r->lo) / NVN_VPOOL_PAGE;
        if (k < r->npages && r->pt[k].phys) *phys = r->pt[k].phys->lo + r->pt[k].off + addr % NVN_VPOOL_PAGE;
    }
    plat_mutex_unlock_shared(&pool_lock);
    return *phys != 0;
}

uint64_t vk_mem_gen(void) {
    return __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) + __atomic_load_n(&mirror_gen, __ATOMIC_ACQUIRE);
}

void vk_mem_stats(VkMemStats *s) {
    memset(s, 0, sizeof *s);
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT) s->imported_pools++;
        else if (r->kind == POOL_PHYS) s->physical_pools++;
        else s->virtual_pools++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    s->import_failures = (uint64_t)__atomic_load_n(&n_import_failures, __ATOMIC_RELAXED);
    s->upload_batches = upload_batches();
    plat_mutex_lock(&phys_lock);
    s->phys_host_chunks = (uint64_t)phys_host_n, s->phys_failed_chunks = (uint64_t)phys_failed_n;
    plat_mutex_unlock(&phys_lock);
}

void vk_mem_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    vk_upload_flush(1);
    vk_queue_lock();
    const char *prev = hang_mark("shutdown: vkDeviceWaitIdle");
    vkDeviceWaitIdle(c->device);
    hang_mark(prev);
    vk_queue_unlock();
    vk_texture_shutdown();
    upload_shutdown(c);
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) destroy_rec(c, pool_index.e[i].buf);
    pool_index.n = 0;
    shadow_reset();
    mirror_reset(c);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    nphys = 0;
    plat_mutex_unlock(&pool_lock);
    gen_reset();
}
