// Measure PS5 page-table consumption for a large sparse image, sparse block binds,
// and 2 GiB of direct memory mapped in 64 KiB vs 2 MiB units.
#include <limits.h>
#include <ps5platform/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "platform.h"   // PS5_VulkanTemplate's title logger

int sceKernelGetPageTableStats(int *cpu_total, int *cpu_free, int *gpu_total, int *gpu_free);
// Exported by PS5_PayloadSDK sce_stubs/libkernel.c. Seven arguments: the memory type comes third, before the protection
// (shadPS4 src/core/libraries/kernel/memory.cpp, sceKernelMapDirectMemory2; signature only). A six-argument declaration
// shifts every argument after the length and maps a wrong physical offset.
int32_t sceKernelMapDirectMemory2(void **address, size_t length, int type, int protection, int flags,
                                  int64_t direct_start, size_t alignment);

// PRT apertures: in PayloadSDK sce_stubs/libkernel.c (names, NIDs BohYr-F7-is / L0v2Go5jOuM). Signatures agree in shadPS4
// src/core/libraries/kernel/memory.cpp (b97bafa3; three apertures on the PS4) and AnyPS5 core/libs/prx/libkernel/include/
// Api.hpp; both GPL, signatures only. What an aperture does on the PS5's GPU is what the experiment below asks.
int32_t sceKernelSetPrtAperture(int id, void *address, size_t size);
int32_t sceKernelGetPrtAperture(int id, void **address, size_t *size);
// sceKernelQueryMemoryProtection comes from ps5platform/kernel.h (PS5_PayloadSDK; AnyPS5 agrees on the layout).
// The module loader: AnyPS5 libkernel/include/Api.hpp and PS5_Vulkan src/diagnostics.cpp agree.
int32_t sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, const void *option,
                                 int *result);
// The module list and a module's name: shadPS4 src/core/libraries/kernel/process.cpp and src/core/module.h, AnyPS5
// core/libs/SceTypes.hpp (ModuleInfo, 0x160 bytes: the size first, the name at +8) agree.
int32_t sceKernelGetModuleList(int *handles, uint64_t count, uint64_t *out_count);
int32_t sceKernelGetModuleInfo(int handle, void *info);
// libSceAmpr through a linked import: on this console a title's sceKernelDlsym finds nothing in any form (by name, by NID
// or as NID#library#module; PS5_Vulkan docs/HARDWARE_FINDINGS.md, 2026-09-18 and 2026-09-27; probe run 7: ESRCH for
// the name), while the loader binds a title's own imports at start. tools/ps5/ampr_stub.c names it; build_smoke.py links
// that stub as libSceAmpr.prx. A module the loader does not load for a title leaves the import null (PS5_Vulkan, the
// GNM driver), so the probe reads the GOT slot (&fn, -fPIC) before calling.
// sceAmprAmmGetVirtualAddressRanges (NID wkQR9+xTFKY) takes four out-pointers: AnyPS5 libSceAmpr/include/Api.hpp names
// them (start, end, multimapStart, multimapEnd); prosper (hle_kernel_mem.cpp, from a retail title's SDK wrapper and a
// live argument log) agrees on four 64-bit slots and calls the meaning of the last two unknown. They point at consecutive
// slots of one zeroed array here, so a struct-pointer reading of the call would fill the same slots.
int32_t sceAmprAmmGetVirtualAddressRanges(uint64_t *start, uint64_t *end, uint64_t *multimap_start,
                                          uint64_t *multimap_end);

// Direct-memory types from OpenOrbis / PS4-PS5 libkernel conventions:
// SCE_KERNEL_WB_ONION = 0x0, SCE_KERNEL_WC_GARLIC = 0x3, SCE_KERNEL_WB_GARLIC = 0xA, PS5_KERNEL_DIRECT_TYPE_CPU = 12.
enum {
    PT_MEM_WB_ONION = 0,
    PT_MEM_WC_GARLIC = 3,
    PT_MEM_WB_GARLIC = 10,
};

enum {
    PT_UNIT_64K = 65536u,
    PT_UNIT_2M = 2u << 20,
    PT_SPAN_2G = 2ull << 30,
    PT_SPARSE_BLOCKS = 3072u,
    PT_POOL_BLOCKS = 1024u,
};

static int pt_cpu_used(void) {
    int cpu_tot = 0, cpu_free = 0, gpu_tot = 0, gpu_free = 0;
    if (sceKernelGetPageTableStats(&cpu_tot, &cpu_free, &gpu_tot, &gpu_free) != 0) return -1;
    return cpu_tot - cpu_free;
}

static int pt_gpu_used(void) {
    int cpu_tot = 0, cpu_free = 0, gpu_tot = 0, gpu_free = 0;
    if (sceKernelGetPageTableStats(&cpu_tot, &cpu_free, &gpu_tot, &gpu_free) != 0) return -1;
    return gpu_tot - gpu_free;
}

static void log_pt(const char *step) {
    int cpu_tot = 0, cpu_free = 0, gpu_tot = 0, gpu_free = 0;
    int pt_rc = sceKernelGetPageTableStats(&cpu_tot, &cpu_free, &gpu_tot, &gpu_free);
    int64_t phys = 0;
    size_t avail = 0;
    int av_rc = sceKernelAvailableDirectMemorySize(0, INT64_MAX, PT_UNIT_64K, &phys, &avail);
    say("PS5 page-table %s: pt_rc=%d cpu=%d/%d (used=%d) gpu=%d/%d (used=%d) dmem_avail=%zu MiB (rc=%#x)",
        step, pt_rc, cpu_free, cpu_tot, cpu_tot - cpu_free, gpu_free, gpu_tot, gpu_tot - gpu_free,
        av_rc == 0 ? (avail >> 20) : 0, (unsigned)av_rc);
}

static uint32_t find_mem_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

static bool probe_unbound_readback(VkPhysicalDevice phys, VkDevice dev, VkQueue queue,
                                   uint32_t queue_family, VkImage img) {
    enum { READ_BYTES = 4096, BUF_BYTES = 65536 };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUF_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t mtype = find_mem_type(
        phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (mtype == UINT32_MAX ||
        vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mtype,
        }, NULL, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buf, NULL);
        return false;
    }
    vkBindBufferMemory(dev, buf, mem, 0);

    void *mapped = NULL;
    if (vkMapMemory(dev, mem, 0, BUF_BYTES, 0, &mapped) != VK_SUCCESS || !mapped) {
        vkFreeMemory(dev, mem, NULL);
        vkDestroyBuffer(dev, buf, NULL);
        return false;
    }
    memset(mapped, 0xA5, READ_BYTES);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family,
    };
    if (vkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS) {
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
        vkDestroyBuffer(dev, buf, NULL);
        return false;
    }
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(dev, pool, NULL);
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
        vkDestroyBuffer(dev, buf, NULL);
        return false;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier imb = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 67, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &imb);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 67, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {64, 64, 1},
    };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VkBufferMemoryBarrier bmb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf,
        .offset = 0,
        .size = READ_BYTES,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bmb, 0, NULL);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VkResult sr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (sr == VK_SUCCESS) sr = vkQueueWaitIdle(queue);

    unsigned zeros = 0;
    int first_nz = -1;
    const uint8_t *p = (const uint8_t *)mapped;
    for (unsigned i = 0; i < READ_BYTES; i++) {
        if (p[i] == 0) zeros++;
        else if (first_nz < 0) first_nz = (int)i;
    }
    bool ok = (sr == VK_SUCCESS && zeros == READ_BYTES);
    say("PS5 page-table unbound-readback: submit=%d zeros=%u/%u first_nz=%d ok=%d",
        (int)sr, zeros, (unsigned)READ_BYTES, first_nz, ok ? 1 : 0);

    vkDestroyCommandPool(dev, pool, NULL);
    vkUnmapMemory(dev, mem);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    return ok;
}

static bool probe_sparse_image(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t queue_family) {
    int cpu_before = pt_cpu_used();
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                 VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_BC6H_UFLOAT_BLOCK,
        .extent = {8192, 8192, 1},
        .mipLevels = 12,
        .arrayLayers = 68,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(dev, &ci, NULL, &img);
    say("PS5 page-table sparse create (8192x8192x68 BC6H 12 mips): VkResult=%d", (int)r);
    log_pt("after-sparse-create");
    if (r != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t nr = 1;
    VkSparseImageMemoryRequirements sr;
    vkGetImageSparseMemoryRequirements(dev, img, &nr, &sr);
    uint32_t gw = sr.formatProperties.imageGranularity.width;
    uint32_t gh = sr.formatProperties.imageGranularity.height;
    uint64_t slot_sz = mr.alignment ? mr.alignment : PT_UNIT_64K;
    uint32_t first_tail = nr ? sr.imageMipTailFirstLod : ci.mipLevels;
    say("PS5 page-table sparse reqs: size=%llu align=%llu granularity=%ux%u tail_lod=%u tail_size=%llu",
        (unsigned long long)mr.size, (unsigned long long)slot_sz, gw, gh, first_tail,
        (unsigned long long)(nr ? sr.imageMipTailSize : 0));

    uint32_t mtype = find_mem_type(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        vkDestroyImage(dev, img, NULL);
        return false;
    }

    enum { NPOOLS = 3 };
    VkDeviceMemory pools[NPOOLS] = {VK_NULL_HANDLE};
    for (uint32_t p = 0; p < NPOOLS; p++) {
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = (VkDeviceSize)PT_POOL_BLOCKS * slot_sz,
            .memoryTypeIndex = mtype,
        };
        if (vkAllocateMemory(dev, &ai, NULL, &pools[p]) != VK_SUCCESS) {
            say("PS5 page-table sparse pool %u alloc failed", p);
            for (uint32_t k = 0; k < p; k++) vkFreeMemory(dev, pools[k], NULL);
            vkDestroyImage(dev, img, NULL);
            return false;
        }
    }

    VkSparseImageMemoryBind *binds = calloc(PT_SPARSE_BLOCKS, sizeof *binds);
    if (!binds) {
        for (uint32_t p = 0; p < NPOOLS; p++) vkFreeMemory(dev, pools[p], NULL);
        vkDestroyImage(dev, img, NULL);
        return false;
    }

    uint32_t nb = 0;
    for (int l = (int)first_tail - 1; l >= 0 && nb < PT_SPARSE_BLOCKS; l--) {
        uint32_t lw = ci.extent.width >> l ? ci.extent.width >> l : 1;
        uint32_t lh = ci.extent.height >> l ? ci.extent.height >> l : 1;
        uint32_t bx = (lw + gw - 1) / gw;
        uint32_t by = (lh + gh - 1) / gh;
        for (uint32_t layer = 0; layer < ci.arrayLayers && nb < PT_SPARSE_BLOCKS; layer++) {
            for (uint32_t y = 0; y < by && nb < PT_SPARSE_BLOCKS; y++) {
                for (uint32_t x = 0; x < bx && nb < PT_SPARSE_BLOCKS; x++) {
                    uint32_t ox = x * gw, oy = y * gh;
                    uint32_t ew = ox + gw <= lw ? gw : lw - ox;
                    uint32_t eh = oy + gh <= lh ? gh : lh - oy;
                    uint32_t pool = nb / PT_POOL_BLOCKS;
                    uint32_t idx = nb % PT_POOL_BLOCKS;
                    binds[nb++] = (VkSparseImageMemoryBind){
                        .subresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, layer},
                        .offset = {(int32_t)ox, (int32_t)oy, 0},
                        .extent = {ew, eh, 1},
                        .memory = pools[pool],
                        .memoryOffset = (VkDeviceSize)idx * slot_sz,
                    };
                }
            }
        }
    }

      VkSparseImageMemoryBindInfo ib = {.image = img, .bindCount = nb, .pBinds = binds};
      VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .imageBindCount = 1, .pImageBinds = &ib};
      r = vkQueueBindSparse(queue, 1, &bi, VK_NULL_HANDLE);
      if (r == VK_SUCCESS) vkQueueWaitIdle(queue);
      say("PS5 page-table sparse bind (%u blocks): VkResult=%d", nb, (int)r);
      log_pt("after-sparse-bind");
      int cpu_after = pt_cpu_used();
      int cpu_delta = (cpu_before >= 0 && cpu_after >= 0) ? (cpu_after - cpu_before) : -1;
      bool ok_readback = (r == VK_SUCCESS) && probe_unbound_readback(phys, dev, queue, queue_family, img);
      bool ok_cpu = (cpu_delta >= 0 && cpu_delta < 100);
      say("PS5 page-table sparse cpu_delta=%d ok_cpu=%d ok_readback=%d",
          cpu_delta, ok_cpu ? 1 : 0, ok_readback ? 1 : 0);

      free(binds);
      vkDestroyImage(dev, img, NULL);
      for (uint32_t p = 0; p < NPOOLS; p++) vkFreeMemory(dev, pools[p], NULL);
      vkDeviceWaitIdle(dev);
      log_pt("after-sparse-destroy");
      return r == VK_SUCCESS && ok_cpu && ok_readback;
  }

static bool probe_gpu_only_1g(VkPhysicalDevice phys, VkDevice dev) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mtype = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) ==
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            mtype = i;
            break;
        }
    }
    if (mtype == UINT32_MAX) {
        say("PS5 page-table 1g-gpu-only: no non-host-visible DEVICE_LOCAL type");
        return false;
    }
    int cpu_before = pt_cpu_used();
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 1ull << 30,
        .memoryTypeIndex = mtype,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem);
    log_pt("after-1g-gpu-only-alloc");
    int cpu_after = pt_cpu_used();
    int cpu_delta = (cpu_before >= 0 && cpu_after >= 0) ? (cpu_after - cpu_before) : -1;
    if (r == VK_SUCCESS) vkFreeMemory(dev, mem, NULL);
    log_pt("after-1g-gpu-only-free");
    bool ok = (r == VK_SUCCESS && cpu_delta == 0);
    say("PS5 page-table 1g-gpu-only: mtype=%u VkResult=%d cpu_delta=%d ok=%d",
        mtype, (int)r, cpu_delta, ok ? 1 : 0);
    return ok;
}

static bool probe_direct_granularity(size_t unit, const char *label) {
    size_t count = PT_SPAN_2G / unit;
    void *va = NULL;
    int rc = sceKernelReserveVirtualRange(&va, PT_SPAN_2G, 0, unit);
    say("PS5 page-table %s reserve 2 GiB: va=%p rc=%#x", label, va, (unsigned)rc);
    if (rc) return false;
    char tag[48];
    snprintf(tag, sizeof tag, "after-%s-reserve", label);
    log_pt(tag);

    int64_t *phys = malloc(count * sizeof *phys);
    if (!phys) {
        sceKernelMunmap(va, PT_SPAN_2G);
        return false;
    }
    for (size_t i = 0; i < count; i++) phys[i] = -1;

    int64_t dmem_size = sceKernelGetDirectMemorySize();
    int prot = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    size_t mapped = 0;
    for (size_t i = 0; i < count; i++) {
        rc = sceKernelAllocateDirectMemory(0, dmem_size, unit, unit, PS5_KERNEL_DIRECT_TYPE_CPU, &phys[i]);
        if (rc) {
            say("PS5 page-table %s alloc failed at unit %zu/%zu: rc=%#x", label, i, count, (unsigned)rc);
            break;
        }
        void *at = (void *)((uintptr_t)va + i * unit);
        void *got = at;
        rc = sceKernelMapDirectMemory(&got, unit, prot, PS5_KERNEL_MAP_FIXED, phys[i], unit);
        if (rc || got != at) {
            say("PS5 page-table %s map failed at unit %zu/%zu: rc=%#x got=%p", label, i, count, (unsigned)rc, got);
            sceKernelReleaseDirectMemory(phys[i], unit);
            phys[i] = -1;
            break;
        }
        mapped++;
    }
    say("PS5 page-table %s mapped %zu/%zu units (%zu MiB)", label, mapped, count, (mapped * unit) >> 20);
    snprintf(tag, sizeof tag, "after-%s-map", label);
    log_pt(tag);

    sceKernelMunmap(va, PT_SPAN_2G);
    for (size_t i = 0; i < count; i++)
        if (phys[i] >= 0) sceKernelReleaseDirectMemory(phys[i], unit);
    free(phys);
    snprintf(tag, sizeof tag, "after-%s-unmap", label);
    log_pt(tag);
    return mapped == count;
}

static bool probe_alias_2m(bool use_v2, int mem_type, int prot, const char *label, int *out_cpu_delta) {
    size_t count = PT_SPAN_2G / PT_UNIT_2M;
    int64_t dmem_size = sceKernelGetDirectMemorySize();
    int64_t phys = -1;
    int rc = sceKernelAllocateDirectMemory(0, dmem_size, PT_UNIT_2M, PT_UNIT_2M, mem_type, &phys);
    if (rc) {
        say("PS5 page-table %s: alloc 2 MiB (type=%d) failed rc=%#x", label, mem_type, (unsigned)rc);
        if (out_cpu_delta) *out_cpu_delta = -1;
        return false;
    }
    void *va = NULL;
    rc = sceKernelReserveVirtualRange(&va, PT_SPAN_2G, 0, PT_UNIT_2M);
    if (rc) {
        say("PS5 page-table %s: reserve 2 GiB failed rc=%#x", label, (unsigned)rc);
        sceKernelReleaseDirectMemory(phys, PT_UNIT_2M);
        if (out_cpu_delta) *out_cpu_delta = -1;
        return false;
    }
    int cpu0 = pt_cpu_used(), gpu0 = pt_gpu_used();
    size_t mapped = 0;
    for (size_t i = 0; i < count; i++) {
        void *at = (void *)((uintptr_t)va + i * PT_UNIT_2M);
        void *got = at;
        rc = use_v2 ? sceKernelMapDirectMemory2(&got, PT_UNIT_2M, mem_type, prot, PS5_KERNEL_MAP_FIXED, phys, PT_UNIT_2M)
                    : sceKernelMapDirectMemory(&got, PT_UNIT_2M, prot, PS5_KERNEL_MAP_FIXED, phys, PT_UNIT_2M);
        if (rc || got != at) {
            say("PS5 page-table %s: map failed at %zu/%zu rc=%#x got=%p", label, i, count, (unsigned)rc, got);
            break;
        }
        mapped++;
    }
    int cpu1 = pt_cpu_used(), gpu1 = pt_gpu_used();
    int cd = (cpu0 >= 0 && cpu1 >= 0) ? (cpu1 - cpu0) : -1;
    int gd = (gpu0 >= 0 && gpu1 >= 0) ? (gpu1 - gpu0) : -1;
    say("PS5 page-table %s: mapped=%zu/%zu type=%d prot=%#x v2=%d cpu_delta=%d gpu_delta=%d",
        label, mapped, count, mem_type, (unsigned)prot, use_v2 ? 1 : 0, cd, gd);
    if (out_cpu_delta) *out_cpu_delta = (mapped == count) ? cd : -1;
    sceKernelMunmap(va, PT_SPAN_2G);
    sceKernelReleaseDirectMemory(phys, PT_UNIT_2M);
    return mapped == count;
}

static bool probe_distinct_2m(bool use_v2, const char *label, int *out_cpu_delta) {
    size_t count = PT_SPAN_2G / PT_UNIT_2M;
    void *va = NULL;
    int rc = sceKernelReserveVirtualRange(&va, PT_SPAN_2G, 0, PT_UNIT_2M);
    if (rc) {
        say("PS5 page-table %s: reserve 2 GiB failed rc=%#x", label, (unsigned)rc);
        if (out_cpu_delta) *out_cpu_delta = -1;
        return false;
    }
    int64_t *phys = malloc(count * sizeof *phys);
    if (!phys) {
        sceKernelMunmap(va, PT_SPAN_2G);
        if (out_cpu_delta) *out_cpu_delta = -1;
        return false;
    }
    for (size_t i = 0; i < count; i++) phys[i] = -1;
    int64_t dmem_size = sceKernelGetDirectMemorySize();
    int prot = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    int cpu0 = pt_cpu_used(), gpu0 = pt_gpu_used();
    size_t mapped = 0;
    for (size_t i = 0; i < count; i++) {
        rc = sceKernelAllocateDirectMemory(0, dmem_size, PT_UNIT_2M, PT_UNIT_2M, PS5_KERNEL_DIRECT_TYPE_CPU, &phys[i]);
        if (rc) break;
        void *at = (void *)((uintptr_t)va + i * PT_UNIT_2M);
        void *got = at;
        rc = use_v2 ? sceKernelMapDirectMemory2(&got, PT_UNIT_2M, PS5_KERNEL_DIRECT_TYPE_CPU, prot, PS5_KERNEL_MAP_FIXED, phys[i], PT_UNIT_2M)
                    : sceKernelMapDirectMemory(&got, PT_UNIT_2M, prot, PS5_KERNEL_MAP_FIXED, phys[i], PT_UNIT_2M);
        if (rc || got != at) {
            sceKernelReleaseDirectMemory(phys[i], PT_UNIT_2M);
            phys[i] = -1;
            break;
        }
        mapped++;
    }
    int cpu1 = pt_cpu_used(), gpu1 = pt_gpu_used();
    int cd = (cpu0 >= 0 && cpu1 >= 0) ? (cpu1 - cpu0) : -1;
    int gd = (gpu0 >= 0 && gpu1 >= 0) ? (gpu1 - gpu0) : -1;
    say("PS5 page-table %s: mapped=%zu/%zu v2=%d cpu_delta=%d gpu_delta=%d",
        label, mapped, count, use_v2 ? 1 : 0, cd, gd);
    if (out_cpu_delta) *out_cpu_delta = (mapped == count) ? cd : -1;
    sceKernelMunmap(va, PT_SPAN_2G);
    for (size_t i = 0; i < count; i++)
        if (phys[i] >= 0) sceKernelReleaseDirectMemory(phys[i], PT_UNIT_2M);
    free(phys);
    return mapped == count;
}

// Compute shader: DstBuf(pc.dst).val = SrcBuf(pc.src).val (Vulkan 1.2, PhysicalStorageBuffer64).
static const uint32_t bda_read_spv[] = {
    0x07230203u, 0x00010500u, 0x0008000bu, 0x00000020u, 0x00000000u, 0x00020011u, 0x00000001u, 0x00020011u,
    0x0000000bu, 0x00020011u, 0x000014e3u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
    0x00000000u, 0x0003000eu, 0x000014e4u, 0x00000001u, 0x0006000fu, 0x00000005u, 0x00000004u, 0x6e69616du,
    0x00000000u, 0x00000009u, 0x00060010u, 0x00000004u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u,
    0x00030003u, 0x00000002u, 0x000001c2u, 0x00070004u, 0x455f4c47u, 0x625f5458u, 0x65666675u, 0x65725f72u,
    0x65726566u, 0x0065636eu, 0x000d0004u, 0x455f4c47u, 0x735f5458u, 0x65646168u, 0x78655f72u, 0x63696c70u,
    0x615f7469u, 0x68746972u, 0x6974656du, 0x79745f63u, 0x5f736570u, 0x36746e69u, 0x00000034u, 0x00040005u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x00030005u, 0x00000007u, 0x00004350u, 0x00040006u, 0x00000007u,
    0x00000000u, 0x00637273u, 0x00040006u, 0x00000007u, 0x00000001u, 0x00747364u, 0x00030005u, 0x00000009u,
    0x00006370u, 0x00040005u, 0x00000011u, 0x42747344u, 0x00006675u, 0x00040006u, 0x00000011u, 0x00000000u,
    0x006c6176u, 0x00040005u, 0x00000017u, 0x42637253u, 0x00006675u, 0x00040006u, 0x00000017u, 0x00000000u,
    0x006c6176u, 0x00030047u, 0x00000007u, 0x00000002u, 0x00050048u, 0x00000007u, 0x00000000u, 0x00000023u,
    0x00000000u, 0x00050048u, 0x00000007u, 0x00000001u, 0x00000023u, 0x00000008u, 0x00030047u, 0x00000011u,
    0x00000002u, 0x00040048u, 0x00000011u, 0x00000000u, 0x00000019u, 0x00050048u, 0x00000011u, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00030047u, 0x00000017u, 0x00000002u, 0x00040048u, 0x00000017u, 0x00000000u,
    0x00000018u, 0x00050048u, 0x00000017u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x0000001fu,
    0x0000000bu, 0x00000019u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00040015u,
    0x00000006u, 0x00000040u, 0x00000000u, 0x0004001eu, 0x00000007u, 0x00000006u, 0x00000006u, 0x00040020u,
    0x00000008u, 0x00000009u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000009u, 0x00040015u,
    0x0000000au, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000au, 0x0000000bu, 0x00000001u, 0x00040020u,
    0x0000000cu, 0x00000009u, 0x00000006u, 0x00030027u, 0x0000000fu, 0x000014e5u, 0x00040015u, 0x00000010u,
    0x00000020u, 0x00000000u, 0x0003001eu, 0x00000011u, 0x00000010u, 0x00040020u, 0x0000000fu, 0x000014e5u,
    0x00000011u, 0x0004002bu, 0x0000000au, 0x00000013u, 0x00000000u, 0x00030027u, 0x00000016u, 0x000014e5u,
    0x0003001eu, 0x00000017u, 0x00000010u, 0x00040020u, 0x00000016u, 0x000014e5u, 0x00000017u, 0x00040020u,
    0x00000019u, 0x000014e5u, 0x00000010u, 0x00040017u, 0x0000001du, 0x00000010u, 0x00000003u, 0x0004002bu,
    0x00000010u, 0x0000001eu, 0x00000001u, 0x0006002cu, 0x0000001du, 0x0000001fu, 0x0000001eu, 0x0000001eu,
    0x0000001eu, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x00050041u, 0x0000000cu, 0x0000000du, 0x00000009u, 0x0000000bu, 0x0004003du, 0x00000006u, 0x0000000eu,
    0x0000000du, 0x00040078u, 0x0000000fu, 0x00000012u, 0x0000000eu, 0x00050041u, 0x0000000cu, 0x00000014u,
    0x00000009u, 0x00000013u, 0x0004003du, 0x00000006u, 0x00000015u, 0x00000014u, 0x00040078u, 0x00000016u,
    0x00000018u, 0x00000015u, 0x00050041u, 0x00000019u, 0x0000001au, 0x00000018u, 0x00000013u, 0x0006003du,
    0x00000010u, 0x0000001bu, 0x0000001au, 0x00000002u, 0x00000004u, 0x00050041u, 0x00000019u, 0x0000001cu,
    0x00000012u, 0x00000013u, 0x0005003eu, 0x0000001cu, 0x0000001bu, 0x00000002u, 0x00000004u, 0x000100fdu,
    0x00010038u,
};

// Reads one word at read_va (or, when write_va != NULL, writes 0xdeadbeef to write_va and then reads mapped read_va)
// on the GPU and logs what came back.
static __attribute__((unused)) void probe_gpu_op(VkPhysicalDevice phys, uint32_t queue_family, const char *label,
                                                 void *read_va, void *write_va) {
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    if (!f12.bufferDeviceAddress) {
        say("PS5 page-table %s: bufferDeviceAddress unsupported", label);
        return;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = queue_family, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan12Features en12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .bufferDeviceAddress = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &en12,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = vkCreateDevice(phys, &dci, NULL, &dev);
    if (r != VK_SUCCESS) {
        say("PS5 page-table %s: vkCreateDevice failed (%d)", label, (int)r);
        return;
    }
    PFN_vkGetDeviceQueue p_get_q = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(dev, "vkGetDeviceQueue");
    PFN_vkCreateBuffer p_cbuf = (PFN_vkCreateBuffer)vkGetDeviceProcAddr(dev, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements p_bmr = (PFN_vkGetBufferMemoryRequirements)vkGetDeviceProcAddr(dev, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory p_alloc = (PFN_vkAllocateMemory)vkGetDeviceProcAddr(dev, "vkAllocateMemory");
    PFN_vkBindBufferMemory p_bind = (PFN_vkBindBufferMemory)vkGetDeviceProcAddr(dev, "vkBindBufferMemory");
    PFN_vkMapMemory p_map = (PFN_vkMapMemory)vkGetDeviceProcAddr(dev, "vkMapMemory");
    PFN_vkGetBufferDeviceAddress p_bda = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddress");
    PFN_vkCreateShaderModule p_csm = (PFN_vkCreateShaderModule)vkGetDeviceProcAddr(dev, "vkCreateShaderModule");
    PFN_vkCreatePipelineLayout p_cpl = (PFN_vkCreatePipelineLayout)vkGetDeviceProcAddr(dev, "vkCreatePipelineLayout");
    PFN_vkCreateComputePipelines p_ccp = (PFN_vkCreateComputePipelines)vkGetDeviceProcAddr(dev, "vkCreateComputePipelines");
    PFN_vkCreateCommandPool p_cpool = (PFN_vkCreateCommandPool)vkGetDeviceProcAddr(dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers p_acb = (PFN_vkAllocateCommandBuffers)vkGetDeviceProcAddr(dev, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer p_bcb = (PFN_vkBeginCommandBuffer)vkGetDeviceProcAddr(dev, "vkBeginCommandBuffer");
    PFN_vkCmdBindPipeline p_cbp = (PFN_vkCmdBindPipeline)vkGetDeviceProcAddr(dev, "vkCmdBindPipeline");
    PFN_vkCmdPushConstants p_cpc = (PFN_vkCmdPushConstants)vkGetDeviceProcAddr(dev, "vkCmdPushConstants");
    PFN_vkCmdDispatch p_disp = (PFN_vkCmdDispatch)vkGetDeviceProcAddr(dev, "vkCmdDispatch");
    PFN_vkEndCommandBuffer p_ecb = (PFN_vkEndCommandBuffer)vkGetDeviceProcAddr(dev, "vkEndCommandBuffer");
    PFN_vkCreateFence p_cf = (PFN_vkCreateFence)vkGetDeviceProcAddr(dev, "vkCreateFence");
    PFN_vkQueueSubmit p_sub = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(dev, "vkQueueSubmit");
    PFN_vkWaitForFences p_wait = (PFN_vkWaitForFences)vkGetDeviceProcAddr(dev, "vkWaitForFences");
    PFN_vkDestroyDevice p_dd = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");

    VkQueue q = VK_NULL_HANDLE;
    p_get_q(dev, queue_family, 0, &q);

    VkBuffer buf = VK_NULL_HANDLE;
    p_cbuf(dev, &(VkBufferCreateInfo){
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    }, NULL, &buf);
    VkMemoryRequirements mr;
    p_bmr(dev, buf, &mr);
    uint32_t mtype = find_mem_type(phys, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkMemoryAllocateFlagsInfo afi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    p_alloc(dev, &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &afi,
        .allocationSize = mr.size, .memoryTypeIndex = mtype,
    }, NULL, &mem);
    p_bind(dev, buf, mem, 0);
    uint32_t *dst = NULL;
    p_map(dev, mem, 0, 64, 0, (void **)&dst);
    dst[0] = 0xA5A5A5A5u;
    dst[1] = 0xdeadbeefu;
    uint64_t dst_addr = p_bda(dev, &(VkBufferDeviceAddressInfo){
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf});

    void *unmapped_va = read_va;
    if (!unmapped_va) {
        int rc = sceKernelReserveVirtualRange(&unmapped_va, PT_UNIT_2M, 0, PT_UNIT_2M);
        if (rc != 0 || !unmapped_va) {
            say("PS5 page-table %s: reserve failed rc=%#x", label, (unsigned)rc);
            p_dd(dev, NULL);
            return;
        }
    }

    VkShaderModule sm = VK_NULL_HANDLE;
    p_csm(dev, &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof bda_read_spv, .pCode = bda_read_spv,
    }, NULL, &sm);
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16};
    VkPipelineLayout pl = VK_NULL_HANDLE;
    p_cpl(dev, &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
    }, NULL, &pl);
    VkPipeline pipe = VK_NULL_HANDLE;
    p_ccp(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
        .layout = pl,
    }, NULL, &pipe);

    VkCommandPool pool = VK_NULL_HANDLE;
    p_cpool(dev, &(VkCommandPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = queue_family,
    }, NULL, &pool);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    p_acb(dev, &(VkCommandBufferAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    }, &cmd);
    p_bcb(cmd, &(VkCommandBufferBeginInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT});
    p_cbp(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    if (write_va) {
        uint64_t wpc[2] = {dst_addr + 4, (uint64_t)(uintptr_t)write_va};
        p_cpc(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, wpc);
        p_disp(cmd, 1, 1, 1);
        PFN_vkCmdPipelineBarrier p_bar = (PFN_vkCmdPipelineBarrier)vkGetDeviceProcAddr(dev, "vkCmdPipelineBarrier");
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        p_bar(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    uint64_t pc[2] = {(uint64_t)(uintptr_t)unmapped_va, dst_addr};
    p_cpc(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
    p_disp(cmd, 1, 1, 1);
    p_ecb(cmd);

    VkFence fence = VK_NULL_HANDLE;
    p_cf(dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence);
    VkResult sr = p_sub(q, 1, &(VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd,
    }, fence);
    VkResult wr = (sr == VK_SUCCESS) ? p_wait(dev, 1, &fence, VK_TRUE, 1000000000ull) : sr;
    say("PS5 page-table %s: va=%p submit=%d wait=%d dst=0x%08x (0xa5a5a5a5: the shader never wrote)",
        label, write_va ? write_va : unmapped_va, (int)sr, (int)wr, dst[0]);
    if (!read_va) sceKernelMunmap(unmapped_va, PT_UNIT_2M);
    p_dd(dev, NULL);
}

static __attribute__((unused)) void probe_gpu_read_at(VkPhysicalDevice phys, uint32_t queue_family, const char *label,
                                                      void *va) {
    probe_gpu_op(phys, queue_family, label, va, NULL);
}

// The modules this process holds (by sceKernelGetModuleList and sceKernelGetModuleInfo), and the one a handle names.
static void module_name(int handle, char *out, size_t n) {
    uint64_t info[0x400 / 8] = {0};   // ModuleInfo is 0x160 bytes; room for a longer one
    info[0] = 0x160;
    int rc = sceKernelGetModuleInfo(handle, info);
    if (rc != 0) snprintf(out, n, "(info rc=%#x)", (unsigned)rc);
    else snprintf(out, n, "%.60s", (const char *)&info[1]);
}

static void log_modules(void) {
    int handles[256] = {0};
    uint64_t count = 0;
    int rc = sceKernelGetModuleList(handles, 256, &count);
    if (rc != 0) {
        say("PS5 page-table modules: list rc=%#x", (unsigned)rc);
        return;
    }
    char line[1600];
    int k = snprintf(line, sizeof line, "PS5 page-table modules: %llu:", (unsigned long long)count);
    for (uint64_t i = 0; i < count && i < 256 && k < (int)sizeof line - 80; i++) {
        char name[64];
        module_name(handles[i], name, sizeof name);
        k += snprintf(line + k, sizeof line - (size_t)k, " %#x=%s", (unsigned)handles[i], name);
    }
    say("%s", line);
}

// Where the title's AMM (address memory manager) VA lives: the kernel prints "[Amm config] ... VA range size 512 GiB,
// MultiMap VA range size 0 GiB" for the title; libSceAmpr hands the range out. Read-only: no AMM command is written (the
// public sources disagree on sceAmprCommandBufferSetBuffer, which the map path needs; docs/research.md).
static void probe_amm_ranges(void) {
    log_modules();
    int status = 0;
    int handle = sceKernelLoadStartModule("libSceAmpr.sprx", 0, NULL, 0, NULL, &status);
    char name[64] = "";
    if (handle >= 0) module_name(handle, name, sizeof name);
    say("PS5 page-table amm-ranges: sceKernelLoadStartModule(libSceAmpr.sprx) handle=%#x status=%#x module=%s",
        (unsigned)handle, (unsigned)status, name);
    void *volatile bound = (void *)&sceAmprAmmGetVirtualAddressRanges;   // the GOT slot the loader filled, or 0
    if (!bound) {
        say("PS5 page-table amm-ranges: linked import not bound (libSceAmpr not loaded for this title)");
        return;
    }
    uint64_t r[8] = {0};
    int cpu0 = pt_cpu_used(), gpu0 = pt_gpu_used();
    int rc = sceAmprAmmGetVirtualAddressRanges(&r[0], &r[1], &r[2], &r[3]);
    say("PS5 page-table amm-ranges: import at %p rc=%#x range=[%#llx,%#llx) %llu GiB, multimap=[%#llx,%#llx) %llu GiB, "
        "slots 4-7 %#llx %#llx %#llx %#llx (should stay 0), cpu_delta=%d gpu_delta=%d", bound, (unsigned)rc,
        (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)(r[1] > r[0] ? (r[1] - r[0]) >> 30 : 0),
        (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)(r[3] > r[2] ? (r[3] - r[2]) >> 30 : 0),
        (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)r[6], (unsigned long long)r[7],
        pt_cpu_used() - cpu0, pt_gpu_used() - gpu0);
    for (int i = 0; i < 4; i += 2) {
        if (!r[i]) continue;
        void *start = NULL, *end = NULL;
        uint32_t prot = 0;
        int qrc = sceKernelQueryMemoryProtection((void *)(uintptr_t)r[i], &start, &end, &prot);
        say("PS5 page-table amm-ranges: %s start %#llx: query rc=%#x [%p,%p) prot=%#x", i ? "multimap" : "amm",
            (unsigned long long)r[i], (unsigned)qrc, start, end, prot);
    }
}

// PRT apertures. Run 7 (klog-probe-pt7): ids 0-2 read back (rc 0, first output 1, second 0), id 3 is EINVAL, and
// Set(0, 0x20b800000, 2 GiB) was EINVAL. shadPS4's sceKernelSetPrtAperture (src/core/libraries/kernel/memory.cpp,
// b97bafa3) checks only the id (0-2), a 4 KiB aligned address and that [address, address + size) lies inside the PS4's
// PRT area, 0x10_0000_0000 + 0xec_0000_0000; 0x20b800000 is below it. No public source gives the PS5's area. So every
// attempt here is inside that area, at an address aligned to the size, over VA reserved for it and then over the same VA
// unreserved; the first success stops the search.
#define PRT_AREA_START 0x1000000000ull
#define PRT_AREA_END (0x1000000000ull + 0xec00000000ull)
#define PRT_SENTINEL 0xa5a5a5a5a5a5a5a5ull

static void prt_get_all(const char *when) {
    for (int id = 0; id < 3; id++) {
        uint64_t out[2] = {PRT_SENTINEL, PRT_SENTINEL};   // which output the call writes, and with what
        int rc = sceKernelGetPrtAperture(id, (void **)&out[0], (size_t *)&out[1]);
        say("PS5 page-table prt-aperture: %s get id=%d rc=%#x out1=%#llx out2=%#llx (%#llx: not written)", when, id,
            (unsigned)rc, (unsigned long long)out[0], (unsigned long long)out[1], (unsigned long long)PRT_SENTINEL);
    }
}

// VA of `size` bytes aligned to `size` inside the free PRT area (>= 192 GiB, outside RADV's 256..512 GiB region
// and outside ps5-high144's 64..176 GiB guest regions). 0 when no hint landed.
static uint64_t prt_reserve(uint64_t size) {
    static const uint64_t hints[] = {0xa000000000ull, 0xc000000000ull, 0x3000000000ull};
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
        if (hints[i] % size != 0) continue;
        void *va = (void *)(uintptr_t)hints[i];
        // 2 MiB alignment with a size-aligned hint: console run 9 asked for alignment = size (4-128 GiB) and every
        // reservation failed, while run 8's hinted 64 KiB reservation landed exactly on its hint.
        int rc = sceKernelReserveVirtualRange(&va, size, 0, PT_UNIT_2M);
        uint64_t got = (uint64_t)(uintptr_t)va;
        say("PS5 page-table prt-aperture: reserve %llu GiB hint=%#llx rc=%#x got=%#llx", (unsigned long long)(size >> 30),
            (unsigned long long)hints[i], (unsigned)rc, (unsigned long long)got);
        if (rc == 0 && got >= PRT_AREA_START && got + size <= PRT_AREA_END && got % size == 0) return got;
        if (rc == 0) sceKernelMunmap(va, size);
    }
    return 0;
}

static void prt_clear(int id) {
    int rc = sceKernelSetPrtAperture(id, (void *)1, 0);
    say("PS5 page-table prt-aperture: clear id=%d rc=%#x", id, (unsigned)rc);
}

// Map one 64 KiB page at `page`, write `val` from CPU, read back via GPU, optionally unmap and re-read.
static void prt_check_page(VkPhysicalDevice phys, uint32_t queue_family, const char *label, uint64_t page) {
#ifdef MKW_PT_PRT_GPU_READ
    char tag[96];
    snprintf(tag, sizeof tag, "prt-gpu-read unmapped (%s)", label);
    probe_gpu_read_at(phys, queue_family, tag, (void *)(uintptr_t)page);

    int64_t pa = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), PT_UNIT_64K, PT_UNIT_64K,
                                           PS5_KERNEL_DIRECT_TYPE_CPU, &pa);
    int allocated = rc == 0, mapped = 0;
    void *at = (void *)(uintptr_t)page;
    int cpu0 = pt_cpu_used();
    if (rc == 0)
        rc = sceKernelMapDirectMemory(&at, PT_UNIT_64K,
                                      PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE |
                                      PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE,
                                      PS5_KERNEL_MAP_FIXED, pa, PT_UNIT_64K);
    mapped = allocated && rc == 0 && at == (void *)(uintptr_t)page;
    say("PS5 page-table prt-gpu-read mapped (%s): page=%#llx rc=%#x cpu_delta=%d",
        label, (unsigned long long)page, (unsigned)rc, pt_cpu_used() - cpu0);
    if (mapped) {
        *(volatile uint32_t *)at = 0x5eed1234u;
        snprintf(tag, sizeof tag, "prt-gpu-read mapped (%s expect 0x5eed1234)", label);
        probe_gpu_read_at(phys, queue_family, tag, at);
        sceKernelMunmap(at, PT_UNIT_64K);
        snprintf(tag, sizeof tag, "prt-gpu-read reunbound (%s expect 0)", label);
        probe_gpu_read_at(phys, queue_family, tag, (void *)(uintptr_t)page);
    }
    if (allocated) sceKernelReleaseDirectMemory(pa, PT_UNIT_64K);
#else
    (void)phys, (void)queue_family, (void)label, (void)page;
#endif
}

// GPU write to unmapped VA inside the aperture, then read back a mapped neighbour.
static void prt_check_unmapped_write(VkPhysicalDevice phys, uint32_t queue_family, uint64_t va) {
#ifdef MKW_PT_PRT_GPU_READ
    uint64_t neighbour = va + PT_UNIT_64K;
    int64_t pa = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), PT_UNIT_64K, PT_UNIT_64K,
                                           PS5_KERNEL_DIRECT_TYPE_CPU, &pa);
    void *at = (void *)(uintptr_t)neighbour;
    int mapped = 0;
    if (rc == 0) {
        rc = sceKernelMapDirectMemory(&at, PT_UNIT_64K,
                                      PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE |
                                      PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE,
                                      PS5_KERNEL_MAP_FIXED, pa, PT_UNIT_64K);
        mapped = (rc == 0 && at == (void *)(uintptr_t)neighbour);
    }
    if (mapped) {
        *(volatile uint32_t *)at = 0xcafebabeu;
        probe_gpu_op(phys, queue_family, "prt-gpu-write unmapped + read neighbour (expect 0xcafebabe)",
                     at, (void *)(uintptr_t)va);
        sceKernelMunmap(at, PT_UNIT_64K);
    } else {
        say("PS5 page-table prt-gpu-write: neighbour map failed rc=%#x", (unsigned)rc);
    }
    if (pa > 0 || rc == 0) sceKernelReleaseDirectMemory(pa, PT_UNIT_64K);
#else
    (void)phys, (void)queue_family, (void)va;
#endif
}

// Map 2 GiB of distinct 64 KiB pages scattered across the aperture (4 x 512 MiB chunks) and measure CPU/GPU units.
static void prt_scatter_2g(uint64_t va, uint64_t size) {
    enum { CHUNKS = 4, CHUNK_BYTES = 512ull << 20, PAGES_PER_CHUNK = CHUNK_BYTES / PT_UNIT_64K, TOTAL = CHUNKS * PAGES_PER_CHUNK };
    uint64_t bases[CHUNKS];
    bases[0] = va;
    bases[1] = (va + size / 3) & ~(uint64_t)(PT_UNIT_2M - 1);
    bases[2] = (va + (size * 2) / 3) & ~(uint64_t)(PT_UNIT_2M - 1);
    bases[3] = (va + size - CHUNK_BYTES) & ~(uint64_t)(PT_UNIT_2M - 1);

    int64_t *phys = malloc(TOTAL * sizeof *phys);
    if (!phys) {
        say("PS5 page-table prt-scatter-2g: OOM");
        return;
    }
    for (size_t i = 0; i < TOTAL; i++) phys[i] = -1;

    int64_t dmem = sceKernelGetDirectMemorySize();
    int prot = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE;
    int cpu0 = pt_cpu_used(), gpu0 = pt_gpu_used();
    size_t mapped = 0;
    for (int c = 0; c < CHUNKS; c++) {
        for (size_t p = 0; p < PAGES_PER_CHUNK; p++) {
            size_t idx = (size_t)c * PAGES_PER_CHUNK + p;
            int rc = sceKernelAllocateDirectMemory(0, dmem, PT_UNIT_64K, PT_UNIT_64K, PS5_KERNEL_DIRECT_TYPE_CPU, &phys[idx]);
            if (rc) goto done;
            void *at = (void *)(uintptr_t)(bases[c] + p * PT_UNIT_64K);
            void *got = at;
            rc = sceKernelMapDirectMemory(&got, PT_UNIT_64K, prot, PS5_KERNEL_MAP_FIXED, phys[idx], PT_UNIT_64K);
            if (rc || got != at) {
                sceKernelReleaseDirectMemory(phys[idx], PT_UNIT_64K);
                phys[idx] = -1;
                goto done;
            }
            mapped++;
        }
    }
done:
    say("PS5 page-table prt-scatter-2g: mapped=%zu/%zu cpu_delta=%d gpu_delta=%d",
        mapped, (size_t)TOTAL, pt_cpu_used() - cpu0, pt_gpu_used() - gpu0);
    for (int c = 0; c < CHUNKS; c++) sceKernelMunmap((void *)(uintptr_t)bases[c], CHUNK_BYTES);
    for (size_t i = 0; i < TOTAL; i++)
        if (phys[i] >= 0) sceKernelReleaseDirectMemory(phys[i], PT_UNIT_64K);
    free(phys);
    say("PS5 page-table prt-scatter-2g after unmap: cpu_delta=%d gpu_delta=%d",
        pt_cpu_used() - cpu0, pt_gpu_used() - gpu0);
}

static void probe_prt_aperture(VkPhysicalDevice phys, uint32_t queue_family) {
    static const uint64_t sizes[] = {4ull << 30, 8ull << 30, 16ull << 30, 32ull << 30, 64ull << 30, 128ull << 30};
    prt_get_all("before");

    void *cur_va = NULL;
    size_t cur_sz = 0;
    bool radv_holds_ap0 = (sceKernelGetPrtAperture(0, &cur_va, &cur_sz) == 0 && cur_sz != 0);
    if (radv_holds_ap0) {
        say("PS5 page-table prt-aperture: id=0 already held (va=%p size=%llu GiB); skipping 128 GiB sweep",
            cur_va, (unsigned long long)(cur_sz >> 30));
    } else {
        uint64_t best_size = 0, best_va = 0;
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            uint64_t size = sizes[si];
            uint64_t va = prt_reserve(size);
            if (!va) {
                say("PS5 page-table prt-aperture: size=%llu GiB no free aligned VA", (unsigned long long)(size >> 30));
                continue;
            }
            int cpu0 = pt_cpu_used(), gpu0 = pt_gpu_used();
            int rc = sceKernelSetPrtAperture(0, (void *)(uintptr_t)va, size);
            say("PS5 page-table prt-aperture: try id=0 va=%#llx size=%llu GiB rc=%#x cpu_delta=%d gpu_delta=%d",
                (unsigned long long)va, (unsigned long long)(size >> 30), (unsigned)rc,
                pt_cpu_used() - cpu0, pt_gpu_used() - gpu0);
            if (rc == 0) {
                best_size = size;
                best_va = va;
                prt_clear(0);
            }
            sceKernelMunmap((void *)(uintptr_t)va, size);
        }

        void *va = (void *)(uintptr_t)best_va;
        if (!best_size) {
            say("PS5 page-table prt-aperture: no size 4..128 GiB succeeded");
        } else if (sceKernelReserveVirtualRange(&va, best_size, 0, PT_UNIT_2M) != 0 || va != (void *)(uintptr_t)best_va) {
            // Re-reserve and keep the largest aperture active for the far-end and 2 GiB scatter checks.
            say("PS5 page-table prt-aperture: re-reserve %llu GiB at %#llx failed",
                (unsigned long long)(best_size >> 30), (unsigned long long)best_va);
            best_size = 0;
        }
        if (best_size) {
            int cpu0 = pt_cpu_used(), gpu0 = pt_gpu_used();
            int rc = sceKernelSetPrtAperture(0, va, best_size);
            say("PS5 page-table prt-aperture: keep id=0 va=%#llx size=%llu GiB rc=%#x cpu_delta=%d gpu_delta=%d",
                (unsigned long long)best_va, (unsigned long long)(best_size >> 30), (unsigned)rc,
                pt_cpu_used() - cpu0, pt_gpu_used() - gpu0);
            if (rc == 0) {
                prt_check_page(phys, queue_family, "first", best_va);
                prt_check_page(phys, queue_family, "middle", (best_va + best_size / 2) & ~(uint64_t)(PT_UNIT_64K - 1));
                prt_check_page(phys, queue_family, "last", best_va + best_size - PT_UNIT_64K);
                prt_scatter_2g(best_va, best_size);
                prt_clear(0);
            }
            sceKernelMunmap(va, best_size);
        }
    }

    // Test free apertures (4 GiB each at 640, 644, 648 GiB, clear of RADV's 512..640 GiB arena) and overlap behaviour.
    uint64_t a3 = 4ull << 30;
    uint64_t va3[3] = {0xa000000000ull, 0xa100000000ull, 0xa200000000ull};
    int r3[3] = {-1, -1, -1};
    int first_id = radv_holds_ap0 ? 1 : 0;
    for (int id = first_id; id < 3; id++) {
        void *p = (void *)(uintptr_t)va3[id];
        int rr = sceKernelReserveVirtualRange(&p, a3, 0, PT_UNIT_2M);
        if (rr == 0 && p == (void *)(uintptr_t)va3[id])
            r3[id] = sceKernelSetPrtAperture(id, p, a3);
        say("PS5 page-table prt-aperture: multi id=%d va=%#llx size=4GiB reserve_rc=%#x set_rc=%#x",
            id, (unsigned long long)va3[id], (unsigned)rr, (unsigned)r3[id]);
    }
    prt_get_all("3-active");
    for (int id = first_id; id < 3; id++) prt_clear(id);

    // Overlap check: id=first_id at va3[first_id]..+4GiB, id=first_id+1 exact same and partial +2GiB.
    int ov_a = first_id, ov_b = first_id + 1;
    int ov0 = sceKernelSetPrtAperture(ov_a, (void *)(uintptr_t)va3[first_id], a3);
    int ov_exact = sceKernelSetPrtAperture(ov_b, (void *)(uintptr_t)va3[first_id], a3);
    int ov_part = sceKernelSetPrtAperture(ov_b, (void *)(uintptr_t)(va3[first_id] + (2ull << 30)), a3);
    say("PS5 page-table prt-aperture: overlap set%d=%#x exact_id%d=%#x partial_id%d=%#x",
        ov_a, (unsigned)ov0, ov_b, (unsigned)ov_exact, ov_b, (unsigned)ov_part);
    prt_clear(ov_a);
    prt_clear(ov_b);
    for (int id = first_id; id < 3; id++) sceKernelMunmap((void *)(uintptr_t)va3[id], a3);

    // Last, because a GPU fault ends the title: a store to unmapped VA inside an aperture (4 GiB at va3[0]).
    void *wva = (void *)(uintptr_t)va3[0];
    if (sceKernelReserveVirtualRange(&wva, a3, 0, PT_UNIT_2M) == 0 && wva == (void *)(uintptr_t)va3[0]) {
        int wrc = sceKernelSetPrtAperture(first_id, wva, a3);
        say("PS5 page-table prt-gpu-write: aperture id=%d va=%#llx 4 GiB rc=%#x",
            first_id, (unsigned long long)va3[0], (unsigned)wrc);
        if (wrc == 0) prt_check_unmapped_write(phys, queue_family, va3[0]);
        prt_clear(first_id);
        sceKernelMunmap(wva, a3);
    } else {
        say("PS5 page-table prt-gpu-write: reserving va=%#llx failed", (unsigned long long)va3[0]);
    }
}

bool mkw_page_table_probe(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t queue_family) {
    int cpu_tot = 0, cpu_free = 0, gpu_tot = 0, gpu_free = 0;
    int cfg_rc = sceKernelGetPageTableStats(&cpu_tot, &cpu_free, &gpu_tot, &gpu_free);
    say("PS5 page-table config: rc=%d cpu_total=%d (%d MiB) gpu_total=%d",
        cfg_rc, cpu_tot, (cpu_tot * 4) / 1024, gpu_tot);
    log_pt("initial");
    bool ok_sparse = probe_sparse_image(phys, dev, queue, queue_family);
    bool ok_gpu_1g = probe_gpu_only_1g(phys, dev);
    bool ok_64k = probe_direct_granularity(PT_UNIT_64K, "2g-64k");
    bool ok_2m = probe_direct_granularity(PT_UNIT_2M, "2g-2m");

    int cpu_rw = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    int gpu_rw = PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE;
    int min_delta = 1 << 30;
    int d = -1;
    if (probe_alias_2m(false, PS5_KERNEL_DIRECT_TYPE_CPU, cpu_rw, "alias-2m", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(true, PS5_KERNEL_DIRECT_TYPE_CPU, cpu_rw, "map2-alias-2m", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_distinct_2m(true, "map2-distinct-2m", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(false, PT_MEM_WB_ONION, cpu_rw, "wb-onion-cpu", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(false, PT_MEM_WB_ONION, gpu_rw, "wb-onion-gpuonly", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(false, PT_MEM_WC_GARLIC, cpu_rw, "wc-garlic-cpu", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(false, PT_MEM_WC_GARLIC, gpu_rw, "wc-garlic-gpuonly", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(false, PT_MEM_WB_GARLIC, cpu_rw, "wb-garlic-cpu", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(false, PT_MEM_WB_GARLIC, gpu_rw, "wb-garlic-gpuonly", &d) && d >= 0 && d < min_delta) min_delta = d;
    if (probe_alias_2m(false, PS5_KERNEL_DIRECT_TYPE_CPU, gpu_rw, "cpu12-gpuonly", &d) && d >= 0 && d < min_delta) min_delta = d;

    // Experiment 3 reads reserved, unmapped VA on the GPU. On the console it ends the process
    // (klog-probe-pt6: exception 0xa0d0c005, GPU_FAULT_PAGE_FAULT_ASYNC), so it runs only in a build
    // made with build_smoke.py --page-table --unmapped-gpu-read.
#ifdef MKW_PT_UNMAPPED_GPU_READ
    if (min_delta >= 512) probe_gpu_read_at(phys, queue_family, "unmapped-gpu-read", NULL);
    else say("PS5 page-table unmapped-gpu-read: skipped (cheap mapping found, min_delta=%d)", min_delta);
#else
    (void)min_delta;
    say("PS5 page-table unmapped-gpu-read: skipped (faults the GPU; build with --unmapped-gpu-read)");
#endif

    probe_amm_ranges();
    probe_prt_aperture(phys, queue_family);
    say("PS5 page-table amm-map: skipped (public sources disagree on sceAmprCommandBufferSetBuffer; docs/research.md)");

    bool ok = ok_sparse && ok_gpu_1g && ok_64k && ok_2m;
    say("PS5 page-table probe: %s (sparse=%d, gpu_1g=%d, 2g-64k=%d, 2g-2m=%d)",
        ok ? "passed" : "failed", ok_sparse, ok_gpu_1g, ok_64k, ok_2m);
    return ok;
}
