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
#include "platform/platform.h"

// ---------------------------------------------------------------- released entries, eviction and the owned list
// Out of memory with no released entry left: live textures that mirror CPU-visible storage (uploaded again from it
// on their next use), that nothing looked up in the last EVICT_IDLE submissions and the GPU did not write, go oldest
// use first. By the draw bindings' check alone (checked), a texture the replay used only as a copy source
// (vk_texture_get) looked idle while a part being recorded could name it: the likely cause of a view destroyed with
// that part not yet sent (MKW_GPU_GUARD, tsvp1 frame 1 477 and tsvp2, 08/11/2026, both after evictions). The race's downscaled render targets (bloom, the final composite's inputs) found the heap full of decoded
// ASTC textures and got no image: a black race (run-race4, 31/10/2026).
#define EVICT_IDLE 64
static uint64_t n_evicted, evicted_bytes;

uint32_t tex_evict_idle(void) {
    static int32_t v = -1;
    int32_t val = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (val < 0) {
        const char *e = cfg_get("MKW_TEX_EVICT_IDLE");
        val = e && *e && atoi(e) > 0 ? atoi(e) : EVICT_IDLE;
        __atomic_store_n(&v, val, __ATOMIC_RELAXED);
    }
    return (uint32_t)val;
}

VkResult evict_idle(VkCtx *c, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                    VmaAllocation *alloc) {
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t idle = tex_evict_idle();
    for (int round = 0; r == VK_ERROR_OUT_OF_DEVICE_MEMORY && round < 4; round++) {
        Tex *cand[64];
        int n = 0;
        for (uint32_t i = 0; i < by_key.cap && n < 64; i++) {
            Tex *e = by_key.vals ? by_key.vals[i] : NULL;
            if (e && e->alloc && e->pub.mirrored && !e->pub.gpu_owned && !e->gpu_dirty && !e->released && !e->pin &&
                (uint32_t)(submit_epoch - e->seen) > idle)
                cand[n++] = e;
        }
        if (!n) break;
        for (int i = 0; i < n; i++) {
            VmaAllocationInfo vi = {0};
            vmaGetAllocationInfo(c->vma, cand[i]->alloc, &vi);
            n_evicted++, evicted_bytes += vi.size;
            destroy_idle(cand[i]);
        }
        vk_mem_collect();
        r = vma_image(c, ci, ai, img, alloc);
    }
    if (n_evicted && !(n_evicted & (n_evicted - 1)))
        rt_log("vk: %llu idle mirrored textures evicted for room (%.0f MiB)", (unsigned long long)n_evicted,
               evicted_bytes / 1048576.0);
    return r;
}

// vk_staging out of memory: released entries, oldest first (as create_tex), then idle mirrored textures (evict_idle's
// rule), worth at least bytes, when the texture lock is free or this thread's (a thread holding it may wait for the
// upload lock the caller holds). 1 if any went. tt4 (09/11/2026) died on a staging buffer with 1 249 released entries
// holding 946 MiB: the idle ones alone were looked at.
// The trylock succeeds on the thread that holds the lock, the recursive case: upload() asking for staging trimmed the
// very entry it was uploading (looked up last more than idle submissions ago) and its caller went on with the freed
// Tex. make_fresh marks the entry seen before it uploads and pins it (tex_pin); a pinned entry is skipped here.
uint64_t vk_texture_trim(uint64_t bytes) {
    VkCtx *c = vk_ctx();
    if (!c || !plat_rmutex_trylock(&tex_lock)) return 0;
    uint64_t freed = 0;
    for (Tex *r; freed < bytes && (r = rel_oldest());) {
        VmaAllocationInfo vi = {0};
        if (r->alloc) vmaGetAllocationInfo(c->vma, r->alloc, &vi);
        freed += vi.size;
        destroy_idle(r);
    }
    uint32_t idle = tex_evict_idle();
    for (uint32_t i = 0; i < by_key.cap && freed < bytes; i++) {
        Tex *e = by_key.vals ? by_key.vals[i] : NULL;
        if (!e || !e->alloc || !e->pub.mirrored || e->pub.gpu_owned || e->gpu_dirty || e->released || e->pin ||
            (uint32_t)(submit_epoch - e->seen) <= idle)
            continue;
        VmaAllocationInfo vi = {0};
        vmaGetAllocationInfo(c->vma, e->alloc, &vi);
        freed += vi.size, n_evicted++, evicted_bytes += vi.size;
        destroy_idle(e);
        i--;   // the table shifted an entry into this slot
    }
    unlock();
    return freed;
}

static uint64_t released_bytes;

static uint64_t tex_alloc_bytes(Tex *e) {
    if (!e->alloc) return 0;
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    VmaAllocationInfo vi = {0};
    vmaGetAllocationInfo(c->vma, e->alloc, &vi);
    return vi.size;
}

uint64_t vk_texture_trim_released(uint64_t keep_bytes) {
    VkCtx *c = vk_ctx();
    if (!c || !plat_rmutex_trylock(&tex_lock)) return 0;
    uint64_t before = released_bytes;
    for (Tex *r; released_bytes > keep_bytes && (r = rel_oldest());)
        destroy_idle(r);
    uint64_t freed = before > released_bytes ? before - released_bytes : 0;
    unlock();
    return freed;
}

int owned_lower(uint64_t a) {
    int lo = 0, hi = nowned;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (owned[m]->pub.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

void own_add(Tex *e) {
    if (e->listed) return;
    plat_mutex_lock(&owned_lock);
    if (nowned == cowned) {
        cowned = cowned ? 2 * cowned : 256;
        if (!(owned = realloc(owned, sizeof *owned * (size_t)cowned))) rt_fatal("vk: out of memory for the owned-texture list");
    }
    int i = owned_lower(e->pub.addr);
    memmove(owned + i + 1, owned + i, sizeof *owned * (size_t)(nowned - i));
    owned[i] = e;
    __atomic_store_n(&nowned, nowned + 1, __ATOMIC_RELEASE);
    if (e->t.L.size > max_owned) max_owned = e->t.L.size;
    e->listed = 1;
    plat_mutex_unlock(&owned_lock);
}

static void own_remove(Tex *e) {
    if (!e->listed) return;
    plat_mutex_lock(&owned_lock);
    for (int i = owned_lower(e->pub.addr); i < nowned && owned[i]->pub.addr == e->pub.addr; i++)
        if (owned[i] == e) {
            memmove(owned + i, owned + i + 1, sizeof *owned * (size_t)(nowned - i - 1));
            __atomic_store_n(&nowned, nowned - 1, __ATOMIC_RELEASE);
            break;
        }
    e->listed = 0;
    plat_mutex_unlock(&owned_lock);
}

void rel_append(Tex *e) {
    e->rel_next = NULL;
    e->rel_prev = rel_tail;
    if (rel_tail) rel_tail->rel_next = e;
    else rel_head = e;
    rel_tail = e;
    released_bytes += tex_alloc_bytes(e);
}

// Released entries are never the one a caller is uploading (taking one back clears released), but the sweeps that walk
// them run inside staging that ran short too: a pinned one stays, as in vk_texture_trim.
Tex *rel_oldest(void) {
    Tex *r = rel_head;
    while (r && r->pin) r = r->rel_next;
    return r;
}

void rel_unlink(Tex *e) {
    uint64_t b = tex_alloc_bytes(e);
    released_bytes = released_bytes > b ? released_bytes - b : 0;
    if (e->rel_prev) e->rel_prev->rel_next = e->rel_next;
    else rel_head = e->rel_next;
    if (e->rel_next) e->rel_next->rel_prev = e->rel_prev;
    else rel_tail = e->rel_prev;
    e->rel_prev = e->rel_next = NULL;
}

void destroy_tex(Tex *e) {
    own_remove(e);
    tex_gen_bump(e->key.addr);
    nvn_hm_del(&by_key, &e->key);
    Tex *head = nvn_hm_get(&by_addr, &e->key.addr);
    if (head == e) {
        if (e->next_at) nvn_hm_put(&by_addr, &e->key.addr, e->next_at);
        else nvn_hm_del(&by_addr, &e->key.addr);
    } else {
        for (Tex *p = head; p; p = p->next_at)
            if (p->next_at == e) { p->next_at = e->next_at; break; }
    }
    if (e->released) nreleased--, rel_unlink(e);
    vk_mem_defer_use(e->use);   // MKW_GPU_GUARD: the image and its views
    vk_exec_image_gone(e->pub.image);
    // A sparse image also waits for its binds: they are queue operations the parts sent do not cover (vk_sparse.c).
    uint64_t binds = e->sparse ? vk_sparse_destroy(e->pub.image) : 0, prev = 0;
    if (binds) prev = vk_mem_defer_fence(0), vk_mem_defer_fence(prev > binds ? prev : binds);
    vk_defer_image(e->pub.image, e->alloc);
    if (binds) vk_mem_defer_fence(prev);
    vk_mem_defer_use(0);
    free(e);
}

// A released entry is not named by the submission being recorded (taking one back clears released), nor an idle one:
// they go once the parts already sent are done (vk_mem_defer_idle).
// Not when the submission being recorded looked it up (seen): a released entry replaced at a new descriptor of its
// address (entry_of) was still named by the part being recorded, and its view went with the parts already sent only
// (MKW_GPU_GUARD stack, gstack1 08/11/2026). Then it waits for that submission (the replay records one at a time).
void destroy_idle(Tex *e) {
    if (e->seen == submit_epoch) {
        vk_mem_defer_fence(vk_sync_issued() + 1);
        destroy_tex(e);
        vk_mem_defer_fence(0);
        return;
    }
    vk_mem_defer_idle(1);
    destroy_tex(e);
    vk_mem_defer_idle(0);
}

// Released entries with another descriptor at the same address are replaced; past RELEASED_MAX released entries the
// oldest one goes at each release (it comes back on demand, a mirrored one re-uploaded from its storage). TotK
// finalizes and initializes transient textures every frame, so a released entry is mostly taken back within frames.
// Dropping all of them at once when the count passed 4096 destroyed thousands of images in one frame: 333-422 ms
// frames every 40-50 s on a GTX 1050 Ti, then the textures still in use were created and uploaded again.
#define RELEASED_MAX       2048
#define RELEASED_MAX_BYTES (256ull << 20)
void sweep_released(void) {
    for (Tex *r; (nreleased > RELEASED_MAX || released_bytes > RELEASED_MAX_BYTES) && (r = rel_oldest());)
        destroy_idle(r);
}

