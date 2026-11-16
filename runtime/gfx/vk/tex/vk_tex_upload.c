#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_tex_internal.h"
#include "ui/glyphs/glyphs.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- uploads
static uint32_t texel_alpha_one(int format, uint32_t *elem) {
    switch (format) {
    case 0x19: *elem = 1; return 0xFF;
    case 0x1A: *elem = 1; return 0x7F;
    case 0x1B: case 0x1C: *elem = 1; return 1;
    case 0x1D: *elem = 2; return 0x3C00;
    case 0x1E: *elem = 2; return 0xFFFF;
    case 0x1F: *elem = 2; return 0x7FFF;
    case 0x20: case 0x21: *elem = 2; return 1;
    case 0x22: *elem = 4; return 0x3F800000;
    default: *elem = 4; return 1;   // 0x23, 0x24
    }
}

// 3-component texels to 4 (n texels).
void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e, one = texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 3 * e, dst += 4 * e) {
        memcpy(dst, src, 3 * e);
        memcpy(dst + 3 * e, &one, e);
    }
}

void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e;
    texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 4 * e, dst += 3 * e) memcpy(dst, src, 3 * e);
}

// MKW_ASTC_BC_CHECK=N (diagnosis, task 40; N > 0 turns it on): each ASTC upload re-encoded as BC7 or BC1 (2D, arrays and cube maps, up to
// 64 MiB of texels) is read back through a blit to RGBA8 and compared with astc_decode_ldr of its blocks: what sampling
// the BC image gives against what the RGBA8 image gave. Sums for arrays and for single images apart (the arrays went to
// BC with their layers counted), and the first N textures one line each. Waits for the GPU at every upload.
static void bc_check(const Tex *e, const uint8_t *raw, const VkAstcRegion *ar, int n, int lines) {
    static volatile int32_t n_logged;
    static struct { uint64_t tex, texels, sum, worst_tex; int worst; double worst_psnr; } cls[2];   // [array]
    static PlatMutex lock = PLAT_MUTEX_INIT;
    VkCtx *c = vk_ctx();
    uint64_t texels = 0;
    for (int k = 0; k < n; k++) texels += (uint64_t)ar[k].w * ar[k].h * ar[k].slices;
    if (e->pub.type != VK_IMAGE_TYPE_2D || texels * 4 > (64ull << 20)) return;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = e->f.astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {e->pub.width, e->pub.height, 1}, .mipLevels = e->pub.levels,
                            .arrayLayers = e->pub.layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation alloc;
    if (vmaCreateImage(c->vma, &ci, &ai, &img, &alloc, NULL) != VK_SUCCESS) return;
    VkBuffer hb;
    VmaAllocation ha;
    VkCommandBuffer cb = vk_upload_begin();
    uint8_t *map = vk_staging(texels * 4, &hb, &ha);
    VkBufferImageCopy *cp = calloc((size_t)n, sizeof *cp);
    if (!cb || !map || !cp) rt_fatal("vk: MKW_ASTC_BC_CHECK: no staging");
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                                   VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    barrier(cb, ALL_RW, ALL_RW);
    VkDeviceSize off = 0;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r->level, r->layer, 1},
                          .srcOffsets = {{0, 0, 0}, {(int32_t)r->w, (int32_t)r->h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r->level, r->layer, 1},
                          .dstOffsets = {{0, 0, 0}, {(int32_t)r->w, (int32_t)r->h, 1}}};
        vkCmdBlitImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
        cp[k] = (VkBufferImageCopy){.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r->level, r->layer, 1},
                                    .imageExtent = {r->w, r->h, 1}};
        off += (VkDeviceSize)r->w * r->h * 4;
    }
    barrier(cb, ALL_RW, ALL_RW);
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, hb, (uint32_t)n, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    vk_upload_flush(1);
    vmaInvalidateAllocation(c->vma, ha, 0, VK_WHOLE_SIZE);
    uint32_t bw = e->f.astc_bw, bh = e->f.astc_bh;
    uint64_t sum = 0, sq = 0;
    int worst = 0;
    const uint8_t *got = map;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        size_t st = (size_t)r->w * r->h * 4;
        uint8_t *want = malloc(st);
        astc_decode_ldr(raw + r->src, r->w, r->h, bw, bh, e->f.astc_srgb, want);
        for (size_t i = 0; i < st; i++) {
            int d = abs((int)want[i] - (int)got[i]);
            sum += (uint64_t)d, sq += (uint64_t)d * (uint64_t)d;
            if (d > worst) worst = d;
        }
        free(want);
        got += st;
    }
    double mse = (double)sq / (double)(texels * 4), psnr = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99.0;
    int array = e->pub.layers > 1;
    plat_mutex_lock(&lock);
    cls[array].tex++, cls[array].texels += texels, cls[array].sum += sum;
    if (psnr < cls[array].worst_psnr || !cls[array].worst_tex)
        cls[array].worst_psnr = psnr, cls[array].worst = worst, cls[array].worst_tex = e->pub.addr;
    uint64_t all = cls[0].tex + cls[1].tex;
    if (plat_inc32(&n_logged) <= lines)
        rt_log("vk: ASTC BC check: %#llx %ux%u x%u layers, %u levels, ASTC %ux%u -> %s: mean error %.3f, largest %d, PSNR "
               "%.1f dB", (unsigned long long)e->pub.addr, e->pub.width, e->pub.height, e->pub.layers, e->pub.levels, bw, bh,
               e->f.astc_bc7 == 2 ? "BC1" : "BC7", (double)sum / (double)(texels * 4), worst, psnr);
    if (all % 32 == 0 || all < 4)
        for (int a = 0; a < 2; a++)
            if (cls[a].tex)
                rt_log("vk: ASTC BC check: %s: %llu textures, %.1f M texels, mean error %.3f per channel; worst PSNR %.1f dB "
                       "(%#llx, largest error %d)", a ? "arrays and cube maps" : "single images",
                       (unsigned long long)cls[a].tex, (double)cls[a].texels / 1e6,
                       (double)cls[a].sum / (double)(cls[a].texels * 4), cls[a].worst_psnr,
                       (unsigned long long)cls[a].worst_tex, cls[a].worst);
    plat_mutex_unlock(&lock);
    free(cp);
    vmaDestroyBuffer(c->vma, hb, ha);
    vk_defer_image(img, alloc);
}

// MKW_ASTC=check: the compute decoder's texels of n regions (gpu: the regions one after the other) against
// astc_decode_ldr of the same raw blocks (raw: the staging bytes the regions point into).
static void astc_check(const Tex *e, const uint8_t *raw, const VkAstcRegion *ar, int n, const uint8_t *gpu) {
    static volatile int32_t n_bad;
    static uint64_t n_tex, n_texels, n_diff_tex;
    uint32_t bw = e->f.astc_bw, bh = e->f.astc_bh;
    uint64_t diff = 0, first = UINT64_MAX;
    uint32_t want = 0, got = 0;
    int first_k = 0;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        size_t sb = (size_t)((r->w + bw - 1) / bw) * ((r->h + bh - 1) / bh) * 16, st = (size_t)r->w * r->h * 4;
        uint8_t *cpu = malloc(st);
        for (uint32_t z = 0; z < r->slices; z++, gpu += st) {
            astc_decode_ldr(raw + r->src + z * sb, r->w, r->h, bw, bh, e->f.astc_srgb, cpu);
            if (!memcmp(cpu, gpu, st)) continue;
            for (size_t i = 0; i < st / 4; i++)
                if (memcmp(cpu + 4 * i, gpu + 4 * i, 4)) {
                    if (first == UINT64_MAX) {
                        first = i, first_k = k;
                        memcpy(&want, cpu + 4 * i, 4), memcpy(&got, gpu + 4 * i, 4);
                    }
                    diff++;
                }
        }
        free(cpu);
        n_texels += (uint64_t)r->w * r->h * r->slices;
    }
    n_tex++;
    if (diff) {
        n_diff_tex++;
        log_limited(&n_bad, "ASTC check: texture %#llx fmt %#x %ux%u: %llu texels differ, first region %d texel %llu "
                    "cpu %08x gpu %08x", (unsigned long long)e->pub.addr, e->t.d.format, bw, bh,
                    (unsigned long long)diff, first_k, (unsigned long long)first, want, got);
    }
    if (n_tex == 1 || !(n_tex & 63) || diff)
        rt_log("vk: ASTC check: %llu textures, %llu texels compared, %llu textures differ", (unsigned long long)n_tex,
               (unsigned long long)n_texels, (unsigned long long)n_diff_tex);
}

// An ASTC texture re-encoded as BC7: the BC7 encoded offline for these storage bytes (vk_texpre.c) straight into the
// image. Without one, the storage is recorded for the offline encoder and 0 sends the upload on through the GPU encoder.
static int upload_texpre(Tex *e, uint32_t levels, uint32_t layers) {
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t n = levels * layers, k = 0;
    VkTexpreRegion *pr = calloc(n, sizeof *pr);
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = mip(d->width, lv), h = one_d ? 1 : mip(d->height, lv), sl = is3d ? mip(d->depth, lv) : 1;
            pr[k] = (VkTexpreRegion){w, h, sl, lv, ly};
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                        .imageExtent = {w, h, sl}};
            total += vk_texpre_region_bytes(&pr[k]);
        }
    uint64_t h = vk_texpre_hash(t->mem, t->L.size);
    int ok = 0;
    if (vk_texpre_has(h)) {
        VkBuffer sb;
        uint8_t *stg;
        VkCommandBuffer cb = vk_upload_begin_staging(total, &sb, (void **)&stg);
        if (cb) {   // no staging: upload() tries its own, smaller or deferred
            if (vk_texpre_load(h, stg, total)) {
                barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
                barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
                ok = 1;
            }
            vk_upload_end();
        }
    }
    if (!ok) vk_texpre_record(h, d, pr, (int)n, t->mem, t->L.size);
    free(pr), free(rg);
    return ok;
}

// Copies the whole storage into the image (every level and layer the image has).
// MKW_TRACE_ADDR=<hex> (diagnosis): the texture whose storage starts there: its GPU writes (vk_texture_gpu_written),
// uploads and WriteTexels, the first 64 events.
uint64_t trace_addr = 1;
static volatile int32_t traced;
// MKW_TRACE_ADDR=all (diagnosis): every texture, up to 20 000 lines: two replays of a capture told apart by their
// uploads and GPU writes (tools/test: a recording order that changes the image).
int tracing(uint64_t addr) {
    if (trace_addr == 1) {
        const char *e = cfg_get("MKW_TRACE_ADDR");
        trace_addr = e && !strcmp(e, "all") ? UINT64_MAX : e && *e ? strtoull(e, NULL, 16) : 0;
    }
    if (trace_addr == UINT64_MAX) return plat_inc32(&traced) <= 20000;
    return trace_addr && addr == trace_addr && plat_inc32(&traced) <= 64;
}

// The button icon sheet (ui/glyphs/glyphs.c): its texture object and the icons it was drawn with. When they change it is
// uploaded again, redrawn, at the next present (vk_backend.c, the game thread, after the queued replays). Waiting for its next storage check was not enough: after the first
// upload the binding memos never check it again while its pages stay clean (gl4, gl5, 06/11/2026).
static uint64_t g_glyph_addr;
static int g_glyph_known;
static uint32_t g_glyph_gen;
static int upload(Tex *e);

int vk_glyphs_due(void) {
    if (!__atomic_load_n(&g_glyph_known, __ATOMIC_ACQUIRE)) return 0;
    static uint64_t last_ns;
    uint64_t now = plat_now_ns();
    if (now - last_ns < 100000000ull) return 0;   // ten times a second: a device switch shows within a few frames
    last_ns = now;
    return glyphs_generation() != g_glyph_gen;
}

// Every live entry at the sheet's address: the game samples it through more than one (another view of the same
// storage); uploading the one entry_of found again changed nothing on screen (gl6, 06/11/2026).
void vk_glyphs_redo(void) {
    lock();
    g_glyph_gen = glyphs_generation();
    for (Tex *e = entries_at(g_glyph_addr); e; e = e->next_at)
        if (e->uploaded && !e->released && e->pub.mirrored && !e->pub.gpu_owned) upload(e);
    unlock();
}

// MKW_TEX_UP_LOG's content checks (diagnosis, task 32: magenta bands on the character-select cards, right of a step in
// every card, the shape of a byte range of a block-linear texture; ASTC blocks that are not valid 4x4 blocks decode to
// magenta). The cards (275x362 ASTC 4x4, 144 KiB, CPU-written storage) were uploaded up to 6 times in 12 frames as the
// game wrote them, never with a zero tail. Each ASTC upload of 64 KiB or more keeps a hash per 4 KiB page of what it
// read (the latest upload of an address replaces the earlier one); tex_up_recheck hashes the pages again 300 frames
// later: a page changed with no upload since is a write the runtime missed, and its offset places it on the card.
#define ZT_MAX 256
#define ZT_PAGES 64
static struct { uint64_t addr, size, ztail, frame; uint32_t n_up; int checked; uint64_t h[ZT_PAGES]; } zt[ZT_MAX];
static int nzt;
static PlatMutex zt_lock = PLAT_MUTEX_INIT;

// Offset of the first byte of [addr, addr + size)'s trailing run of zero bytes (size when the last byte is not zero).
static uint64_t zero_tail(uint64_t addr, uint64_t size) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    uint64_t n = size;
    while (n && !p[n - 1]) n--;
    return n;
}

static void page_hashes(uint64_t addr, uint64_t size, uint64_t *h) {   // FNV-1a over each 4 KiB page of the range
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    for (uint64_t k = 0; k < ZT_PAGES && k * 4096 < size; k++) {
        uint64_t x = 0xcbf29ce484222325ull, end = (k + 1) * 4096 < size ? (k + 1) * 4096 : size;
        for (uint64_t i = k * 4096; i < end; i++) x = (x ^ p[i]) * 0x100000001b3ull;
        h[k] = x;
    }
}

static void zero_tail_note(uint64_t addr, uint64_t size, uint64_t ztail, uint64_t frame, uint32_t n_up) {
    if (size > (uint64_t)ZT_PAGES * 4096) size = (uint64_t)ZT_PAGES * 4096;
    plat_mutex_lock(&zt_lock);
    int i = 0;
    while (i < nzt && zt[i].addr != addr) i++;
    if (i == nzt && nzt == ZT_MAX)   // full: an entry already checked makes room (the menus' filled it before the cards)
        for (i = 0; i < nzt && !zt[i].checked; i++) {}
    if (i == nzt && nzt < ZT_MAX) nzt++;
    if (i < nzt) {
        zt[i].addr = addr, zt[i].size = size, zt[i].ztail = ztail, zt[i].frame = frame, zt[i].n_up = n_up;
        zt[i].checked = 0;
        page_hashes(addr, size, zt[i].h);
    }
    plat_mutex_unlock(&zt_lock);
}

// From the texture lock's perf report (vk_tex_slots.c), once a minute: each noted upload 300 frames or more old, read
// again if its storage is still backed.
void tex_zero_tail_recheck(uint64_t frame) {
    plat_mutex_lock(&zt_lock);
    for (int i = 0; i < nzt; i++) {
        if (zt[i].checked || frame < zt[i].frame + 300) continue;
        zt[i].checked = 1;
        if (plat_mem_backed_bytes(zt[i].addr, zt[i].size) < zt[i].size) {
            rt_log("vk: upload check %#llx (upload %u at frame %llu): storage no longer backed, not read",
                   (unsigned long long)zt[i].addr, zt[i].n_up, (unsigned long long)zt[i].frame);
            continue;
        }
        uint64_t h[ZT_PAGES];
        page_hashes(zt[i].addr, zt[i].size, h);
        uint64_t npg = (zt[i].size + 4095) / 4096, changed = 0, first = npg, last = 0;
        for (uint64_t k = 0; k < npg; k++)
            if (h[k] != zt[i].h[k]) changed++, first = k < first ? k : first, last = k;
        rt_log("vk: upload check %#llx (%.1f KiB, upload %u at frame %llu, zero tail %#llx) at frame %llu: %s; %llu of "
               "%llu pages changed (offsets %#llx-%#llx)",
               (unsigned long long)zt[i].addr, zt[i].size / 1024.0, zt[i].n_up, (unsigned long long)zt[i].frame,
               (unsigned long long)zt[i].ztail, (unsigned long long)frame,
               changed ? "CHANGED with no upload since" : "unchanged", (unsigned long long)changed,
               (unsigned long long)npg, (unsigned long long)(changed ? first * 4096 : 0),
               (unsigned long long)(changed ? (last + 1) * 4096 : 0));
    }
    plat_mutex_unlock(&zt_lock);
}

// 1 uploaded (or nothing to upload); 0 deferred for want of staging memory (e->up_pending: tried again next frame).
static int upload(Tex *e) {
    static volatile int32_t n_bad, n_undecoded;
    uint64_t frame_now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (e->up_pending && e->up_fail_frame == frame_now) return 0;   // failed this frame already: no new attempt
    if (tracing(e->pub.addr))
        rt_log("vk: trace %#llx: upload %u of entry %p (%ux%u target %d), submit epoch %u", (unsigned long long)e->pub.addr,
               e->n_up + 1, (void *)e, e->t.d.width, e->t.d.height, e->t.d.target, submit_epoch);
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb || !t->mem) return 1;
    uint64_t p0 = perf_begin(), u0 = kern_now_ns();
    int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t levels = linear ? 1 : e->pub.levels, layers = linear ? 1 : e->pub.layers;
    int astc = e->f.astc_bw != 0, rgb = e->f.expand_rgb;
    // MKW_ASTC=check stages a CPU decode beside the GPU one: textures over 16 MiB stay unchecked (a 357 MB staging
    // buffer failed at a course load, vis22; 64 MiB still failed with the mip levels, vis24).
    int agpu = astc && vk_astc_gpu(),
        acheck = agpu && vk_astc_mode() == VK_ASTC_CHECK && (uint64_t)e->pub.width * e->pub.height * e->pub.layers * 4 <= (16ull << 20);
    uint32_t n = levels * layers, k = 0;
    if (agpu && e->f.astc_bc7 == 1 && !acheck && !linear && vk_texpre_on() && upload_texpre(e, levels, layers)) goto counted;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = agpu ? calloc(n, sizeof *ar) : NULL;
    VkDeviceSize total = 0, decoded = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t w = mip(d->width, lv), h = one_d ? 1 : mip(d->height, lv), sl = is3d ? mip(d->depth, lv) : 1;
            uint64_t texels = (uint64_t)w * h * sl, raw = (uint64_t)(w + bw - 1) / bw * bpb * ((h + bh - 1) / bh) * sl;
            uint64_t out = agpu ? raw : astc ? texels * 4 : rgb ? texels * (uint64_t)bpb / 3 * 4 : raw;
            rg[k++] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                          .imageExtent = {w, h, sl}};
            total += (out + 15) & ~15ull;
            decoded += texels * 4;
        }
    vk_upload_flush_over(64ull << 20);   // as vk_tex_texel.c: the load of a capture sent its textures in one batch
    VkBuffer sb, rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *stg;
    VkCommandBuffer cb = vk_upload_begin_staging(total, &sb, (void **)&stg);
    if (!cb) {   // out of staging memory: the image keeps what it had, and the upload comes again next frame
        free(ar);
        free(rg);
        e->up_pending = 1, e->up_fail_frame = frame_now;
        perf_end(PERF_TEX, p0);
        return 0;
    }
    uint8_t *rmap = acheck ? vk_staging(decoded, &rbuf, &ralloc) : NULL;
    k = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            size_t raw = (size_t)rb * rows * sl;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = (astc && !agpu) || rgb ? malloc(raw) : NULL;
            if (nvn_deswizzle(d, lv, ly, t->mem, tmp ? tmp : dst, rb)) {
                log_limited(&n_bad, "texture %#llx fmt %#x target %d: level %u layer %u has no CPU layout; uploaded as zeros",
                            (unsigned long long)e->pub.addr, d->format, d->target, lv, ly);
                memset(tmp ? tmp : dst, 0, raw);
            }
            if (agpu) {
                ar[k] = (VkAstcRegion){.src = rg[k].bufferOffset, .w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            } else if (astc) {
                uint64_t a0 = perf_begin();
                for (uint32_t z = 0; z < sl; z++)
                    astc_decode_ldr_mt(tmp + (size_t)z * rb * rows, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                                       dst + (size_t)z * w * h * 4);
                perf_end(PERF_ASTC, a0);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else if (rgb) {
                expand_rgb(d->format, tmp, dst, (size_t)w * h * sl);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else {
                rg[k].bufferRowLength = rb / (uint32_t)bpb * (uint32_t)bw, rg[k].bufferImageHeight = rows * (uint32_t)bh;
                if (lv == 0 && ly == 0 && sl == 1 && glyphs_apply(e->pub.addr, d->format, w, h, dst, rb)) {
                    g_glyph_addr = e->pub.addr;   // the button icon sheet (vk_glyphs_due)
                    g_glyph_gen = glyphs_generation();
                    __atomic_store_n(&g_glyph_known, 1, __ATOMIC_RELEASE);
                }
            }
            free(tmp);
        }
    static int bcc = -1;   // MKW_ASTC_BC_CHECK (bc_check)
    if (bcc < 0) bcc = cfg_get("MKW_ASTC_BC_CHECK") ? atoi(cfg_get("MKW_ASTC_BC_CHECK")) : 0;
    int bcheck = bcc > 0 && agpu && e->f.astc_bc7 && !acheck && !linear;
    uint8_t *raw_copy = acheck || bcheck ? malloc(total) : NULL;
    if (raw_copy) memcpy(raw_copy, stg, total);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int checked = 0;
    if (agpu) {
        uint64_t a0 = perf_begin();
        vk_astc_tag(e->pub.addr, 1);
        checked = vk_astc_record(cb, sb, total, ar, (int)n, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->pub.image,
                                 e->pub.aspect, e->f.astc_bc7, rbuf);
        perf_end(PERF_ASTC, a0);
        if (!checked)
            log_limited(&n_undecoded, "ASTC texture %#llx (fmt %#x %ux%u) not decoded on the GPU: left as it was",
                        (unsigned long long)e->pub.addr, d->format, d->width, d->height);
    } else {
        vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    }
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    if (acheck) {   // waits for the GPU
        vk_upload_flush(1);
        if (checked) {
            vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
            astc_check(e, raw_copy, ar, (int)n, rmap);
        }
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    if (bcheck && checked && raw_copy) bc_check(e, raw_copy, ar, (int)n, bcc);
    free(raw_copy);
    free(ar);
    free(rg);
counted:;
    uint64_t dt = kern_now_ns() - u0;
    if (e->uploaded) {
        n_reuploads++, perf_count(PERF_CNT_TEX_REUP);
        ru_n++, ru_bytes += t->L.size, ru_ns += dt;
        if (e->wt_n) {
            ru_wt_n++, ru_wt_bytes += t->L.size, ru_wt_ns += dt;
            if (rt_trace & RT_TRACE_PERF)
                rt_log("perf: frame %llu: re-upload after %u WriteTexels: texture %#llx fmt %#x %ux%u, %.1f KiB, %.3f ms",
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), e->wt_n,
                       (unsigned long long)e->pub.addr, d->format, d->width, d->height, t->L.size / 1024.0, dt / 1e6);
        }
    } else {
        n_uploads++, perf_count(PERF_CNT_TEX_UP);
    }
    // MKW_TEX_UP_LOG=N (diagnosis): the first N uploads from frame MKW_TEX_UP_LOG_AT (2300) on, with how many submits
    // the game had made past the one being recorded: their storage is read now, from guest memory (Hookshot: MKW's
    // terrain height tiles, 68x68 RGBA16 at new addresses every frame, rewritten right after their submit).
    static int up_log = -1, up_at;
    if (up_log < 0) {
        up_log = cfg_get("MKW_TEX_UP_LOG") ? atoi(cfg_get("MKW_TEX_UP_LOG")) : 0;
        up_at = cfg_get("MKW_TEX_UP_LOG_AT") ? atoi(cfg_get("MKW_TEX_UP_LOG_AT")) : 2300;
    }
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (up_log > 0 && fr >= (uint64_t)up_at) {
        up_log--;
        uint64_t ztail = zero_tail((uintptr_t)t->mem, t->L.size);
        rt_log("vk: upload at frame %llu: %#llx %ux%ux%u fmt %#x levels %u, %.1f KiB%s, game %lld submits ahead; "
               "zero tail from %#llx (%.0f%%)",
               (unsigned long long)fr, (unsigned long long)e->pub.addr, d->width, d->height, d->depth, d->format,
               e->pub.levels, t->L.size / 1024.0, e->uploaded ? " (again)" : "",
               (long long)(nvn_submit_count() - __atomic_load_n(&vk_replay_submit, __ATOMIC_RELAXED)),
               (unsigned long long)ztail, t->L.size ? 100.0 * (t->L.size - ztail) / t->L.size : 0.0);
        if (astc && t->L.size >= (64u << 10)) zero_tail_note((uintptr_t)t->mem, t->L.size, ztail, fr, e->n_up + 1);
        uint64_t rlo, rhi, rfreed, rframe;
        uint32_t rflags;
        if (vk_pool_release_find((uintptr_t)t->mem, t->L.size, &rlo, &rhi, &rfreed, &rframe, &rflags))
            rt_log("vk: upload %#llx (%ux%u fmt %#x) lies over pool-finalize release [%#llx,%#llx) flags %#x of frame "
                   "%llu (%.1f KiB released); its storage %.1f of %.1f KiB backed now",
                   (unsigned long long)(uintptr_t)t->mem, d->width, d->height, d->format, (unsigned long long)rlo,
                   (unsigned long long)rhi, rflags, (unsigned long long)rframe, rfreed / 1024.0,
                   plat_mem_backed_bytes((uintptr_t)t->mem, t->L.size) / 1024.0, t->L.size / 1024.0);
        // MKW_TEX_UP_DUMP=<w>x<h> (diagnosis, task 32): the raw guest bytes of the first 4 such uploads go to
        // MKW_TEX_UP_DUMP_DIR (/app0/logs), tex_<addr>_<w>x<h>_<fmt>.bin, to decode on the PC: a fault that is already in
        // guest memory past the cards' first 64 KiB boundary shows there.
        static unsigned dump_w, dump_h, dump_n;
        static int dump_init;
        if (!dump_init) {
            dump_init = 1;
            const char *dw = cfg_get("MKW_TEX_UP_DUMP");
            if (dw && sscanf(dw, "%ux%u", &dump_w, &dump_h) != 2) dump_w = dump_h = 0;
        }
        if (dump_w && d->width == dump_w && d->height == dump_h && dump_n < 4) {
            dump_n++;
            const char *dir = cfg_get("MKW_TEX_UP_DUMP_DIR");
            char path[512];
            snprintf(path, sizeof path, "%s/tex_%llx_%ux%u_%x.bin", dir && *dir ? dir : "/app0/logs",
                     (unsigned long long)(uintptr_t)t->mem, d->width, d->height, d->format);
            FILE *f = fopen(path, "wb");
            size_t wrote = f ? fwrite(t->mem, 1, t->L.size, f) : 0;
            if (f) fclose(f);
            rt_log("vk: upload dump %s: %zu of %llu bytes", path, wrote, (unsigned long long)t->L.size);
        }
    }
    // MKW_TEX_ZERO_LOG=N (diagnosis, 16/11/2026: magenta on scenery, karts and a road edge in some runs, at random
    // moments): the first N ASTC uploads with a whole 64 KiB unit of zero guest bytes. A zero 16-byte ASTC block is not
    // a valid block and decodes as the error colour, magenta, so real texture data never holds a zero unit; each check
    // stops at the first non-zero byte. The line says whether that unit is backed and frozen now and whether a
    // pool-finalize release covered it, to tell which path left the zeros.
    static int zlog = -1;
    if (zlog < 0) zlog = cfg_get("MKW_TEX_ZERO_LOG") ? atoi(cfg_get("MKW_TEX_ZERO_LOG")) : 0;
    if (zlog > 0 && astc && t->L.size >= (64u << 10)) {
        uint64_t lo = ((uintptr_t)t->mem + 0xffffu) & ~0xffffull, hi = ((uintptr_t)t->mem + t->L.size) & ~0xffffull;
        uint64_t first = 0, nz = 0, units = 0;
        for (uint64_t u = lo; u < hi; u += 0x10000, units++) {
            const uint64_t *q = (const uint64_t *)(uintptr_t)u;
            size_t k = 0;
            while (k < 0x10000 / 8 && !q[k]) k++;
            if (k == 0x10000 / 8) nz++, first = first ? first : u;
        }
        if (nz && __atomic_fetch_sub(&zlog, 1, __ATOMIC_RELAXED) > 0) {
            uint64_t rlo = 0, rhi = 0, rfreed = 0, rframe = 0;
            uint32_t rflags = 0;
            int rel = vk_pool_release_find(first, 0x10000, &rlo, &rhi, &rfreed, &rframe, &rflags);
            rt_log("vk: zero upload at frame %llu: texture %#llx %ux%ux%u fmt %#x levels %u, %.1f KiB, upload %u%s: %llu of "
                   "%llu units all zero, first %#llx (backed %.0f KiB, frozen %d)%s",
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
                   (unsigned long long)e->pub.addr, d->width, d->height, d->depth, d->format, e->pub.levels,
                   t->L.size / 1024.0, e->n_up + 1, e->uploaded ? " (again)" : "", (unsigned long long)nz,
                   (unsigned long long)units, (unsigned long long)first, plat_mem_backed_bytes(first, 0x10000) / 1024.0,
                   mem_frozen(first) != 0, rel ? "; under a pool-finalize release" : "");
            if (rel)
                rt_log("vk: zero upload %#llx: pool-finalize release [%#llx,%#llx) flags %#x of frame %llu (%.1f KiB)",
                       (unsigned long long)first, (unsigned long long)rlo, (unsigned long long)rhi, rflags,
                       (unsigned long long)rframe, rfreed / 1024.0);
        }
    }
    e->wt_n = 0;
    e->uploaded = 1;
    e->up_pending = 0;
    e->n_up++;
    perf_end(PERF_TEX, p0);
    return 1;
}

// A texture copy between ASTC textures that went to different Vulkan formats (one kept as RGBA8 under 256x256 into one
// re-encoded as BC7, BC1 against BC7): the source's own ASTC blocks, read from its storage, decoded and encoded
// straight into dst as bc7 says (TexVkFormat.astc_bc7: 0 RGBA8, 1 BC7, 2 BC1). MKW fills the small levels (5-11) of its sparse
// terrain arrays this way, from small ASTC textures; skipped, those levels read zero and the race's bushes and cacti,
// which take their colour from level 11, came out black (m16, 01/11/2026). 0 when it cannot (the caller skips).
int vk_texture_astc_copy(const tex_t *src, uint32_t level, uint32_t layer, uint32_t layers, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, VkImage dst, VkImageAspectFlags aspect, uint32_t dlevel, uint32_t dlayer,
                         int32_t dx, int32_t dy, int bc7) {
    TexVkFormat f;
    if (!tex_vk_format(src->d.format, &f) || !f.astc_bw || !src->mem || x % f.astc_bw || y % f.astc_bh ||
        (bc7 && (dx % 4 || dy % 4)))   // a BC destination: whole blocks
        return 0;
    uint32_t rb = (w + f.astc_bw - 1) / f.astc_bw * 16, rows = (h + f.astc_bh - 1) / f.astc_bh;
    size_t per = (size_t)rb * rows;
    VkAstcRegion *ar = calloc(layers, sizeof *ar);
    if (!ar) return 0;
    vk_upload_flush_over(64ull << 20);
    VkBuffer sb;
    uint8_t *stg;
    VkCommandBuffer cb = vk_upload_begin_staging(per * layers, &sb, (void **)&stg);
    if (!cb) {   // no staging memory: the caller skips the copy (logged by vk_upload_begin_staging)
        free(ar);
        return 0;
    }
    int ok = 1;
    for (uint32_t l = 0; l < layers && ok; l++) {
        ok = !nvn_texels_box(&src->d, level, layer + l, x / f.astc_bw * 16, y / f.astc_bh, 0, rb, rows, src->mem,
                             stg + l * per, rb, 1);
        ar[l] = (VkAstcRegion){.src = l * per, .w = w, .h = h, .slices = 1, .level = dlevel, .layer = dlayer + l,
                               .offset = {dx, dy, 0}};
    }
    // MKW_ASTC_PROBE_LEVEL=<n> (diagnosis): the first copies into level n read back the decoded texels and log them with
    // the source's first block (the bushes' level 11 of the terrain arrays sampled black, 01/11/2026).
    static int probe = -2;
    static volatile int32_t n_probed;
    if (probe == -2) probe = cfg_get("MKW_ASTC_PROBE_LEVEL") ? atoi(cfg_get("MKW_ASTC_PROBE_LEVEL")) : -1;
    VkBuffer rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *rmap = NULL;
    if (ok && probe == (int)dlevel && layers == 1 && w * h <= 64 && plat_inc32(&n_probed) <= 6)
        rmap = vk_staging((VkDeviceSize)w * h * 4, &rbuf, &ralloc);
    if (ok) {
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vk_astc_tag((uintptr_t)src->mem, 2);
        ok = vk_astc_record(cb, sb, per * layers, ar, (int)layers, f.astc_bw, f.astc_bh, f.astc_srgb, dst, aspect, bc7,
                            rbuf);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
    uint8_t block[16];
    memcpy(block, stg, 16);
    vk_upload_end();
    if (rmap) {
        vk_upload_flush(1);
        vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
        uint32_t sum[4] = {0};
        for (uint32_t i = 0; i < w * h; i++)
            for (int k = 0; k < 4; k++) sum[k] += rmap[i * 4 + k];
        char hex[33];
        for (int k = 0; k < 16; k++) snprintf(hex + 2 * k, 3, "%02x", block[k]);
        rt_log("vk: ASTC probe: level %u -> %u layer %u %ux%u: block %s, mean RGBA %u %u %u %u", level, dlevel, dlayer,
               w, h, hex, sum[0] / (w * h), sum[1] / (w * h), sum[2] / (w * h), sum[3] / (w * h));
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    free(ar);
    static volatile int32_t n_logged;
    if (plat_inc32(&n_logged) <= 8)
        rt_log("vk: ASTC %ux%u (fmt %#x) level %u layer %u x%u -> BC7 level %u layer %u at %d,%d: %s", w, h, src->d.format,
               level, layer, layers, dlevel, dlayer, dx, dy, ok ? "decoded" : "failed");
    return ok;
}

// vk_mem_changed over the pages wholly inside e's storage (all of its pages when it fills none): a neighbour that shares
// the first or last page does not count as a write to e.
int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now) {
    uint64_t lo = (e->pub.addr + 0xfff) & ~0xfffull, hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
    if (hi <= lo) lo = e->pub.addr, hi = e->pub.addr + e->t.L.size;
    return vk_mem_changed(lo, hi - lo, since, now);
}

// A mirrored texture is uploaded on first use and again when its storage pages carry a newer write generation.
void make_fresh(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint64_t size = e->t.L.size;
    if (!e->uploaded) {
        vk_mem_changed(e->pub.addr, size, 0, &e->wgen);   // resets the write bits before the storage is read
        upload(e);
        return;
    }
    uint32_t now;
    int ch = vk_mem_changed(e->pub.addr, size, e->wgen, &now);
    e->wgen = now;
    e->last_ch = ch;
    if (ch || e->up_pending) upload(e);   // 1 written, -1 not tracked: both upload again; a deferred one too
}

// make_fresh for draw bindings: the page check answers from this submission's write-watch snapshot (vk_memory.c
// vk_mem_changed_snap) instead of a system call per texture. A write the snapshot of the texture's region missed (made
// after it, during this replay) keeps its write-watch bit and is caught by the next submission's snapshot.
void make_fresh_snap(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint32_t now;
    n_checks++;
    int ch = vk_mem_changed_snap(e->pub.addr, e->t.L.size, e->uploaded ? e->wgen : 0, submit_epoch, &now);
    e->wgen = now;
    if (!e->uploaded) { upload(e); return; }
    e->last_ch = ch;
    if (ch || e->up_pending) upload(e);
}

