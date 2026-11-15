// Diagnostics on the template's presentation device after swapchain creation.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "platform.h" // PS5_VulkanTemplate's title logger

#ifdef PS5_HOST_REFERENCE
#include <errno.h>
#include <sys/mman.h>
#else
#include <ps5platform/kernel.h>
#include "backing_layout.h"
bool mkw_ps5_coexistence_probe(void);
bool mkw_ps5_memory_probe(uintptr_t stack_base, uintptr_t alias_base);
bool mkw_ps5_mixed_probe(uintptr_t base);
#endif

#if defined(PS5_HOST_REFERENCE) && defined(MAP_FIXED_NOREPLACE)
#define VA_EXCLUSIVE_FLAGS MAP_FIXED_NOREPLACE
#endif

typedef struct {
    const char *name;
    uintptr_t start;
    size_t bytes;
} Range;

typedef struct {
    const char *name;
    const Range *ranges;
    size_t count;
    bool try_exclusive;
} LayoutTrial;

#include "probe_layouts.h" // emitted by build_smoke.py from recomp/layout.py

#ifdef PS5_HOST_REFERENCE
static int reserve_mmap(void **address, size_t bytes, int prot, int flags) {
    flags |= MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void *mapped = mmap(*address, bytes, prot, flags, -1, 0);
    if (mapped == MAP_FAILED) return errno;
    *address = mapped;
    return 0;
}
#endif

static int reserve(void **address, size_t bytes, bool exclusive) {
#ifndef PS5_HOST_REFERENCE
    // Flags zero allow relocation and never request replacement. Count it
    // only if the returned address equals the requested hint.
    (void)exclusive;
    return sceKernelReserveVirtualRange(address, bytes, 0, PS5_KERNEL_DIRECT_ALIGNMENT);
#else
    int flags = 0;
#ifdef VA_EXCLUSIVE_FLAGS
    if (exclusive) flags = VA_EXCLUSIVE_FLAGS;
#else
    if (exclusive) return ENOTSUP;
#endif
    return reserve_mmap(address, bytes, PROT_NONE, flags);
#endif
}

static bool release(void *address, size_t bytes) {
#ifdef PS5_HOST_REFERENCE
    const int result = munmap(address, bytes) == 0 ? 0 : errno;
#else
    const int result = sceKernelMunmap(address, bytes);
#endif
    if (result) say("VA release at %p size=%#llx failed: %#x", address, (unsigned long long)bytes, (unsigned)result);
    return !result;
}

#ifdef PS5_HOST_REFERENCE
static bool check_exclusive_mmap(void) {
#ifdef VA_EXCLUSIVE_FLAGS
    // OcarinaPS5 9c97f28 uses the no-replace mmap fallback. Before applying it
    // to guest addresses, require an actual collision to preserve our own data.
    const size_t bytes = 0x10000;
    const int writable = PROT_READ | PROT_WRITE;
    void *owned = NULL;
    const int create = reserve_mmap(&owned, bytes, writable, 0);
    if (create) {
        say("VA exclusive mmap disabled: cannot allocate collision check (%#x)", (unsigned)create);
        return false;
    }
    volatile uint8_t *marker = owned;
    *marker = 0xa5;
    void *collision = owned;
    const int result = reserve_mmap(&collision, bytes, writable, VA_EXCLUSIVE_FLAGS);
    const bool preserved = *marker == 0xa5;
    bool cleanup_ok = release(owned, bytes);
    if (!result && collision != owned) cleanup_ok &= release(collision, bytes);
    const bool verified = result == EEXIST && preserved && cleanup_ok;
    say("VA exclusive mmap collision check: %s status=%#x markerPreserved=%d",
        verified ? "passed" : "disabled", (unsigned)result, preserved);
    return verified;
#else
    say("VA exclusive mmap disabled: no no-replace flag in host headers");
    return false;
#endif
}
#endif

static bool probe_layout(const LayoutTrial *trial, bool exclusive) {
    const char *api = exclusive ? " mmap" : "";
    bool exact[MAX_TRIAL_RANGES] = {0};
    bool cleanup_ok = true;
    const Range *first_miss = NULL;
    for (size_t i = 0; i < trial->count; i++) {
        const Range *range = &trial->ranges[i];
        void *requested = (void *)range->start;
        void *address = requested;
        const int result = reserve(&address, range->bytes, exclusive);
        const char *status = result ? "failed" : address == requested ? "exact" : "relocated";
        say("VA %s%s %s [%p, %p): %s returned=%p status=%#x", trial->name, api, range->name, requested,
            (void *)(range->start + range->bytes), status, result ? NULL : address, (unsigned)result);
        exact[i] = !result && address == requested;
        if (!exact[i] && !first_miss) first_miss = range;
        if (!result && !exact[i]) cleanup_ok &= release(address, range->bytes);
    }
    if (first_miss)
        say("VA %s%s first mismatch: %s address=%p size=%#llx", trial->name, api, first_miss->name,
            (void *)first_miss->start, (unsigned long long)first_miss->bytes);
    say("VA %s%s all reservations exact: %d", trial->name, api, first_miss == NULL);
    // Keep every exact piece in place until the trial has checked all ranges.
    for (size_t i = trial->count; i-- > 0;) {
        const Range *range = &trial->ranges[i];
        if (exact[i]) cleanup_ok &= release((void *)range->start, range->bytes);
    }
    return cleanup_ok;
}

static bool has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++) {
        if (!strcmp(extensions[i].extensionName, name)) return true;
    }
    return false;
}

static void probe_vulkan(VkPhysicalDevice device, uint32_t queue_family, const char *presentation) {
    VkPhysicalDeviceProperties identity;
    vkGetPhysicalDeviceProperties(device, &identity);
    say("Vulkan GPU %s vendor=%#x device=%#x deviceAPI=%u.%u.%u driverVersion=%#x", identity.deviceName,
        identity.vendorID, identity.deviceID, VK_API_VERSION_MAJOR(identity.apiVersion),
        VK_API_VERSION_MINOR(identity.apiVersion), VK_API_VERSION_PATCH(identity.apiVersion), identity.driverVersion);

    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL);
    VkExtensionProperties *extensions = result == VK_SUCCESS ? calloc(count ? count : 1, sizeof(*extensions)) : NULL;
    if (!extensions) {
        say("Vulkan extension enumeration unavailable: %d", result);
        return;
    }
    result = vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions);
    if (result != VK_SUCCESS) {
        say("Vulkan extension enumeration failed: %d", result);
        free(extensions);
        return;
    }
    const bool swapchain = has_extension(extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const bool push = has_extension(extensions, count, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    const bool depth_clip = has_extension(extensions, count, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    const bool external_host = has_extension(extensions, count, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    const bool mesh = has_extension(extensions, count, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    free(extensions);
    say("Vulkan extensions: swapchain=%d pushDescriptor=%d depthClipControl=%d externalMemoryHost=%d meshShader=%d",
        swapchain, push, depth_clip, external_host, mesh);

    VkPhysicalDeviceMeshShaderFeaturesEXT task_mesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,
        .pNext = mesh ? &task_mesh : NULL,
    };
    void *extra = depth_clip ? (void *)&clip : mesh ? (void *)&task_mesh : NULL;
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = extra};
    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = identity.apiVersion >= VK_API_VERSION_1_3 ? &v13 : extra,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = identity.apiVersion >= VK_API_VERSION_1_2 ? &v12 : extra,
    };
    vkGetPhysicalDeviceFeatures2(device, &features);
    const VkPhysicalDeviceFeatures *core = &features.features;
    say("Vulkan 1.2: timelineSemaphore=%u drawIndirectCount=%u scalarBlockLayout=%u bufferDeviceAddress=%u descriptorIndexing=%u runtimeDescriptorArray=%u",
        v12.timelineSemaphore, v12.drawIndirectCount, v12.scalarBlockLayout, v12.bufferDeviceAddress,
        v12.descriptorIndexing, v12.runtimeDescriptorArray);
    say("Vulkan features: dynamicRendering=%u synchronization2=%u depthClipControl=%u shaderStorageImageWriteWithoutFormat=%u shaderInt64=%u",
        v13.dynamicRendering, v13.synchronization2, clip.depthClipControl, core->shaderStorageImageWriteWithoutFormat,
        core->shaderInt64);
    say("Vulkan features: samplerAnisotropy=%u independentBlend=%u geometryShader=%u textureCompressionBC=%u taskShader=%u meshShader=%u meshShaderQueries=%u",
        core->samplerAnisotropy, core->independentBlend, core->geometryShader, core->textureCompressionBC,
        task_mesh.taskShader, task_mesh.meshShader, task_mesh.meshShaderQueries);

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
    };
    VkPhysicalDeviceDriverProperties driver = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        .pNext = external_host ? &host : NULL,
    };
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = identity.apiVersion >= VK_API_VERSION_1_2 ? (void *)&driver : external_host ? (void *)&host : NULL,
    };
    vkGetPhysicalDeviceProperties2(device, &properties);
    say("Vulkan driver %s (%s), minImportedHostPointerAlignment=%llu", driver.driverName, driver.driverInfo,
        (unsigned long long)host.minImportedHostPointerAlignment);

    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    VkQueueFamilyProperties *queues = calloc(count ? count : 1, sizeof(*queues));
    if (!queues) {
        say("Vulkan queue properties unavailable");
        return;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, queues);
    if (queue_family < count) {
        const VkQueueFlags flags = queues[queue_family].queueFlags;
        say("Vulkan queue %u selected for %s: graphics=%d compute=%d", queue_family, presentation,
            !!(flags & VK_QUEUE_GRAPHICS_BIT), !!(flags & VK_QUEUE_COMPUTE_BIT));
    } else {
        say("Vulkan queue %u selected for %s is out of range (%u families)", queue_family, presentation, count);
    }
    free(queues);
}

static bool probe_address_layouts(void) {
    say("VA probe checks address reservations only");
    const size_t trials = sizeof layout_trials / sizeof *layout_trials;
    for (size_t i = 0; i < trials; i++) {
        if (!probe_layout(&layout_trials[i], false)) {
            say("VA cleanup incomplete; skipping remaining trials");
            return false;
        }
    }
#ifdef PS5_HOST_REFERENCE
    if (!check_exclusive_mmap()) return false;
    for (size_t i = 0; i < trials; i++) {
        if (layout_trials[i].try_exclusive && !probe_layout(&layout_trials[i], true)) {
            say("VA cleanup incomplete; skipping remaining trials");
            return false;
        }
    }
#endif
    return true;
}

bool mkw_smoke_probe(VkPhysicalDevice device, uint32_t queue_family, uint32_t instance_api, const char *presentation) {
    say("Vulkan instance requested API=%u.%u.%u", VK_API_VERSION_MAJOR(instance_api),
        VK_API_VERSION_MINOR(instance_api), VK_API_VERSION_PATCH(instance_api));
    probe_vulkan(device, queue_family, presentation);
#ifdef PS5_HOST_REFERENCE
    probe_address_layouts();
    return true;
#else
    say("PS5 title direct-memory pool: %lld bytes", (long long)sceKernelGetDirectMemorySize());
    if (probe_address_layouts() && mkw_ps5_coexistence_probe() &&
        mkw_ps5_memory_probe(PS5_BACKING_STACK_BASE, PS5_BACKING_ALIAS_BASE))
        return mkw_ps5_mixed_probe(PS5_BACKING_STACK_BASE);
    say("PS5 memory mixed-page: failed (preceding memory checks did not pass)");
    return false;
#endif
}
