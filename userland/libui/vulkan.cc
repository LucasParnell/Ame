#include "ame/ui_vulkan.h"
#include "midix/font/vga8x16.h"

namespace ame::ui {
namespace {
constexpr uint32_t kGlyphFirst = 0x20, kGlyphLast = 0x7e;
constexpr uint32_t kCellWidth = 8, kCellHeight = 16;
constexpr uint32_t kAtlasColumns = 16, kAtlasRows = 6;
constexpr uint32_t kAtlasWidth = kAtlasColumns * kCellWidth, kAtlasHeight = kAtlasRows * kCellHeight;
constexpr uint32_t kAtlasBytes = kAtlasWidth * kAtlasHeight;
// 0.0..1.0 unit quad; the vertex shader maps it through a clip-space rect.
constexpr float kUnitQuad[12] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};

void premultiply(uint32_t argb, float (&rgba)[4]) {
    const auto alpha = float(argb >> 24) / 255.0f;
    rgba[0] = float((argb >> 16) & 255) / 255.0f * alpha;
    rgba[1] = float((argb >> 8) & 255) / 255.0f * alpha;
    rgba[2] = float(argb & 255) / 255.0f * alpha;
    rgba[3] = alpha;
}
void straight(uint32_t argb, float (&rgba)[4]) {
    rgba[0] = float((argb >> 16) & 255) / 255.0f;
    rgba[1] = float((argb >> 8) & 255) / 255.0f;
    rgba[2] = float(argb & 255) / 255.0f;
    rgba[3] = float(argb >> 24) / 255.0f;
}
} // namespace

uint32_t VulkanRenderer::memory_type(uint32_t mask, VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

VkResult VulkanRenderer::create_pipeline(VkPipelineLayout layout, Spirv vertex, Spirv fragment,
                                         bool vertex_buffer, VkPipeline& out) {
    const VkShaderModuleCreateInfo info[2] = {
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, vertex.bytes,
         static_cast<const uint32_t*>(vertex.words)},
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, fragment.bytes,
         static_cast<const uint32_t*>(fragment.words)}};
    VkShaderModule modules[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        const auto result = vkCreateShaderModule(device_, &info[i], nullptr, &modules[i]);
        if (result != VK_SUCCESS) {
            if (modules[0]) vkDestroyShaderModule(device_, modules[0], nullptr);
            return result;
        }
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = modules[i]; stages[i].pName = "main";
    }
    VkVertexInputBindingDescription binding{0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex_state{};
    vertex_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (vertex_buffer) {
        vertex_state.vertexBindingDescriptionCount = 1; vertex_state.pVertexBindingDescriptions = &binding;
        vertex_state.vertexAttributeDescriptionCount = 1;
        vertex_state.pVertexAttributeDescriptions = &attribute;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Premultiplied source over destination.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1; blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 2; pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex_state; pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples; pipeline.pColorBlendState = &blending;
    pipeline.pDynamicState = &dynamic; pipeline.layout = layout; pipeline.renderPass = pass_;
    const auto result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &out);
    for (auto module : modules) vkDestroyShaderModule(device_, module, nullptr);
    return result;
}

VkResult VulkanRenderer::initialize_atlas() {
    uint8_t pixels[kAtlasBytes]{};
    for (uint32_t ch = kGlyphFirst; ch <= kGlyphLast; ++ch) {
        const auto cell = ch - kGlyphFirst;
        const auto* glyph = midix::font::vga8x16_glyph(uint8_t(ch));
        const auto ox = (cell % kAtlasColumns) * kCellWidth, oy = (cell / kAtlasColumns) * kCellHeight;
        for (uint32_t y = 0; y < kCellHeight; ++y)
            for (uint32_t x = 0; x < kCellWidth; ++x)
                if (glyph[y] & (0x80 >> x)) pixels[(oy + y) * kAtlasWidth + ox + x] = 255;
    }
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = kAtlasBytes; buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    auto result = vkCreateBuffer(device_, &buffer, nullptr, &staging_);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, staging_, &requirements);
    const auto index = memory_type(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (index == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                    requirements.size, index};
    result = vkAllocateMemory(device_, &allocation, nullptr, &staging_memory_);
    if (result != VK_SUCCESS) return result;
    result = vkBindBufferMemory(device_, staging_, staging_memory_, 0);
    if (result != VK_SUCCESS) return result;
    void* mapped = nullptr;
    result = vkMapMemory(device_, staging_memory_, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) return result;
    __builtin_memcpy(mapped, pixels, kAtlasBytes);
    vkUnmapMemory(device_, staging_memory_);

    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R8_UNORM;
    image.extent = {kAtlasWidth, kAtlasHeight, 1}; image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(device_, &image, nullptr, &atlas_);
    if (result != VK_SUCCESS) return result;
    vkGetImageMemoryRequirements(device_, atlas_, &requirements);
    const auto atlas_index = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (atlas_index == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo atlas_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                          requirements.size, atlas_index};
    result = vkAllocateMemory(device_, &atlas_allocation, nullptr, &atlas_memory_);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(device_, atlas_, atlas_memory_, 0);
    if (result != VK_SUCCESS) return result;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = atlas_; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = VK_FORMAT_R8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = vkCreateImageView(device_, &view, nullptr, &atlas_view_);
    if (result != VK_SUCCESS) return result;

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    result = vkCreateSampler(device_, &sampler, nullptr, &sampler_);
    if (result != VK_SUCCESS) return result;

    // One transfer pass: whole-atlas copy, then hand the image to the sampler.
    result = vkResetCommandPool(device_, pool_, 0);
    if (result != VK_SUCCESS) return result;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    result = vkBeginCommandBuffer(command_, &begin);
    if (result != VK_SUCCESS) return result;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = atlas_; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kAtlasWidth, kAtlasHeight, 1};
    vkCmdCopyBufferToImage(command_, staging_, atlas_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    result = vkEndCommandBuffer(command_);
    if (result != VK_SUCCESS) return result;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    result = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) return result;
    return vkQueueWaitIdle(queue_);
}

VkResult VulkanRenderer::initialize(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                    uint32_t family, VkImage image, VkImageView view,
                                    uint32_t width, uint32_t height, Spirv solid_vertex,
                                    Spirv solid_fragment, Spirv blit_vertex, Spirv blit_fragment) {
    if (state_ != State::Empty || !physical || !device || !queue || !image || !view ||
        !solid_vertex || !solid_fragment || !blit_vertex || !blit_fragment ||
        !width || !height || width > 4096 || height > 4096) return VK_ERROR_INITIALIZATION_FAILED;
    physical_ = physical; device_ = device; queue_ = queue;
    image_ = image; view_ = view; width_ = width; height_ = height;
    // All partial initialization is unsubmitted and can be safely unwound.
    auto fail = [&](VkResult result) { (void)destroy(); error_ = result; return result; };
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo render_pass{};
    render_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass.attachmentCount = 1; render_pass.pAttachments = &attachment;
    render_pass.subpassCount = 1; render_pass.pSubpasses = &subpass;
    auto result = vkCreateRenderPass(device_, &render_pass, nullptr, &pass_);
    if (result != VK_SUCCESS) return fail(result);
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = pass_; framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &view_; framebuffer.width = width;
    framebuffer.height = height; framebuffer.layers = 1;
    result = vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_);
    if (result != VK_SUCCESS) return fail(result);

    // Solid layout: colour only. Blit layout: clip-space quad plus tint.
    const VkPushConstantRange solid_range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &solid_range;
    result = vkCreatePipelineLayout(device_, &layout, nullptr, &solid_layout_);
    if (result != VK_SUCCESS) return fail(result);

    const VkPushConstantRange blit_ranges[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 32},
                                                {VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16}};
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptor{};
    descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor.bindingCount = 1; descriptor.pBindings = &binding;
    result = vkCreateDescriptorSetLayout(device_, &descriptor, nullptr, &descriptor_layout_);
    if (result != VK_SUCCESS) return fail(result);
    layout.pushConstantRangeCount = 2; layout.pPushConstantRanges = blit_ranges;
    layout.setLayoutCount = 1; layout.pSetLayouts = &descriptor_layout_;
    result = vkCreatePipelineLayout(device_, &layout, nullptr, &blit_layout_);
    if (result != VK_SUCCESS) return fail(result);

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
    result = vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_);
    if (result != VK_SUCCESS) return fail(result);
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptor_pool_; allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_layout_;
    result = vkAllocateDescriptorSets(device_, &allocate, &descriptor_);
    if (result != VK_SUCCESS) return fail(result);

    VkCommandPoolCreateInfo command_pool{};
    command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; command_pool.queueFamilyIndex = family;
    result = vkCreateCommandPool(device_, &command_pool, nullptr, &pool_);
    if (result != VK_SUCCESS) return fail(result);
    VkCommandBufferAllocateInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool = pool_; commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device_, &commands, &command_);
    if (result != VK_SUCCESS) return fail(result);

    result = initialize_atlas();
    if (result != VK_SUCCESS) return fail(result);
    VkDescriptorImageInfo info{sampler_, atlas_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    VkBufferCreateInfo quad{};
    quad.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    quad.size = sizeof(kUnitQuad); quad.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    result = vkCreateBuffer(device_, &quad, nullptr, &quad_);
    if (result != VK_SUCCESS) return fail(result);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, quad_, &requirements);
    const auto index = memory_type(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (index == UINT32_MAX) return fail(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                    requirements.size, index};
    result = vkAllocateMemory(device_, &allocation, nullptr, &quad_memory_);
    if (result != VK_SUCCESS) return fail(result);
    result = vkBindBufferMemory(device_, quad_, quad_memory_, 0);
    if (result != VK_SUCCESS) return fail(result);
    void* mapped = nullptr;
    result = vkMapMemory(device_, quad_memory_, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) return fail(result);
    __builtin_memcpy(mapped, kUnitQuad, sizeof(kUnitQuad));
    vkUnmapMemory(device_, quad_memory_);

    result = create_pipeline(solid_layout_, solid_vertex, solid_fragment, false, pipeline_);
    if (result != VK_SUCCESS) return fail(result);
    result = create_pipeline(blit_layout_, blit_vertex, blit_fragment, true, blit_pipeline_);
    if (result != VK_SUCCESS) return fail(result);

    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(device_, &fence, nullptr, &fence_);
    if (result != VK_SUCCESS) return fail(result);
    state_ = State::Ready; error_ = VK_SUCCESS;
    return VK_SUCCESS;
}

VkResult VulkanRenderer::begin(uint32_t background) {
    if (state_ == State::Submitted) {
        const auto result = wait(0);
        if (result != VK_SUCCESS) return result;
    }
    if (state_ != State::Ready) return VK_NOT_READY;
    auto result = vkResetCommandPool(device_, pool_, 0);
    if (result != VK_SUCCESS) { state_ = State::Poisoned; return error_ = result; }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_, &begin);
    if (result != VK_SUCCESS) { state_ = State::Poisoned; return error_ = result; }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearValue clear{}; premultiply(background, clear.color.float32);
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = pass_; pass.framebuffer = framebuffer_;
    pass.renderArea = {{0, 0}, {width_, height_}}; pass.clearValueCount = 1; pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, float(width_), float(height_), 0, 1};
    const VkRect2D whole{{0, 0}, {width_, height_}};
    vkCmdSetViewport(command_, 0, 1, &viewport);
    vkCmdSetScissor(command_, 0, 1, &whole);
    // Static state for the blit path; the atlas binding never changes.
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command_, 0, 1, &quad_, &offset);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_layout_, 0, 1,
                            &descriptor_, 0, nullptr);
    draws_ = 0; error_ = VK_SUCCESS; state_ = State::Recording;
    return VK_SUCCESS;
}

bool VulkanRenderer::sampled_quad(Rect box, Uv uv, Rect clip, uint32_t argb) {
    if (state_ != State::Recording || error_ != VK_SUCCESS) return false;
    const auto area = intersect(intersect(box, clip), {0, 0, int32_t(width_), int32_t(height_)});
    if (!area.width || !area.height || !box.width || !box.height || !(argb >> 24)) return true;
    if (draws_ == kMaxDraws) { error_ = VK_ERROR_OUT_OF_HOST_MEMORY; return false; }
    // Clip-space conversion stays here so the shader keeps no axis or extent policy.
    const float scale_x = 2.0f / float(width_), scale_y = 2.0f / float(height_);
    const float span_u = (uv.x1 - uv.x0) / float(box.width), span_v = (uv.y1 - uv.y0) / float(box.height);
    const float u0 = uv.x0 + float(area.x - box.x) * span_u, v0 = uv.y0 + float(area.y - box.y) * span_v;
    Quad quad{};
    quad.origin[0] = float(area.x) * scale_x - 1.0f;
    quad.origin[1] = 1.0f - float(area.y) * scale_y;
    quad.size[0] = float(area.width) * scale_x;
    quad.size[1] = -float(area.height) * scale_y;
    quad.uv0[0] = u0; quad.uv0[1] = v0;
    quad.uv1[0] = u0 + float(area.width) * span_u; quad.uv1[1] = v0 + float(area.height) * span_v;
    straight(argb, quad.color);
    const VkRect2D scissor{{area.x, area.y}, {uint32_t(area.width), uint32_t(area.height)}};
    vkCmdSetScissor(command_, 0, 1, &scissor);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_pipeline_);
    vkCmdPushConstants(command_, blit_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(quad), &quad);
    vkCmdDraw(command_, 6, 1, 0, 0); ++draws_;
    return true;
}

bool VulkanRenderer::rectangle(Rect box, Rect clip, uint32_t argb) {
    if (state_ != State::Recording || error_ != VK_SUCCESS) return false;
    const auto area = intersect(intersect(box, clip), {0, 0, int32_t(width_), int32_t(height_)});
    if (!area.width || !area.height || !(argb >> 24)) return true;
    if (draws_ == kMaxDraws) { error_ = VK_ERROR_OUT_OF_HOST_MEMORY; return false; }
    // Uniform color permits clipping the viewport itself. Never pass extreme
    // offscreen client coordinates as Vulkan viewport bounds.
    const VkViewport viewport{float(area.x), float(area.y), float(area.width), float(area.height), 0, 1};
    const VkRect2D scissor{{area.x, area.y}, {uint32_t(area.width), uint32_t(area.height)}};
    vkCmdSetViewport(command_, 0, 1, &viewport);
    vkCmdSetScissor(command_, 0, 1, &scissor);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    float color[4]; premultiply(argb, color);
    vkCmdPushConstants(command_, solid_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(color), color);
    vkCmdDraw(command_, 3, 1, 0, 0); ++draws_;
    return true;
}

bool VulkanRenderer::text(Rect box, Rect clip, const Text& value, uint32_t argb) {
    if (state_ != State::Recording || error_ != VK_SUCCESS) return false;
    clip = intersect(clip, box);
    for (uint32_t i = 0; i < value.size; ++i) {
        auto ch = uint8_t(value.bytes[i]);
        if (ch < kGlyphFirst || ch > kGlyphLast) ch = '?';
        const auto cell = ch - kGlyphFirst;
        const Uv uv{float(cell % kAtlasColumns) * kCellWidth / kAtlasWidth,
                    float(cell / kAtlasColumns) * kCellHeight / kAtlasHeight,
                    float(cell % kAtlasColumns + 1) * kCellWidth / kAtlasWidth,
                    float(cell / kAtlasColumns + 1) * kCellHeight / kAtlasHeight};
        if (!sampled_quad({add(box.x, int32_t(i * kCellWidth)), box.y, int32_t(kCellWidth),
                           int32_t(kCellHeight)}, uv, clip, argb)) return false;
    }
    return true;
}

VkResult VulkanRenderer::submit() {
    if (state_ != State::Recording) return VK_NOT_READY;
    if (error_ != VK_SUCCESS) return error_;
    vkCmdEndRenderPass(command_);
    auto result = vkEndCommandBuffer(command_);
    if (result == VK_SUCCESS) result = vkResetFences(device_, 1, &fence_);
    if (result != VK_SUCCESS) { state_ = State::Poisoned; return error_ = result; }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    // Conservatively retain ownership even when submission returns an error.
    in_flight_ = true;
    result = vkQueueSubmit(queue_, 1, &submit, fence_);
    state_ = result == VK_SUCCESS ? State::Submitted : State::Poisoned;
    return error_ = result;
}

VkResult VulkanRenderer::wait(uint64_t timeout_ns) {
    if (state_ == State::Poisoned) return error_;
    if (!in_flight_) return state_ == State::Ready ? VK_SUCCESS : VK_NOT_READY;
    const auto result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
    if (result == VK_SUCCESS) { in_flight_ = false; state_ = State::Ready; }
    else if (result != VK_TIMEOUT && result != VK_NOT_READY) { state_ = State::Poisoned; error_ = result; }
    return result;
}

VkResult VulkanRenderer::cancel() {
    if (state_ != State::Recording || in_flight_) return VK_NOT_READY;
    const auto result = vkResetCommandPool(device_, pool_, 0);
    state_ = result == VK_SUCCESS ? State::Ready : State::Poisoned;
    return error_ = result;
}

VkResult VulkanRenderer::destroy(uint64_t timeout_ns) {
    if (in_flight_) {
        const auto result = wait(timeout_ns);
        if (result != VK_SUCCESS) return result;
    }
    if (device_) {
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (quad_memory_) vkFreeMemory(device_, quad_memory_, nullptr);
        if (quad_) vkDestroyBuffer(device_, quad_, nullptr);
        if (staging_memory_) vkFreeMemory(device_, staging_memory_, nullptr);
        if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        if (atlas_view_) vkDestroyImageView(device_, atlas_view_, nullptr);
        if (atlas_) vkDestroyImage(device_, atlas_, nullptr);
        if (atlas_memory_) vkFreeMemory(device_, atlas_memory_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (blit_pipeline_) vkDestroyPipeline(device_, blit_pipeline_, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (blit_layout_) vkDestroyPipelineLayout(device_, blit_layout_, nullptr);
        if (solid_layout_) vkDestroyPipelineLayout(device_, solid_layout_, nullptr);
        if (descriptor_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
        if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        if (pass_) vkDestroyRenderPass(device_, pass_, nullptr);
    }
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE; queue_ = VK_NULL_HANDLE; image_ = VK_NULL_HANDLE; view_ = VK_NULL_HANDLE;
    pass_ = VK_NULL_HANDLE; framebuffer_ = VK_NULL_HANDLE;
    solid_layout_ = VK_NULL_HANDLE; blit_layout_ = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE; descriptor_pool_ = VK_NULL_HANDLE; descriptor_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE; atlas_ = VK_NULL_HANDLE; atlas_view_ = VK_NULL_HANDLE;
    atlas_memory_ = VK_NULL_HANDLE; staging_ = VK_NULL_HANDLE; staging_memory_ = VK_NULL_HANDLE;
    quad_ = VK_NULL_HANDLE; quad_memory_ = VK_NULL_HANDLE;
    pipeline_ = blit_pipeline_ = VK_NULL_HANDLE; pool_ = VK_NULL_HANDLE; command_ = VK_NULL_HANDLE; fence_ = VK_NULL_HANDLE;
    width_ = height_ = draws_ = 0; state_ = State::Empty; error_ = VK_SUCCESS;
    return VK_SUCCESS;
}
} // namespace ame::ui
