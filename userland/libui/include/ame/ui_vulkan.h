#pragma once
#include "ame/ui.h"
#include <vulkan/vulkan.h>

namespace ame::ui {
// SPIR-V supplied by the caller. Words must be 4-byte aligned and `bytes` a
// multiple of four. The renderer never embeds device- or build-specific blobs:
// the application ships the source-derived module and the driver's own shader
// cache retains the compiled result.
struct Spirv {
    const void* words = nullptr;
    size_t bytes = 0;
    explicit operator bool() const { return words && bytes >= 20 && !(bytes % 4); }
};

// Normalized source rectangle, 0..1, y down in texture space.
struct Uv { float x0 = 0, y0 = 0, x1 = 1, y1 = 1; };

// A single-target GPU sink. Image/view/device/queue are borrowed and must
// outlive this owner. The caller must prove DISPLAY retirement before begin;
// wait() proves GPU completion only. This is not an image-sharing protocol.
class VulkanRenderer {
  public:
    enum class State : uint8_t { Empty, Ready, Recording, Submitted, Poisoned };
    static constexpr uint32_t kMaxDraws = 16384;
    VulkanRenderer() = default;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    // R8G8B8A8_UNORM, one sample, one mip/layer, COLOR_ATTACHMENT usage and
    // same-queue ownership. Full redraw discards previous target contents.
    // Fragment shaders receive premultiplied output over a cleared target.
    VkResult initialize(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
                        VkImage image, VkImageView view, uint32_t width, uint32_t height,
                        Spirv solid_vertex, Spirv solid_fragment,
                        Spirv blit_vertex, Spirv blit_fragment);
    VkResult begin(uint32_t background = 0xff000000);
    bool rectangle(Rect box, Rect clip, uint32_t argb);
    bool text(Rect box, Rect clip, const Text& value, uint32_t argb);
    // Generic one-texture blit with normalized UVs: the compositor's quad.
    bool sampled_quad(Rect box, Uv uv, Rect clip, uint32_t argb);
    VkResult submit();
    VkResult wait(uint64_t timeout_ns);
    // Reset an unsubmitted recording after a sink/budget failure.
    VkResult cancel();
    // Returns failure and retains resources on timeout or uncertain GPU use.
    // A stalled/device-lost submission is quarantined, never freed on timeout.
    VkResult destroy(uint64_t timeout_ns = 0);
    State state() const { return state_; }
    uint32_t draws() const { return draws_; }
    VkResult error() const { return error_; }
  private:
    struct Quad { float origin[2], size[2], uv0[2], uv1[2], color[4]; };
    VkResult initialize_atlas();
    VkResult create_pipeline(VkPipelineLayout layout, Spirv vertex, Spirv fragment,
                             bool vertex_buffer, VkPipeline& out);
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags wanted) const;
    State state_ = State::Empty;
    VkResult error_ = VK_SUCCESS;
    bool in_flight_ = false;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkPipelineLayout solid_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout blit_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline blit_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImage atlas_ = VK_NULL_HANDLE;
    VkImageView atlas_view_ = VK_NULL_HANDLE;
    VkDeviceMemory atlas_memory_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    VkBuffer quad_ = VK_NULL_HANDLE;
    VkDeviceMemory quad_memory_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0, draws_ = 0;
};
} // namespace ame::ui
