// Direct display through RADV's VideoOut WSI. The enumeration and surface
// sequence follows PS5_VulkanTemplate base/VulkanSwapChain.cpp @ 91179df
// (MIT, Sascha Willems and Mihawk). See docs/research.md.
#include "gfx_wsys.h"

#include <stdlib.h>

#include "host.h"
#include "platform/platform.h"

typedef struct {
    VkInstance instance;
    VkPhysicalDevice device;
    VkSurfaceKHR surface;
    VkExtent2D extent;
    uint32_t refresh_mhz;
} DisplaySurface;

static DisplaySurface pending;   // candidate surface owned by device selection
static uint64_t display_size;    // width and height published together
static volatile int32_t close_started;

int gfx_window_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_DISPLAY_EXTENSION_NAME;
    return 2;
}

static int supports_display(VkPhysicalDevice device, uint32_t plane, VkDisplayKHR display) {
    uint32_t count = 0;
    if (vkGetDisplayPlaneSupportedDisplaysKHR(device, plane, &count, NULL) != VK_SUCCESS || !count) return 0;
    VkDisplayKHR *supported = calloc(count, sizeof *supported);
    if (!supported) return 0;
    int found = 0;
    if (vkGetDisplayPlaneSupportedDisplaysKHR(device, plane, &count, supported) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; i++)
            if (supported[i] == display) found = 1;
    free(supported);
    return found;
}

static uint64_t mode_score(VkDisplayModeParametersKHR mode) {
    // Prefer the 1080p60 mode used by the native template. Rank alternatives
    // by their distance from that extent and refresh.
    uint64_t w = mode.visibleRegion.width, h = mode.visibleRegion.height, hz = mode.refreshRate;
    uint64_t dw = w < 1920 ? 1920 - w : w - 1920;
    uint64_t dh = h < 1080 ? 1080 - h : h - 1080;
    uint64_t dr = hz < 60000 ? 60000 - hz : hz - 60000;
    return (dw + dh) * 1000000 + dr;
}

static void choose_mode(VkPhysicalDevice device, const VkDisplayPropertiesKHR *display,
                        const VkDisplayPlanePropertiesKHR *planes, uint32_t plane_count,
                        VkDisplaySurfaceCreateInfoKHR *best, uint32_t *refresh, uint64_t *score) {
    uint32_t mode_count = 0;
    if (vkGetDisplayModePropertiesKHR(device, display->display, &mode_count, NULL) != VK_SUCCESS ||
        !mode_count || !(display->supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        return;
    VkDisplayModePropertiesKHR *modes = calloc(mode_count, sizeof *modes);
    if (!modes) return;
    if (vkGetDisplayModePropertiesKHR(device, display->display, &mode_count, modes) == VK_SUCCESS)
        for (uint32_t plane = 0; plane < plane_count; plane++) {
            if (planes[plane].currentDisplay && planes[plane].currentDisplay != display->display) continue;
            if (!supports_display(device, plane, display->display)) continue;
            for (uint32_t i = 0; i < mode_count; i++) {
                VkDisplayModeParametersKHR mode = modes[i].parameters;
                if (!mode.visibleRegion.width || !mode.visibleRegion.height || !mode.refreshRate) continue;
                uint64_t candidate = mode_score(mode);
                if (candidate >= *score) continue;
                VkDisplayPlaneCapabilitiesKHR caps;
                if (vkGetDisplayPlaneCapabilitiesKHR(device, modes[i].displayMode, plane, &caps) != VK_SUCCESS ||
                    !caps.supportedAlpha)
                    continue;
                VkDisplayPlaneAlphaFlagsKHR alpha = caps.supportedAlpha;
                *best = (VkDisplaySurfaceCreateInfoKHR){
                    .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
                    .displayMode = modes[i].displayMode,
                    .planeIndex = plane,
                    .planeStackIndex = planes[plane].currentStackIndex,
                    .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                    .globalAlpha = 1.0f,
                    .alphaMode = (VkDisplayPlaneAlphaFlagBitsKHR)(alpha & (0u - alpha)),
                    .imageExtent = mode.visibleRegion,
                };
                *refresh = mode.refreshRate;
                *score = candidate;
            }
        }
    free(modes);
}

void ps5_window_discard_candidate(VkInstance instance) {
    if (pending.instance != instance) return;
    if (pending.surface) vkDestroySurfaceKHR(instance, pending.surface, NULL);
    pending = (DisplaySurface){0};
}

static int prepare_surface(VkInstance instance, VkPhysicalDevice device) {
    if (pending.instance == instance && pending.device == device && pending.surface) return 1;
    if (pending.instance) ps5_window_discard_candidate(pending.instance);

    uint32_t display_count = 0, plane_count = 0;
    if (vkGetPhysicalDeviceDisplayPropertiesKHR(device, &display_count, NULL) != VK_SUCCESS || !display_count ||
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(device, &plane_count, NULL) != VK_SUCCESS || !plane_count)
        return 0;

    VkDisplayPropertiesKHR *displays = calloc(display_count, sizeof *displays);
    VkDisplayPlanePropertiesKHR *planes = calloc(plane_count, sizeof *planes);
    VkDisplaySurfaceCreateInfoKHR best = {0};
    uint64_t score = UINT64_MAX;
    uint32_t refresh = 0;
    if (displays && planes &&
        vkGetPhysicalDeviceDisplayPropertiesKHR(device, &display_count, displays) == VK_SUCCESS &&
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(device, &plane_count, planes) == VK_SUCCESS)
        for (uint32_t i = 0; i < display_count; i++)
            choose_mode(device, &displays[i], planes, plane_count, &best, &refresh, &score);
    free(planes);
    free(displays);
    if (!best.displayMode) return 0;

    VkSurfaceKHR surface;
    if (vkCreateDisplayPlaneSurfaceKHR(instance, &best, NULL, &surface) != VK_SUCCESS) return 0;
    pending = (DisplaySurface){instance, device, surface, best.imageExtent, refresh};
    return 1;
}

int gfx_window_vk_presentable(VkInstance instance, VkPhysicalDevice device, uint32_t family) {
    VkBool32 supported = VK_FALSE;
    return prepare_surface(instance, device) &&
           vkGetPhysicalDeviceSurfaceSupportKHR(device, family, pending.surface, &supported) == VK_SUCCESS &&
           supported;
}

int wsys_open(void) { return vk_ctx() != NULL; }

VkSurfaceKHR wsys_surface(VkInstance instance) {
    const VkCtx *ctx = vk_ctx();
    if (!ctx || instance != ctx->instance || !gfx_window_vk_presentable(instance, ctx->phys, ctx->gfx_family))
        return VK_NULL_HANDLE;
    DisplaySurface selected = pending;
    pending = (DisplaySurface){0};
    __atomic_store_n(&display_size, ((uint64_t)selected.extent.width << 32) | selected.extent.height, __ATOMIC_RELEASE);
    gfx_window_set_refresh(selected.refresh_mhz / 1000.0);
    gfx_window_focus_changed(1);
    rt_log("gfx: PS5 direct display %ux%u at %.3f Hz", selected.extent.width, selected.extent.height,
           selected.refresh_mhz / 1000.0);
    return selected.surface;   // gfx_present.c owns and destroys it from here
}

void wsys_client_size(int *width, int *height) {
    uint64_t size = __atomic_load_n(&display_size, __ATOMIC_ACQUIRE);
    *width = (int)(size >> 32);
    *height = (int)(uint32_t)size;
}

int gfx_window_monitor_size(int *width, int *height) {
    wsys_client_size(width, height);
    return *width && *height;
}

int gfx_window_lines(void) { return (int)(uint32_t)__atomic_load_n(&display_size, __ATOMIC_ACQUIRE); }
int wsys_minimized(void) { return gfx_window_lines() == 0; }

void gfx_window_apply_settings(void) { gfx_window_mark_dirty(); }

void wsys_resize(int width, int height) {
    int current_w, current_h;
    wsys_client_size(&current_w, &current_h);
    if (width != current_w || height != current_h)
        rt_log("gfx: PS5 display mode fixes output at %dx%d", current_w, current_h);
}

static void close_window(void *unused) {
    (void)unused;
    gfx_window_closed();
}

void gfx_window_request_close(void) {
    if (plat_cas32(&close_started, 1, 0)) return;
    if (!plat_thread_spawn(close_window, NULL, "display close", 0)) {
        plat_xchg32(&close_started, 0);
        rt_log("gfx: could not start PS5 title shutdown");
    }
}

void wsys_probe_focus(int in) { (void)in; }
void wsys_probe_escape(void) {}
int wsys_probe_click(int where) { (void)where; return 0; }
