// Sparse textures (NVN_TEXTURE_FLAGS_SPARSE in a virtual pool) as Vulkan sparse images. The game maps 64 KiB pages of
// the texture's storage with nvnMemoryPoolMapVirtual; each mapped page here binds the Vulkan sparse blocks that hold
// the same texels to memory of that physical page.
//
// The storage layout is the one the game maps (fitted with tools/gpu/sparse_fit.py on run-sp1, 31/10/2026): a layer is
// its levels with whole tiles (nvn_sparse_tile_layout), level after level, each in row-major tiles of one page; the
// layer stride is those pages and nothing else. The mip tails (the levels smaller than a tile) were never mapped by
// MKW (D16 512x512x101: 10-page layers at 0xa0000 * k; the three 8192x8192x68 arrays: the 1-page level 4 mapped 68
// times, level 3 4 x 68). They are not in the pages, but the game copies into them (levels 5-8 of the 8192x8192x68
// arrays, m13): they get memory of their own, bound once (tail_alloc).
//
// Memory: one 64 KiB slot per (physical page, Vulkan block inside it, format), so virtual pages mapped to the same
// physical page share their texels, as on the console (the game maps a whole layer to one page as a stand-in). ASTC is
// decoded (BC7 or RGBA8): one ASTC 8x8 page of 512x512 texels is 4 BC7 blocks or 16 RGBA8 blocks, each a slot of its
// own. Slots come zeroed from 16 MiB blocks and are freed once no binding names them and the GPU is past the unbind.
//
// Changed blocks are listed per image and bound by the next graphics-queue submission (vk_sparse_order from
// vk_sync.c and vk_upload_flush): vkQueueBindSparse waits for every earlier submission, parts of the frame being
// replayed and uploads included, and every later submission waits for the bind. Sparse binding has no implicit order
// with command buffers; waiting only for the previous frame's value let a bind run beside the parts already sent of
// the current one (hypothesis for the TCP faults of run-race41 and run-mesh3: amdgpu clears the PTEs of a replaced
// range before writing the new ones, and a read in flight there faults). A slot that does not fit leaves its block
// non-resident (reads give zero: residencyNonResidentStrict), with a log line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "hangwatch.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "vk.h"
#include "vk_defaults.h"
#ifdef __PROSPERO__
int sceKernelGetPageTableStats(int *cpu_total, int *cpu_free, int *gpu_total, int *gpu_free);
#endif
#include "platform/platform.h"

#define PAGE NVN_VPOOL_PAGE
#define BLOCK_SLOTS 256   // 16 MiB of 64 KiB slots
#define SMALL_SPARSE (4u << 20)   // storage of an image that gets slots past MKW_SPARSE_MIB

typedef struct SlotBlock {
    VkBuffer buf;                 // over the whole block, for the zero fill
    VmaAllocation alloc;
    VkDeviceMemory mem;
    VkDeviceSize base;            // offset of the block in mem
    VkDeviceSize slot_size;       // bytes of each of its slots
    uint32_t type_bits;           // memory types of the images it serves
    uint32_t used;
    uint8_t busy[BLOCK_SLOTS];
    struct SlotBlock *next;
} SlotBlock;

typedef struct { uint64_t pa; uint32_t sub; int32_t fmt; } SlotKey;
typedef struct Slot {
    SlotKey key;
    SlotBlock *blk;
    uint32_t idx;
    int32_t refs;                 // block bindings that name it
    int32_t reaps;                // frees queued behind a submission (slot_reap)
    int32_t written;              // a copy or a pass wrote into it (vk_sparse_written)
} Slot;

typedef struct SparseImg {
    VkImage img;
    uint64_t addr, size;          // NVN storage range
    VkImageAspectFlags aspect;
    VkFormat fmt;
    uint32_t w, h, layers, nlev;  // nlev: levels bound per tile (whole NVN tiles and below the Vulkan mip tail)
    uint32_t ntw, nth;            // NVN tile in texels
    uint32_t gw, gh;              // Vulkan sparse block in texels
    uint32_t nnvn;                // NVN levels with whole tiles (nnvn >= nlev)
    uint32_t tiles_x[16], tiles_y[16], tile0[16], stride;   // NVN tiles a row, rows, first page of each level; pages a layer
    uint32_t bx[16], blk0[16], nblk;           // Vulkan blocks a row and first block of each level; blocks a layer
    VkDeviceSize slot_size;
    uint32_t type_bits;
    Slot **bound;                 // layers * nblk
    // The levels NVN keeps out of its tiles (below nlev: smaller than a tile, and the Vulkan mip tail) have no pages
    // the game maps, so they never got memory and read as zero, copies into them dropped. They get memory of their
    // own instead (tail_*: one allocation, bound once): the race's bushes and cacti sample those levels of the terrain
    // arrays and came out black (m8, m10: still black with every tile resident).
    VmaAllocation tail_alloc;
    VkDeviceMemory tail_mem;
    VkDeviceSize tail_base, tail_bytes;
    uint32_t nmips, tail_end, tail_blocks;   // levels; first Vulkan mip tail level (nmips: none); blocks a layer in
                                             // levels nlev..tail_end-1
    VkSparseImageMemoryRequirements tail_req;
    int tail_pending;                 // bound by the next flush
    uint32_t tail_written;            // levels outside the tiles a copy or pass wrote (bit per level), for the log
    // The submission whose completion means every bind issued for s has run (vk_sparse_destroy). Binds are queue
    // operations, not parts: a texture destroyed while idle waits only for the parts already sent (vk_tex_evict.c), and
    // NVIDIA's kernel mapper then ran a queued bind of an image already destroyed: "Requested object not found" for
    // pUnmap->hVirtualMemory in mem_mapper.c, and the device lost (620.70.06, RTX 5070 Ti, 09/11/2026).
    uint64_t bind_fence;
    VkDeviceSize va_bytes;        // vkGetImageMemoryRequirements size: the VA the driver reserves (sparse_census)

    uint8_t *dirty;               // bound changed since the last flush; listed in dl
    uint32_t *dl;
    int ndl, cdl;
    uint64_t created_frame, last_sample_frame, last_write_frame, sample_count;
    struct SparseImg *next;
} SparseImg;

// Lock order: the queue lock, then sp_lock, then the upload lock (vk_mem_upload.c, cmd_lock). sp_owner: the thread
// holding sp_lock, for the hang watch.
static PlatMutex sp_lock = PLAT_MUTEX_INIT;
static uint32_t sp_owner;
static inline __attribute__((always_inline)) void sp_take(void) {
    const char *prev = hang_mark("sparse: sp_lock waiting");
    plat_mutex_lock(&sp_lock);
    hang_mark(prev);
    __atomic_store_n(&sp_owner, hang_tid(), __ATOMIC_RELAXED);
}
static inline __attribute__((always_inline)) void sp_give(void) {
    __atomic_store_n(&sp_owner, 0, __ATOMIC_RELAXED);
    plat_mutex_unlock(&sp_lock);
}
static SparseImg *images;
static nvn_hmap slots = {.klen = sizeof(SlotKey)};
static nvn_hmap cap_pending = {.klen = sizeof(SlotKey)};   // runtime/gfx/cap: SlotKey -> saved texels
static SlotBlock *blocks;
static int ndirty;               // images with a non-empty dirty list
static Slot **dying;              // refs reached 0 since the last flush: reaped behind the next submission
static int ndying, cdying;
static VkSemaphore order_tl;      // timeline: one value per graphics-queue submission and per bind (vk_sparse_order)
static VkSemaphore bind_sem;      // MKW_SPARSE_ORDER=0: binary, signalled by a bind for the next replayed submission
static uint64_t order_val, order_submitted, bind_val;
static int binary_pending;        // MKW_SPARSE_ORDER=0: a bind signalled bind_sem for the submission being made   // last value handed out; value of the last bind (0: none yet)
static int64_t n_slots, n_failed, n_binds, n_images, n_blocks, n_over, n_written, n_tail_writes;
static VmaAllocation *dying_tails;   // tail memory of destroyed images: freed behind the next submission
static int ndying_tails, cdying_tails, ntail_pending;

static void grow(void **arr, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 256;
    if (!(*arr = realloc(*arr, (size_t)*cap * elem))) rt_fatal("vk: out of memory (sparse)");
}

// MKW_SPARSE_MIB: most slot memory. run-sp3 (3 GB cap) bound 1 472 MiB, nearly all in three 8192x8192x68 arrays
// (BC6H 509, two ASTC as BC7 505 and 374), and 64 other textures (menu icons, the ASTC the arrays are copied from) had
// no memory left. Past it a block stays non-resident (reads zero); a new image binds its coarse levels first.
// Unset, it comes from the GPU type and its largest device-local heap (vk_default_sparse_mib). 0 lifts it.
static uint64_t budget_default(void) {
    VkCtx *c = vk_ctx();
    if (!c) return vk_default_sparse_mib(0, 0);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && c->mem_props.memoryHeaps[h].size > vram)
            vram = c->mem_props.memoryHeaps[h].size;
#ifdef __PROSPERO__
    uint64_t def = vk_default_sparse_mib(1, vram >> 20);
    return def > 1536 ? 1536 : def;
#else
    return vk_default_sparse_mib(c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, vram >> 20);
#endif
}

static uint64_t budget(void) {
    static uint64_t v;
    if (!v) {
        const char *e = cfg_get("MKW_SPARSE_MIB");
        uint64_t mib = e && *e ? (uint64_t)atol(e) : budget_default();
        rt_log("vk: sparse: tile slots up to %llu MiB (MKW_SPARSE_MIB%s; 0 lifts it)", (unsigned long long)mib,
               e && *e ? "" : " unset, from the GPU");
        v = mib ? mib << 20 : UINT64_MAX;
    }
    return v;
}

static int on(void) {
    static int v = -1;
    if (v < 0) v = !(cfg_get("MKW_SPARSE_IMAGES") && !atoi(cfg_get("MKW_SPARSE_IMAGES")));   // default on
    return v;
}

static SlotBlock *new_block(VkCtx *c, uint32_t type_bits, VkDeviceSize slot_size) {
    SlotBlock *b = calloc(1, sizeof *b);
    if (!b) rt_fatal("vk: out of memory (sparse block)");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = slot_size * BLOCK_SLOTS,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(c->device, &bi, NULL, &b->buf) != VK_SUCCESS) { free(b); return NULL; }
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(c->device, b->buf, &rb);
    VkMemoryRequirements req = {.size = slot_size * BLOCK_SLOTS, .alignment = slot_size > rb.alignment ? slot_size : rb.alignment,
                                .memoryTypeBits = rb.memoryTypeBits & type_bits};
    VmaAllocationCreateInfo aci = {.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VmaAllocationInfo info;
    if (!req.memoryTypeBits || vmaAllocateMemory(c->vma, &req, &aci, &b->alloc, &info) != VK_SUCCESS ||
        vmaBindBufferMemory(c->vma, b->alloc, b->buf) != VK_SUCCESS) {
        if (b->alloc) vmaFreeMemory(c->vma, b->alloc);
        vkDestroyBuffer(c->device, b->buf, NULL);
        free(b);
        return NULL;
    }
    b->mem = info.deviceMemory, b->base = info.offset, b->type_bits = type_bits, b->slot_size = slot_size;
    n_blocks++;
    b->next = blocks, blocks = b;
    return b;
}

// The slot of a physical page's block for an image, made (zeroed) on first use. NULL when it does not fit.
static Slot *slot_get(VkCtx *c, const SparseImg *s, uint64_t pa, uint32_t sub) {
    SlotKey k = {pa, sub, (int32_t)s->fmt};
    Slot *sl = nvn_hm_get(&slots, &k);
    if (sl) return sl;
    SlotBlock *b = blocks;
    while (b && (b->used == BLOCK_SLOTS || b->type_bits != s->type_bits)) b = b->next;
    // Small images are outside the cap: it is there for the 8192x8192x68 arrays, which fill it before the 512x512 ones
    // the game makes later (foliage maps, r8sn) get a slot, and those read as zero drew the bushes black (01/11/2026).
    // The live slots count too: a block opened past the cap for a small image is not for the arrays to fill (m3).
    int big = s->size > SMALL_SPARSE;
    if (big && ((uint64_t)(n_slots + 1) * s->slot_size > budget() ||
                (!b && (uint64_t)(n_blocks + 1) * BLOCK_SLOTS * s->slot_size > budget()))) {
        if (++n_over <= 4 || !(n_over & (n_over - 1)))
            rt_log("vk: sparse: tile slots over MKW_SPARSE_MIB (%lld MiB in blocks; %lld tiles left non-resident)",
                   (long long)(budget() >> 20), (long long)n_over);
        return NULL;
    }
    if (!b && !(b = new_block(c, s->type_bits, s->slot_size))) {
        if (++n_failed <= 8 || !(n_failed & (n_failed - 1)))
            rt_log("vk: sparse: no memory for a tile slot (%lld failed, %lld slots live)", (long long)n_failed,
                   (long long)n_slots);
        return NULL;
    }
    uint32_t i = 0;
    while (b->busy[i]) i++;
    b->busy[i] = 1, b->used++;
    if (!(sl = calloc(1, sizeof *sl))) rt_fatal("vk: out of memory (sparse slot)");
    sl->key = k, sl->blk = b, sl->idx = i;
    nvn_hm_put(&slots, &k, sl);
    n_slots++;
    VkCommandBuffer cb = vk_upload_begin();   // pool memory starts zeroed; the queue order puts it before its use
    vkCmdFillBuffer(cb, b->buf, (VkDeviceSize)i * s->slot_size, s->slot_size, 0);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &dep);
    // A capture being replayed (runtime/gfx/cap) holds this slot's texels: they replace the zeros, each time the slot is
    // made (a slot no binding names is freed once the GPU is past it, which depends on timing, and may come back).
    uint8_t *saved = cap_pending.n ? nvn_hm_get(&cap_pending, &k) : NULL;
    if (saved) {
        VkBuffer sb;
        uint8_t *map = vk_staging(s->slot_size, &sb, NULL);
        memcpy(map, saved, s->slot_size);
        VkBufferCopy cp = {0, (VkDeviceSize)i * s->slot_size, s->slot_size};
        vkCmdCopyBuffer(cb, sb, b->buf, 1, &cp);
        vkCmdPipelineBarrier2(cb, &dep);
        sl->written = 1;
    }
    vk_upload_end();
    return sl;
}

static void slot_unref(Slot *sl) {
    if (--sl->refs) return;
    if (ndying == cdying) grow((void **)&dying, &cdying, sizeof *dying);
    dying[ndying++] = sl;
    sl->reaps++;
}

// Completion thread, once the submission after the unbind is done: frees the slot unless a binding named it again.
static void slot_reap(void *arg) {
    Slot *sl = arg;
    SlotBlock *dead_blk = NULL;
    sp_take();
    if (!--sl->reaps && !sl->refs) {
        SlotBlock *b = sl->blk;
        nvn_hm_del(&slots, &sl->key);
        b->busy[sl->idx] = 0, b->used--;
        n_slots--;
        n_written -= sl->written;
        free(sl);
        if (!b->used) {
            for (SlotBlock **pp = &blocks; *pp; pp = &(*pp)->next)
                if (*pp == b) { *pp = b->next; break; }
            n_blocks--;
            dead_blk = b;
        }
    }
    sp_give();
    if (dead_blk) {
        VkCtx *c = vk_ctx();
        if (c) {
            if (dead_blk->buf) vkDestroyBuffer(c->device, dead_blk->buf, NULL);
            if (dead_blk->alloc) vmaFreeMemory(c->vma, dead_blk->alloc);
        }
        free(dead_blk);
    }
}

static void mark_dirty(SparseImg *s, size_t i) {
    if (s->dirty[i]) return;
    s->dirty[i] = 1;
    if (!s->ndl) ndirty++;
    if (s->ndl == s->cdl) grow((void **)&s->dl, &s->cdl, sizeof *s->dl);
    s->dl[s->ndl++] = (uint32_t)i;
}

// The bind of block i of s to what bound[] names now.
static VkSparseImageMemoryBind make_bind(const SparseImg *s, uint32_t i) {
    uint32_t layer = i / s->nblk, r = i % s->nblk, l = 0;
    while (l + 1 < s->nlev && r >= s->blk0[l + 1]) l++;
    r -= s->blk0[l];
    uint32_t x = r % s->bx[l] * s->gw, y = r / s->bx[l] * s->gh;
    uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
    const Slot *sl = s->bound[i];
    return (VkSparseImageMemoryBind){
        .subresource = {s->aspect, l, layer},
        .offset = {(int32_t)x, (int32_t)y, 0},
        .extent = {lw - x < s->gw ? lw - x : s->gw, lh - y < s->gh ? lh - y : s->gh, 1},
        .memory = sl ? sl->blk->mem : VK_NULL_HANDLE,
        .memoryOffset = sl ? sl->blk->base + (VkDeviceSize)sl->idx * s->slot_size : 0};
}

// Binds the Vulkan blocks inside page p of s to what the page maps now (sp_lock held).
static void bind_page(VkCtx *c, SparseImg *s, uint64_t p) {
    uint32_t layer = (uint32_t)(p / s->stride), rel = (uint32_t)(p % s->stride), l = 0;
    if (layer >= s->layers) return;   // the mip tails: never mapped by MKW
    while (l + 1 < s->nnvn && rel >= s->tile0[l + 1]) l++;
    if (l >= s->nlev) return;         // inside the Vulkan mip tail
    uint32_t t = rel - s->tile0[l], tx = t % s->tiles_x[l], ty = t / s->tiles_x[l];
    uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
    uint64_t pa = 0;
    int mapped = vk_mem_virt_phys(s->addr + p * PAGE, &pa);

    for (uint32_t sy = 0; sy < s->nth / s->gh; sy++)
        for (uint32_t sx = 0; sx < s->ntw / s->gw; sx++) {
            uint32_t x = tx * s->ntw + sx * s->gw, y = ty * s->nth + sy * s->gh;
            if (x >= lw || y >= lh) continue;
            size_t i = (size_t)layer * s->nblk + s->blk0[l] + (y / s->gh) * s->bx[l] + x / s->gw;
            Slot *want = mapped ? slot_get(c, s, pa, sy * (s->ntw / s->gw) + sx) : NULL;
            if (want == s->bound[i]) continue;
            if (want) want->refs++;
            if (s->bound[i]) slot_unref(s->bound[i]);
            s->bound[i] = want;
            mark_dirty(s, i);
        }
}

static void bind_range(VkCtx *c, SparseImg *s, uint64_t lo, uint64_t hi) {
    if (lo < s->addr) lo = s->addr;
    if (hi > s->addr + s->size) hi = s->addr + s->size;
    for (uint64_t a = lo & ~(uint64_t)(PAGE - 1); a < hi; a += PAGE) bind_page(c, s, (a - s->addr) / PAGE);
}

static uint32_t tail_count(const SparseImg *s) {   // mip tails of s: one a layer, or one for all
    if (s->tail_end == s->nmips || !s->tail_req.imageMipTailSize) return 0;
    return s->tail_req.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 1 : s->layers;
}

static VkDeviceSize round_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// Memory for the levels of s outside NVN's tiles (see tail_alloc in SparseImg); levels: the image's mip count.
static void tail_alloc(VkCtx *c, SparseImg *s, const VkSparseImageMemoryRequirements *sr, uint32_t levels, uint32_t types) {
    s->nmips = levels, s->tail_end = sr && sr->imageMipTailFirstLod < levels ? sr->imageMipTailFirstLod : levels;
    if (sr) s->tail_req = *sr;
    for (uint32_t l = s->nlev; l < s->tail_end; l++) {
        uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
        s->tail_blocks += ((lw + s->gw - 1) / s->gw) * ((lh + s->gh - 1) / s->gh);
    }
    VkDeviceSize bytes = (VkDeviceSize)s->layers * s->tail_blocks * s->slot_size +
                         tail_count(s) * round_up(s->tail_req.imageMipTailSize, s->slot_size);
    if (!bytes) return;
    VkMemoryRequirements req = {.size = bytes, .alignment = s->slot_size, .memoryTypeBits = types};
    VmaAllocationCreateInfo aci = {.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VmaAllocationInfo info;
    if (vmaAllocateMemory(c->vma, &req, &aci, &s->tail_alloc, &info) != VK_SUCCESS) {
        s->tail_alloc = NULL;
        rt_log("vk: sparse: no memory for the %llu KiB of levels outside the tiles (%ux%u layers %u)",
               (unsigned long long)(bytes >> 10), s->w, s->h, s->layers);
        return;
    }
    static volatile int64_t g_sparse_tail_bytes_unused;
    (void)g_sparse_tail_bytes_unused;
    s->tail_mem = info.deviceMemory, s->tail_base = info.offset, s->tail_bytes = bytes, s->tail_pending = 1;
    __atomic_add_fetch(&s->tail_bytes, 0, __ATOMIC_RELAXED);
    rt_log("vk: sparse: %ux%u layers %u: levels %u-%u in blocks (%u a layer), mip tail from %u: offset %#llx size %#llx "
           "stride %#llx flags %#x", s->w, s->h, s->layers, s->nlev, s->tail_end - 1, s->tail_blocks, s->tail_end,
           (unsigned long long)s->tail_req.imageMipTailOffset, (unsigned long long)s->tail_req.imageMipTailSize,
           (unsigned long long)s->tail_req.imageMipTailStride, s->tail_req.formatProperties.flags);
}

// The binds of s's levels outside the tiles: blocks into ib/bb, mip tails into ob/mb (sp_lock held).
static void tail_binds(const SparseImg *s, VkSparseImageMemoryBindInfo *ib, VkSparseImageMemoryBind *bb,
                       VkSparseImageOpaqueMemoryBindInfo *ob, VkSparseMemoryBind *mb) {
    uint32_t n = 0;
    VkDeviceSize off = s->tail_base;
    for (uint32_t layer = 0; layer < s->layers; layer++)
        for (uint32_t l = s->nlev; l < s->tail_end; l++) {
            uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
            for (uint32_t y = 0; y < lh; y += s->gh)
                for (uint32_t x = 0; x < lw; x += s->gw, off += s->slot_size)
                    bb[n++] = (VkSparseImageMemoryBind){.subresource = {s->aspect, l, layer},
                                                        .offset = {(int32_t)x, (int32_t)y, 0},
                                                        .extent = {lw - x < s->gw ? lw - x : s->gw, lh - y < s->gh ? lh - y : s->gh, 1},
                                                        .memory = s->tail_mem, .memoryOffset = off};
        }
    *ib = (VkSparseImageMemoryBindInfo){.image = s->img, .bindCount = n, .pBinds = bb};
    uint32_t nt = tail_count(s);
    for (uint32_t t = 0; t < nt; t++, off += round_up(s->tail_req.imageMipTailSize, s->slot_size))
        mb[t] = (VkSparseMemoryBind){.resourceOffset = s->tail_req.imageMipTailOffset + t * s->tail_req.imageMipTailStride,
                                     .size = s->tail_req.imageMipTailSize, .memory = s->tail_mem, .memoryOffset = off};
    *ob = (VkSparseImageOpaqueMemoryBindInfo){.image = s->img, .bindCount = nt, .pBinds = mb};
}

static void tail_free(void *arg) {
    VmaAllocation a = (VmaAllocation)arg;
    VmaAllocationInfo info = {0};
    vmaGetAllocationInfo(vk_ctx()->vma, a, &info);
    // Tracked in g_sparse_tail_bytes; approximate via info.size on free.
    vmaFreeMemory(vk_ctx()->vma, a);
}

uint64_t vk_sparse_bytes(void) {
    return (uint64_t)__atomic_load_n(&n_slots, __ATOMIC_RELAXED) * 65536ull;
}

static int order_on(void);

// RADV on the PS5 binds sparse memory on the CPU (has_sparse_cpu_binds): vkQueueBindSparse first waits, on the calling
// thread, until the GPU has run what the bind's wait semaphores stand for (radv_queue_submit_bind_sparse_memory in
// build/ps5-stack/PS5_Mesa/src/amd/vulkan/radv_queue.c), here every earlier submission: run 5's second bind took
// 379 ms. Elsewhere a bind is queued GPU work. MKW_SPARSE_CPU_WAIT makes the wait here, under the queue lock:
//   1 (default on the PS5): before sp_lock is taken, so the game threads mapping pages (vk_sparse_remap), the replay
//     thread's vk_sparse_written and the completion thread's slot_reap are not held for the GPU's time. RADV's own wait
//     then finds the semaphore reached.
//   2 (test only): just before vkQueueBindSparse, under sp_lock, where RADV waits on the PS5: its timing on Linux.
//   0 (default elsewhere): no wait of ours.
// The wait cannot deadlock: every value up to order_val is in the queue when the queue lock is taken (vk_queue_lock
// sends the parts waiting for the submit thread first; vk_upload_flush and vk_sync_submit submit the value they take
// before letting the lock go), so only the GPU has to run for it to end, and no submission it waits for needs a lock
// or a CPU signal (the only other waits are the swapchain's acquire semaphores, signalled at acquire).
static int cpu_wait_mode(void) {
    static int v = -1;
    if (v < 0) {
#ifdef __PROSPERO__
        const char *def = "1";
#else
        const char *def = "0";
#endif
        const char *e = cfg_get("MKW_SPARSE_CPU_WAIT");
        v = atoi(e && *e ? e : def);
        if (v) rt_log("vk: sparse: binds wait on the CPU for the GPU %s (MKW_SPARSE_CPU_WAIT=%d)",
                      v == 2 ? "under sp_lock, as RADV does on the PS5" : "before sp_lock", v);
    }
    return v;
}

void vk_sparse_order_submitted(uint64_t signal) {
    if (signal > order_submitted) __atomic_store_n(&order_submitted, signal, __ATOMIC_RELEASE);
}

// Queue lock held: until order_tl reaches value.
static void cpu_wait(VkCtx *c, uint64_t value) {
    if (!value || !order_tl) return;
    if (value > __atomic_load_n(&order_submitted, __ATOMIC_ACQUIRE)) vk_sync_parts_flush();
    uint64_t sub = __atomic_load_n(&order_submitted, __ATOMIC_ACQUIRE);
    if (value > sub)
        rt_fatal("vk: sparse: wait for unsubmitted order value %llu (submitted=%llu, handed=%llu)",
                 (unsigned long long)value, (unsigned long long)sub,
                 (unsigned long long)__atomic_load_n(&order_val, __ATOMIC_RELAXED));
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &order_tl, .pValues = &value};
    const char *prev = hang_mark_val("sparse: CPU wait for order value", value);
    VkResult r = vkWaitSemaphores(c->device, &wi, UINT64_MAX);
    hang_mark(prev);
    if (r != VK_SUCCESS) rt_fatal("vk: sparse: wait for order value %llu before a bind failed (%d)", (unsigned long long)value, (int)r);
}

// Hang watch: who holds the queue, sparse and upload locks, and where the order timeline is.
static void hang_report(char *buf, int n) {
    VkCtx *c = vk_ctx();
    uint64_t reached = 0;
    if (c && order_tl) vkGetSemaphoreCounterValue(c->device, order_tl, &reached);
    snprintf(buf, (size_t)n,
             "locks: queue held by tid %u, sp_lock by tid %u, upload by tid %u; sparse order timeline reached %llu, "
             "submitted %llu, handed out %llu, last bind %llu; %d images with binds queued, %d tails pending; graphics "
             "submissions issued %llu",
             vk_queue_owner(), __atomic_load_n(&sp_owner, __ATOMIC_RELAXED), vk_upload_owner(),
             (unsigned long long)reached, (unsigned long long)__atomic_load_n(&order_submitted, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&order_val, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&bind_val, __ATOMIC_RELAXED), __atomic_load_n(&ndirty, __ATOMIC_RELAXED),
             __atomic_load_n(&ntail_pending, __ATOMIC_RELAXED), (unsigned long long)vk_sync_issued());
}

static void hang_report_once(void) {
    static int done;
    if (!__atomic_exchange_n(&done, 1, __ATOMIC_RELAXED)) hang_reporter(hang_report);
}

// Binds s's levels outside the tiles now, on the timeline every graphics-queue submission waits on (vk_sparse_order):
// the game copies into them right after creating the array, and a copy recorded in the upload batch (ASTC decoded
// into BC7, vk_texture_astc_copy) went out before a bind left for the next replayed submission and was lost (m17).
static void tail_bind_now(VkCtx *c, SparseImg *s) {
    if (!s->tail_pending || !order_on()) return;   // MKW_SPARSE_ORDER=0: with the other binds (vk_sparse_order)
    VkSparseImageMemoryBindInfo ib;
    VkSparseImageOpaqueMemoryBindInfo ob;
    VkSparseImageMemoryBind *bb = malloc(sizeof *bb * ((size_t)s->layers * s->tail_blocks + 1));
    VkSparseMemoryBind *mb = malloc(sizeof *mb * (tail_count(s) + 1));
    if (!bb || !mb) rt_fatal("vk: out of memory (sparse tail binds)");
    tail_binds(s, &ib, bb, &ob, mb);
    vk_queue_lock();
    if (!order_tl) {
        VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
        if (vkCreateSemaphore(c->device, &si, NULL, &order_tl) != VK_SUCCESS) rt_fatal("vk: sparse order semaphore");
    }
    uint64_t wait = order_val, sig = ++order_val;
    VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = wait ? 1u : 0u, .pWaitSemaphoreValues = &wait,
                                        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sig};
    VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &tl,
                           .waitSemaphoreCount = wait ? 1u : 0u, .pWaitSemaphores = &order_tl,
                           .imageOpaqueBindCount = ob.bindCount ? 1u : 0u, .pImageOpaqueBinds = &ob,
                           .imageBindCount = ib.bindCount ? 1u : 0u, .pImageBinds = &ib,
                           .signalSemaphoreCount = 1, .pSignalSemaphores = &order_tl};
    if (cpu_wait_mode()) cpu_wait(c, wait);   // only the queue lock is held here
    const char *prev = hang_mark_val("sparse: vkQueueBindSparse (levels outside the tiles)", sig);
    VkResult r = vkQueueBindSparse(c->gfx_queue, 1, &bi, VK_NULL_HANDLE);
    hang_mark(prev);
    if (r == VK_SUCCESS) vk_sparse_order_submitted(sig);
    bind_val = sig;
    s->bind_fence = vk_sync_issued() + 2;   // the submission being recorded may have taken its wait already
    vk_queue_unlock();
    if (r != VK_SUCCESS) rt_fatal("vk: vkQueueBindSparse of the levels outside the tiles failed (%d)", (int)r);
    free(bb);
    free(mb);
    s->tail_pending = 0;
}

static uint64_t destroy_gen;
static struct {
    uint64_t addr, size, gen;
    uint32_t w, h, layers;
    VkFormat fmt;
    int attempts;
} failed_create[32];
static int n_failed_create;

static int sparse_create_blocked(uint64_t addr, uint64_t size, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers) {
    int blocked = 0;
    sp_take();
    for (int i = 0; i < n_failed_create; i++) {
        if (failed_create[i].addr == addr && failed_create[i].size == size && failed_create[i].fmt == fmt &&
            failed_create[i].w == w && failed_create[i].h == h && failed_create[i].layers == layers) {
            if (failed_create[i].gen == destroy_gen && failed_create[i].attempts >= 1) blocked = 1;
            break;
        }
    }
    sp_give();
    return blocked;
}

// The PS5's CPU page-table pool (16,384 units, one per 2 MiB of mapped VA) runs out on these images: RADV maps its
// zero block over every unbound part of their VA, so an 8192x8192x68 array costs 2,903 units whatever the game binds
// (run 8). For each big image: its VA, and per level the NVN pages the game maps, the Vulkan blocks that have a slot,
// and the layers with any mapped page: what creating images with only the bound layers or levels would save
// (hydrogen task 10). sp_lock held; the page lookups take the pool lock under it, as bind_page does.
static void census_locked(const char *why) {
    static int nlog;
    if (++nlog > 24) return;
    for (const SparseImg *s = images; s; s = s->next) {
        if (s->size <= SMALL_SPARSE) continue;
        char buf[1024];
        int k = 0;
        for (uint32_t l = 0; l < s->nlev && k < (int)sizeof buf - 96; l++) {
            uint64_t pages = (uint64_t)s->tiles_x[l] * s->tiles_y[l], mapped = 0, slots = 0, blocks = 0;
            uint32_t layers_mapped = 0;
            uint32_t lh = s->h >> l ? s->h >> l : 1, lblocks = s->bx[l] * ((lh + s->gh - 1) / s->gh);
            for (uint32_t y = 0; y < s->layers; y++) {
                uint64_t m0 = mapped, pa = 0;
                for (uint64_t t = 0; t < pages; t++)
                    mapped += (uint64_t)vk_mem_virt_phys(s->addr + ((uint64_t)y * s->stride + s->tile0[l] + t) * PAGE, &pa);
                layers_mapped += mapped > m0;
                for (uint32_t b = 0; b < lblocks; b++) slots += s->bound[(size_t)y * s->nblk + s->blk0[l] + b] != NULL;
                blocks += lblocks;
            }
            k += snprintf(buf + k, sizeof buf - (size_t)k, "; L%u pages mapped %llu/%llu in %u/%u layers, blocks with a slot %llu/%llu",
                          l, (unsigned long long)mapped, (unsigned long long)(pages * s->layers), layers_mapped, s->layers,
                          (unsigned long long)slots, (unsigned long long)blocks);
        }
        rt_log("vk: sparse census (%s): %#llx %ux%u layers %u VkFormat %d: VA %llu MiB (%llu 2 MiB units)%s", why,
               (unsigned long long)s->addr, s->w, s->h, s->layers, (int)s->fmt, (unsigned long long)(s->va_bytes >> 20),
               (unsigned long long)((s->va_bytes + (2u << 20) - 1) >> 21), buf);
    }
}

static void sparse_create_note_fail(uint64_t addr, uint64_t size, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers,
                                    VkResult r) {
    int log_it = 0;
    sp_take();
    int idx = -1;
    for (int i = 0; i < n_failed_create; i++) {
        if (failed_create[i].addr == addr && failed_create[i].size == size && failed_create[i].fmt == fmt &&
            failed_create[i].w == w && failed_create[i].h == h && failed_create[i].layers == layers) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        idx = n_failed_create < 32 ? n_failed_create++ : 0;
        failed_create[idx] = (typeof(failed_create[0])){addr, size, destroy_gen, w, h, layers, fmt, 0};
        log_it = 1;
    } else if (failed_create[idx].gen != destroy_gen) {
        failed_create[idx].gen = destroy_gen;
        failed_create[idx].attempts = 0;
        log_it = 1;
    }
    failed_create[idx].attempts++;
    sp_give();
    if (log_it) {
#ifdef __PROSPERO__
        int ct = 0, cf = 0, gt = 0, gf = 0;
        int ptrc = sceKernelGetPageTableStats(&ct, &cf, &gt, &gf);
        if (ptrc == 0)
            rt_log("vk: sparse: vkCreateImage failed (VkResult=%d, VkFormat %d %ux%u layers %u, pt cpu=%d/%d gpu=%d/%d)",
                   (int)r, (int)fmt, w, h, layers, cf, ct, gf, gt);
        else
            rt_log("vk: sparse: vkCreateImage failed (VkResult=%d, VkFormat %d %ux%u layers %u, pt_rc=%d)",
                   (int)r, (int)fmt, w, h, layers, ptrc);
#else
        rt_log("vk: sparse: vkCreateImage failed (VkResult=%d, VkFormat %d %ux%u layers %u)", (int)r, (int)fmt, w, h,
               layers);
#endif
        sp_take();
        census_locked("create failed");
        sp_give();
    }
}

int vk_sparse_create(const VkImageCreateInfo *ci0, const nvn_texdesc *d, uint64_t addr, uint64_t size, VkImage *out) {
    VkCtx *c = vk_ctx();
    static volatile int32_t nwarn;
    *out = VK_NULL_HANDLE;
    if (!on() || !(d->flags & NVN_TEXTURE_FLAGS_SPARSE) || !nvn_gpu_only(addr) || ci0->imageType != VK_IMAGE_TYPE_2D ||
        ci0->samples != VK_SAMPLE_COUNT_1_BIT || !(ci0->usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return 0;
    VkImageCreateInfo ci = *ci0;
    ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    ci.usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    uint32_t n = 0;
    VkSparseImageFormatProperties fp[4];
    vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &n, NULL);
    if (n > 4) n = 4;
    vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &n, fp);
    if (!n || (fp[0].flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT)) {
        if (plat_inc32(&nwarn) <= 8) rt_log("vk: sparse: VkFormat %d usage %#x has no standard sparse blocks", (int)ci.format, ci.usage);
        return 0;
    }
    int32_t lay[4];   // levels with whole tiles, tile width, height, depth
    nvn_sparse_tile_layout(d, lay);
    SparseImg *s = calloc(1, sizeof *s);
    if (!s) rt_fatal("vk: out of memory (sparse image)");
    s->addr = addr, s->size = size, s->fmt = ci.format, s->aspect = fp[0].aspectMask;
    s->w = ci.extent.width, s->h = ci.extent.height, s->layers = ci.arrayLayers;
    s->created_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    s->ntw = (uint32_t)lay[1], s->nth = (uint32_t)lay[2];
    s->gw = fp[0].imageGranularity.width, s->gh = fp[0].imageGranularity.height;
    if (!s->gw || !s->gh || s->ntw % s->gw || s->nth % s->gh || lay[0] > 16) {
        if (plat_inc32(&nwarn) <= 8)
            rt_log("vk: sparse: NVN tile %ux%u is not whole Vulkan blocks of %ux%u (VkFormat %d)", s->ntw, s->nth, s->gw,
                   s->gh, (int)ci.format);
        free(s);
        return 0;
    }
    static volatile int32_t n_small_logged;
    int big = s->size > SMALL_SPARSE;
    if (sparse_create_blocked(addr, size, ci.format, s->w, s->h, s->layers)) {
        free(s);
        return 0;
    }
    int small_log = !big && plat_inc32(&n_small_logged) <= 4;
    if (big) plat_mem_census_log("before-big-sparse-create");
    else if (small_log) plat_mem_census_log("before-small-sparse-create");
    VkResult cr = vkCreateImage(c->device, &ci, NULL, &s->img);
    if (cr != VK_SUCCESS) {
        sparse_create_note_fail(addr, size, ci.format, s->w, s->h, s->layers, cr);
        free(s);
        return 0;
    }
    if (big) plat_mem_census_log("after-big-sparse-create");
    else if (small_log) plat_mem_census_log("after-small-sparse-create");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->device, s->img, &mr);
    s->va_bytes = mr.size;
    uint32_t nr = 1;
    VkSparseImageMemoryRequirements sr;
    vkGetImageSparseMemoryRequirements(c->device, s->img, &nr, &sr);
    s->slot_size = mr.alignment, s->type_bits = mr.memoryTypeBits;
    s->nlev = (uint32_t)lay[0] < ci.mipLevels ? (uint32_t)lay[0] : ci.mipLevels;
    if (nr && sr.imageMipTailFirstLod < s->nlev) s->nlev = sr.imageMipTailFirstLod;   // not seen on RADV: log below
    s->nnvn = (uint32_t)lay[0];
    for (uint32_t l = 0, page = 0, blk = 0; l < s->nnvn; l++) {
        uint32_t lw = s->w >> l ? s->w >> l : 1, lh = s->h >> l ? s->h >> l : 1;
        s->tiles_x[l] = (lw + s->ntw - 1) / s->ntw, s->tiles_y[l] = (lh + s->nth - 1) / s->nth, s->tile0[l] = page;
        page += s->tiles_x[l] * s->tiles_y[l];
        s->stride = page;
        if (l < s->nlev) {
            s->bx[l] = (lw + s->gw - 1) / s->gw, s->blk0[l] = blk;
            blk += s->bx[l] * ((lh + s->gh - 1) / s->gh);
            s->nblk = blk;
        }
    }
    if (!(s->bound = calloc((size_t)s->layers * s->nblk, sizeof *s->bound)) ||
        !(s->dirty = calloc((size_t)s->layers * s->nblk, 1)))
        rt_fatal("vk: out of memory (sparse table)");
    tail_alloc(c, s, nr ? &sr : NULL, ci.mipLevels, mr.memoryTypeBits);
    tail_bind_now(c, s);
    sp_take();
    s->next = images, images = s;
    if (s->tail_pending) ntail_pending++;
    n_images++;
    // Coarse levels first, so a full budget leaves fine detail out rather than whole layers.
    for (int l = (int)s->nlev - 1; l >= 0; l--)
        for (uint32_t y = 0; y < s->layers; y++) {
            uint64_t lo = s->addr + ((uint64_t)y * s->stride + s->tile0[l]) * PAGE;
            bind_range(c, s, lo, lo + (uint64_t)s->tiles_x[l] * s->tiles_y[l] * PAGE);
        }
    int64_t ni = n_images, ns = n_slots;
    sp_give();
    if (ni <= 16 || !(ni & (ni - 1)))
        rt_log("vk: sparse image %lld: %#llx %ux%u layers %u VkFormat %d, NVN tile %ux%u (%u levels, %u pages a layer), "
               "Vulkan block %ux%u of %llu bytes, mip tail from level %u; %lld slots live", (long long)ni,
               (unsigned long long)addr, s->w, s->h, s->layers, (int)ci.format, s->ntw, s->nth, (unsigned)lay[0],
               s->stride, s->gw, s->gh, (unsigned long long)s->slot_size, nr ? sr.imageMipTailFirstLod : 0, (long long)ns);
    *out = s->img;
    return 1;
}

uint64_t vk_sparse_destroy(VkImage img) {
    uint64_t bind_fence = 0;
    sp_take();
    SparseImg **pp = &images;
    while (*pp && (*pp)->img != img) pp = &(*pp)->next;
    SparseImg *s = *pp;
    if (s) {
        *pp = s->next;
        bind_fence = s->bind_fence;
        // Whether two sets of the terrain arrays live at once or one replaces the other: what sharing one VA reservation
        // between them would save on the PS5 (sparse_census).
        if (s->size > SMALL_SPARSE)
            rt_log("vk: sparse: destroying %#llx %ux%u layers %u VkFormat %d (VA %llu MiB); %lld images left", (unsigned long long)s->addr,
                   s->w, s->h, s->layers, (int)s->fmt, (unsigned long long)(s->va_bytes >> 20), (long long)n_images - 1);
        for (size_t i = 0; i < (size_t)s->layers * s->nblk; i++)
            if (s->bound[i]) slot_unref(s->bound[i]);
        if (s->ndl) ndirty--;   // binds not issued yet: the image goes away first
        if (s->tail_pending) ntail_pending--;
        if (s->tail_alloc) {
            if (ndying_tails == cdying_tails) grow((void **)&dying_tails, &cdying_tails, sizeof *dying_tails);
            dying_tails[ndying_tails++] = s->tail_alloc;
        }
        free(s->bound);
        free(s->dirty);
        free(s->dl);
        free(s);
        n_images--;
        destroy_gen++;
    }
    sp_give();
    return bind_fence;
}

int vk_sparse_is(VkImage img) {
    sp_take();
    const SparseImg *s = images;
    while (s && s->img != img) s = s->next;
    sp_give();
    return s != NULL;
}

void vk_sparse_remap(uint64_t lo, uint64_t size) {
    sp_take();
    VkCtx *c = vk_ctx();
    for (SparseImg *s = images; s; s = s->next)
        if (lo < s->addr + s->size && s->addr < lo + size) bind_range(c, s, lo, lo + size);
    sp_give();
}

// Eglantine (d4ee7fcd51) found 13-26% of MKW's sparse terrain blocks written and nine layer pools never written: this
// counts the slots a copy or a render pass writes, to measure how much slot memory only ever holds zeros. w == 0: the
// whole level.
void vk_sparse_touch(VkImage img) {
    if (!img) return;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    sp_take();
    for (SparseImg *s = images; s; s = s->next)
        if (s->img == img) {
            s->last_sample_frame = f;
            s->sample_count++;
            break;
        }
    sp_give();
}

void vk_sparse_vram_log(uint64_t frame) {
    if (!plat_mutex_trylock(&sp_lock)) return;
    __atomic_store_n(&sp_owner, hang_tid(), __ATOMIC_RELAXED);
    if (!images && !n_slots && !n_blocks) {
        sp_give();
        return;
    }
    uint64_t cur_f = frame ? frame : __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    uint64_t tail_bytes = 0;
    uint64_t act_i = 0, act_s = 0, act_l0 = 0;
    uint64_t idle_i = 0, idle_s = 0, idle_l0 = 0;
    uint64_t nev_i = 0, nev_s = 0, nev_l0 = 0;
    char top[320] = "";
    int tk = 0, ntop = 0;
    for (SparseImg *s = images; s; s = s->next) {
        tail_bytes += s->tail_bytes;
        uint64_t bound_n = 0, l0_n = 0;
        for (uint32_t ly = 0; ly < s->layers; ly++) {
            size_t base = (size_t)ly * s->nblk;
            uint32_t l0_end = s->nlev > 1 ? s->blk0[1] : s->nblk;
            for (uint32_t k = 0; k < s->nblk; k++) {
                if (!s->bound[base + k]) continue;
                bound_n++;
                if (k < l0_end) l0_n++;
            }
        }
        if (!s->sample_count) {
            nev_i++, nev_s += bound_n, nev_l0 += l0_n;
        } else if (cur_f <= s->last_sample_frame || cur_f - s->last_sample_frame <= 60) {
            act_i++, act_s += bound_n, act_l0 += l0_n;
        } else {
            idle_i++, idle_s += bound_n, idle_l0 += l0_n;
        }
        if (bound_n >= 256 && ntop < 4 && tk < (int)sizeof top - 80) {
            uint64_t age = !s->sample_count ? (cur_f > s->created_frame ? cur_f - s->created_frame : 0)
                                            : (cur_f > s->last_sample_frame ? cur_f - s->last_sample_frame : 0);
            tk += snprintf(top + tk, sizeof top - (size_t)tk, " %s%#llx(%ux%ux%u b=%llu L0=%llu %s%llu)",
                           ntop ? "," : "", (unsigned long long)s->addr, s->w, s->h, s->layers,
                           (unsigned long long)bound_n, (unsigned long long)l0_n,
                           s->sample_count ? "age=" : "nev=", (unsigned long long)age);
            ntop++;
        }
    }
    const double M = 1048576.0;
    uint64_t slot_sz = blocks ? blocks->slot_size : 65536ull;
    rt_log("mem: sparse frame %llu: %lld blocks (%.0f MiB), %lld slots (%.0f MiB, %lld written), tails %.0f MiB; "
           "sampled active<=60f %llu imgs/%llu blks (L0 %llu), idle>60f %llu imgs/%llu blks (L0 %llu), never %llu imgs/%llu blks (L0 %llu)%s",
           (unsigned long long)frame, (long long)n_blocks, (double)n_blocks * BLOCK_SLOTS * slot_sz / M,
           (long long)n_slots, (double)n_slots * slot_sz / M, (long long)n_written, tail_bytes / M,
           (unsigned long long)act_i, (unsigned long long)act_s, (unsigned long long)act_l0,
           (unsigned long long)idle_i, (unsigned long long)idle_s, (unsigned long long)idle_l0,
           (unsigned long long)nev_i, (unsigned long long)nev_s, (unsigned long long)nev_l0, top);
    sp_give();
}

uint64_t vk_sparse_trim(uint64_t bytes) {
    static int evict_mode = -1;
    if (evict_mode < 0) evict_mode = rt_knob("SPARSE_EVICT", "0");
    if (!RT_KNOB_ON(evict_mode) || !bytes) return 0;

    enum { IDLE_FRAMES = 300 };
    uint64_t cur_f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    uint64_t freed_bytes = 0;

    if (!plat_mutex_trylock(&sp_lock)) return 0;
    __atomic_store_n(&sp_owner, hang_tid(), __ATOMIC_RELAXED);
    for (int pass = 0; pass < 2 && freed_bytes < bytes; pass++) {
        for (SparseImg *s = images; s && freed_bytes < bytes; s = s->next) {
            if (s->size <= SMALL_SPARSE || !s->nlev) continue;
            uint64_t last = s->last_sample_frame > s->last_write_frame ? s->last_sample_frame : s->last_write_frame;
            if (!last) last = s->created_frame;
            if (cur_f <= last || cur_f - last <= IDLE_FRAMES) continue;
            // First unbind fine levels (L0..nlev-2), keeping coarsest level resident; on second pass unbind all.
            uint32_t max_l = (pass == 0 && s->nlev > 1) ? s->nlev - 1 : s->nlev;
            for (uint32_t l = 0; l < max_l && freed_bytes < bytes; l++) {
                uint32_t start = s->blk0[l];
                uint32_t end = (l + 1 < s->nlev) ? s->blk0[l + 1] : s->nblk;
                for (uint32_t ly = 0; ly < s->layers && freed_bytes < bytes; ly++) {
                    size_t base = (size_t)ly * s->nblk;
                    for (uint32_t b = start; b < end && freed_bytes < bytes; b++) {
                        Slot *sl = s->bound[base + b];
                        if (!sl) continue;
                        int32_t before = sl->refs;
                        slot_unref(sl);
                        s->bound[base + b] = NULL;
                        mark_dirty(s, base + b);
                        if (before == 1) freed_bytes += s->slot_size;
                    }
                }
            }
        }
    }
    sp_give();
    return freed_bytes;
}

void vk_sparse_written(VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int32_t x, int32_t y, uint32_t w,
                       uint32_t h) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    sp_take();
    SparseImg *s = images;
    while (s && s->img != img) s = s->next;
    if (s) s->last_write_frame = f;
    if (s && level >= s->nlev && level < 32 && !(s->tail_written >> level & 1)) {
        s->tail_written |= 1u << level, n_tail_writes++;
        rt_log("vk: sparse: first write to level %u of %ux%u layers %u (outside the tiles; %s)", level, s->w, s->h,
               s->layers, s->tail_alloc ? "has memory" : "no memory: lost");
    }
    if (s && level < s->nlev) {
        uint32_t lw = s->w >> level ? s->w >> level : 1, lh = s->h >> level ? s->h >> level : 1;
        if (!w) x = y = 0, w = lw, h = lh;
        uint32_t bx0 = (uint32_t)x / s->gw, by0 = (uint32_t)y / s->gh;
        uint32_t bx1 = ((uint32_t)x + w + s->gw - 1) / s->gw, by1 = ((uint32_t)y + h + s->gh - 1) / s->gh;
        uint32_t rows = (lh + s->gh - 1) / s->gh;
        if (bx1 > s->bx[level]) bx1 = s->bx[level];
        if (by1 > rows) by1 = rows;
        for (uint32_t l = layer0; l < layer0 + layers && l < s->layers; l++)
            for (uint32_t by = by0; by < by1; by++)
                for (uint32_t bx = bx0; bx < bx1; bx++) {
                    Slot *sl = s->bound[(size_t)l * s->nblk + s->blk0[level] + by * s->bx[level] + bx];
                    if (sl && !sl->written) sl->written = 1, n_written++;
                }
    }
    sp_give();
}

// Queue lock held (it guards order_tl and its values), before a submission to the graphics queue: gives the order of
// that submission (wait for the last bind, signal its own value). binds: first issue the queued binds behind every
// earlier submission (vk_sync.c, and vk_upload_flush before it takes cmd_lock, which slot_get takes under sp_lock: the
// lock order is the queue lock, sp_lock, cmd_lock). fence:
// the timeline value whose completion frees the slots unbound so far (a part or the end of a replayed submission).
// MKW_SPARSE_ORDER=0 (A/B): the order before 31/10/2026, binds behind the previous frame's value only, and no wait or
// signal on the other submissions. binds 2: only issue the queued binds, before the upload batch that a replayed part
// flushes ahead of itself (vk_sync.c), so the batch's copies into sparse levels land in bound memory.
static int order_on(void) {
    static int v = -1;
    if (v < 0) v = !(cfg_get("MKW_SPARSE_ORDER") && !atoi(cfg_get("MKW_SPARSE_ORDER")));
    return v;
}

// Read without sp_lock: a bind queued after this read is issued by the next split, submit or flush.
int vk_sparse_binds_queued(void) {
    return __atomic_load_n(&ndirty, __ATOMIC_RELAXED) || __atomic_load_n(&ntail_pending, __ATOMIC_RELAXED);
}

VkSparseOrder vk_sparse_order(uint64_t fence, int binds) {
    VkCtx *c = vk_ctx();
    int on_ = order_on();
    if (!on_ && binds != 1) return (VkSparseOrder){0};
    if (on_ && !order_tl) {
        VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
        if (vkCreateSemaphore(c->device, &si, NULL, &order_tl) != VK_SUCCESS) rt_fatal("vk: sparse order semaphore");
    }
    if (!binds) return (VkSparseOrder){order_tl, order_tl, bind_val, ++order_val};
    static int64_t ncalls;
    int log_bind = 0;
    hang_report_once();
    if (__atomic_load_n(&ncalls, __ATOMIC_RELAXED) < 4) {
        const char *prev = hang_mark("sparse: before-sparse-bind census");
        plat_mem_census_log("before-sparse-bind");
        hang_mark(prev);
        log_bind = 1;
    }
    // Binds queued (read without sp_lock: one queued after this read is bound with RADV's own wait, under sp_lock).
    if (on_ && cpu_wait_mode() == 1 &&
        (__atomic_load_n(&ndirty, __ATOMIC_RELAXED) || __atomic_load_n(&ntail_pending, __ATOMIC_RELAXED)))
        cpu_wait(c, order_val);
    sp_take();
    if (fence) {
        for (int i = 0; i < ndying; i++) vk_completion_add(fence, slot_reap, dying[i]);
        ndying = 0;
        for (int i = 0; i < ndying_tails; i++) vk_completion_add(fence, tail_free, dying_tails[i]);
        ndying_tails = 0;
    }
    if (ndirty || ntail_pending) {
        int ni = 0, nb = 0, total = 0, no = 0, nm = 0, mtotal = 0;
        for (SparseImg *s = images; s; s = s->next) {
            total += s->ndl;
            if (s->tail_pending) total += (int)(s->layers * s->tail_blocks), mtotal += (int)tail_count(s);
        }
        VkSparseImageMemoryBindInfo *ib = malloc(sizeof *ib * (size_t)(ndirty + ntail_pending));
        VkSparseImageMemoryBind *bb = malloc(sizeof *bb * (size_t)(total ? total : 1));
        VkSparseImageOpaqueMemoryBindInfo *ob = malloc(sizeof *ob * (size_t)(ntail_pending ? ntail_pending : 1));
        VkSparseMemoryBind *mb = malloc(sizeof *mb * (size_t)(mtotal ? mtotal : 1));
        if (!ib || !bb || !ob || !mb) rt_fatal("vk: out of memory (sparse binds)");
        for (SparseImg *s = images; s; s = s->next) {
            if (!s->tail_pending) continue;
            tail_binds(s, &ib[ni], bb + nb, &ob[no], mb + nm);
            s->bind_fence = fence;
            nb += (int)ib[ni].bindCount, nm += (int)ob[no].bindCount;
            if (ib[ni].bindCount) ni++;
            if (ob[no].bindCount) no++;
            s->tail_pending = 0;
        }
        ntail_pending = 0;
        for (SparseImg *s = images; s; s = s->next) {
            if (!s->ndl) continue;
            ib[ni++] = (VkSparseImageMemoryBindInfo){.image = s->img, .bindCount = (uint32_t)s->ndl, .pBinds = bb + nb};
            s->bind_fence = fence;   // submission `fence` waits for this bind (bind_sem, or order_tl)
            for (int k = 0; k < s->ndl; k++) bb[nb++] = make_bind(s, s->dl[k]), s->dirty[s->dl[k]] = 0;
            s->ndl = 0;
        }
        ndirty = 0;
        uint64_t wait = on_ ? order_val : fence - 1, sig = on_ ? ++order_val : 0;
        if (!on_ && !bind_sem) {
            VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateSemaphore(c->device, &si, NULL, &bind_sem) != VK_SUCCESS) rt_fatal("vk: sparse bind semaphore");
        }
        VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                            .waitSemaphoreValueCount = wait ? 1u : 0u, .pWaitSemaphoreValues = &wait,
                                            .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sig};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &tl,
                               .waitSemaphoreCount = wait ? 1u : 0u, .pWaitSemaphores = on_ ? &order_tl : &c->timeline,
                               .imageOpaqueBindCount = (uint32_t)no, .pImageOpaqueBinds = ob,
                               .imageBindCount = (uint32_t)ni, .pImageBinds = ib, .signalSemaphoreCount = 1,
                               .pSignalSemaphores = on_ ? &order_tl : &bind_sem};
        if (on_) bind_val = sig;
        else binary_pending = 1;
        if (on_ && cpu_wait_mode() == 2) cpu_wait(c, wait);
        uint64_t t0 = plat_now_ns();
        const char *prev = hang_mark_val("sparse: vkQueueBindSparse", sig);
        VkResult r = vkQueueBindSparse(c->gfx_queue, 1, &bi, VK_NULL_HANDLE);
        hang_mark(prev);
        if (r == VK_SUCCESS && on_) vk_sparse_order_submitted(sig);
        double bind_ms = (double)(plat_now_ns() - t0) / 1e6;
        if (r != VK_SUCCESS || bind_ms > 100.0)
            rt_log("vk: sparse bind %d blocks in %d images took %.1f ms (VkResult=%d)",
                   nb, ni, bind_ms, (int)r);
        if (r != VK_SUCCESS) {
            if (r == VK_ERROR_DEVICE_LOST) vk_exec_recent_log();
            rt_fatal("vk: vkQueueBindSparse of %d binds failed (%d)", nb, (int)r);
        }
        n_binds += nb;
        int64_t calls = __atomic_add_fetch(&ncalls, 1, __ATOMIC_RELAXED);
        if (calls <= 4 || !(calls & (calls - 1)))
            rt_log("vk: sparse bind %lld: %d blocks in %d images (%lld blocks so far, %lld slots live, %lld MiB, "
                   "%lld written)", (long long)calls, nb, ni, (long long)n_binds, (long long)n_slots,
                   (long long)(n_slots * 65536 >> 20), (long long)n_written);
        if (nb >= 256) census_locked("after a bind of 256 blocks or more");
        if (nb >= 256) log_bind = 1;
        free(ib);
        free(bb);
        free(ob);
        free(mb);
    }
    if (binds == 2) {   // the binds only: no submission follows to take a value
        sp_give();
        return (VkSparseOrder){0};
    }
    if (log_bind) {
        sp_give();
        const char *prev = hang_mark("sparse: after-sparse-bind census");
        plat_mem_census_log("after-sparse-bind");
        hang_mark(prev);
        sp_take();
    }
    VkSparseOrder o = {order_tl, order_tl, bind_val, ++order_val};
    if (!on_) o = (VkSparseOrder){binary_pending ? bind_sem : VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0}, binary_pending = 0;
    sp_give();
    return o;
}

// ---- runtime/gfx/cap: the written slots out (key and texels), and back in as the slots are made again
uint64_t vk_sparse_dump(void (*emit)(const void *key, uint32_t klen, const void *p, uint64_t n, void *ctx), void *ctx) {
    vk_sync_fence_wait(vk_sync_issued(), UINT64_MAX);
    vk_upload_flush(1);
    sp_take();
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < slots.cap; i++) {
        Slot *sl = slots.vals ? slots.vals[i] : NULL;
        if (!sl || !sl->written) continue;
        VkDeviceSize n = sl->blk->slot_size;
        VkBuffer sb;
        VmaAllocation sa;
        uint8_t *map = vk_staging(n, &sb, &sa);
        VkCommandBuffer cb = vk_upload_begin();
        VkBufferCopy cp = {(VkDeviceSize)sl->idx * n, 0, n};
        vkCmdCopyBuffer(cb, sl->blk->buf, sb, 1, &cp);
        vk_upload_end();
        vk_upload_flush(1);
        vmaInvalidateAllocation(vk_ctx()->vma, sa, 0, VK_WHOLE_SIZE);
        emit(&sl->key, sizeof sl->key, map, n, ctx);
        vmaDestroyBuffer(vk_ctx()->vma, sb, sa);
        bytes += n;
    }
    sp_give();
    return bytes;
}

void vk_sparse_load(const void *key, uint32_t klen, const void *p, uint64_t n) {
    if (klen != sizeof(SlotKey)) rt_fatal("vk: sparse slot key of %u bytes in a capture", klen);
    uint8_t *copy = malloc(n);
    if (!copy) rt_fatal("vk: out of memory (sparse slot)");
    memcpy(copy, p, n);
    sp_take();
    free(nvn_hm_put(&cap_pending, key, copy));
    sp_give();
}
