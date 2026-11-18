// Clear and present a diagnostic frame through PS5_VulkanTemplate's MIT
// foundation (91179df760df1e6c6f2d4d172bc435319d79ff3c). See docs/project/research.md.
#include "vulkanexamplebase.h"

#include <chrono>
#include <thread>

// Exit after a failed probe only once its log has drained: the title's standard error reaches klog asynchronously, and
// an immediate exit loses the tail, failure line included (console run: two launches cut off at the same line).
[[noreturn]] static void probe_failed(const char *message) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    vks::tools::exitFatal(message, -1);
    __builtin_unreachable();
}

extern "C" bool mkw_smoke_probe(VkPhysicalDevice device, uint32_t queue_family, uint32_t instance_api,
                                const char *presentation);
extern "C" bool mkw_runtime_dispatch_probe();
#ifdef MKW_PS5_QUERY_SMOKE
extern "C" bool mkw_query_memory_probe();
#endif
#ifdef MKW_PS5_ALIAS_SMOKE
extern "C" bool mkw_ps5_alias_subrange_probe();
#endif
#ifdef MKW_PS5_PAGE_TABLE_SMOKE
extern "C" bool mkw_page_table_probe(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t queue_family);
#endif
#ifdef MKW_PS5_DUALSENSE_SMOKE
extern "C" void mkw_pad_probe_start();
extern "C" bool mkw_pad_probe_frame();
extern "C" void mkw_pad_probe_finish();
#endif

class VulkanExample : public VulkanExampleBase {
public:
    VulkanExample() {
        title = "MKW PS5 Smoke";
        apiVersion = VK_API_VERSION_1_3;
        settings.overlay = false;
    }

    void prepare() override {
        VulkanExampleBase::prepare();
        const char *presentation = "swapchain";
#ifdef PS5_HOST_REFERENCE
        if (swapChain.offscreen) presentation = "offscreen";
#endif
        if (!mkw_smoke_probe(physicalDevice, swapChain.queueNodeIndex, apiVersion, presentation))
            probe_failed("PS5 memory mixed-page probe failed");
        if (!mkw_runtime_dispatch_probe()) probe_failed("Runtime dispatch probe failed");
#ifdef MKW_PS5_QUERY_SMOKE
        if (!mkw_query_memory_probe()) probe_failed("PS5 QueryMemory: failed");
#endif
#ifdef MKW_PS5_ALIAS_SMOKE
        if (!mkw_ps5_alias_subrange_probe()) probe_failed("PS5 alias subrange probe failed");
#endif
#ifdef MKW_PS5_PAGE_TABLE_SMOKE
        if (!mkw_page_table_probe(physicalDevice, device, queue, swapChain.queueNodeIndex))
            probe_failed("PS5 page-table probe failed");
#endif
#ifdef MKW_PS5_DUALSENSE_SMOKE
        if (!ps5.frameBudget) ps5.frameBudget = 900;
        mkw_pad_probe_start();
#else
        if (!ps5.frameBudget) {
            ps5.frameBudget = 240; // Hold the clear on screen for four seconds at 60 Hz.
        }
#endif
        prepared = true;
    }

    void render() override {
        prepareFrame();
        VkCommandBuffer command = drawCmdBuffers[currentBuffer];
        VkClearValue clear[2] = {};
        clear[0].color = {{0.015f, 0.18f, 0.64f, 1.0f}};
#ifdef MKW_PS5_DUALSENSE_SMOKE
        if (mkw_pad_probe_frame()) clear[0].color = {{0.01f, 0.65f, 0.08f, 1.0f}};
#endif
        clear[1].depthStencil = {1.0f, 0};
        const VkRenderPassBeginInfo pass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderPass,
            .framebuffer = frameBuffers[currentImageIndex],
            .renderArea = {{0, 0}, {width, height}},
            .clearValueCount = 2,
            .pClearValues = clear,
        };
        const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK_RESULT(vkBeginCommandBuffer(command, &begin));
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command);
        VK_CHECK_RESULT(vkEndCommandBuffer(command));
        submitFrame();
#ifdef MKW_PS5_DUALSENSE_SMOKE
        if (ps5.framesDrawn + 1 >= ps5.frameBudget) mkw_pad_probe_finish();
#endif
    }
};

VULKAN_EXAMPLE_MAIN()
