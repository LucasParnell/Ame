/* SPDX-License-Identifier: MIT */
/* Public Vulkan pixel tests for LOAD, sampling, clears, compute and blending.
 * --record-only compiles/records with the DRM shim and does not execute jobs.
 */
#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifdef AME_NATIVE_VULKAN
#include "native_renderer_shim.h"
#else
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#endif
#ifdef AME_NATIVE_VULKAN
#include "midix/render/vulkan_probe_spirv.h"
#else
#include "probe-shaders.h"
#endif
#include "suite.h"

#define FUNCTIONS(X) \
 X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) \
 X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceImageFormatProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
 X(CreateDevice) X(DestroyDevice) X(DeviceWaitIdle) X(CreateImage) X(DestroyImage) \
 X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) \
 X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass) \
 X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdClearAttachments) X(CmdPipelineBarrier) \
 X(GetPhysicalDeviceMemoryProperties) X(CreateBuffer) X(DestroyBuffer) \
 X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
 X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateSampler) X(DestroySampler) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(CmdResetQueryPool) \
 X(CmdBeginQuery) X(CmdEndQuery) X(CmdCopyQueryPoolResults) X(CreateComputePipelines) X(DestroyPipeline) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(CmdDispatchIndirect) \
 X(CmdCopyBuffer) X(CmdCopyImageToBuffer) X(CmdClearColorImage) X(CmdBindIndexBuffer) X(CmdBindVertexBuffers) X(CmdDrawIndexed) X(CmdPushConstants) X(CmdDraw) X(CreateGraphicsPipelines) X(GetDeviceQueue) X(CreateFence) X(DestroyFence) X(ResetFences) X(QueueSubmit) X(WaitForFences)
#define DECLARE(name) PFN_vk##name name = NULL;
#define CHECK(call) do { VkResult r=(call); if (r!=VK_SUCCESS) { \
    fprintf(stderr,"%s: VkResult=%d\n",#call,r); goto done; } } while (0)

static int run_case(const char *icd, int record_only, VkFormat format,
                    VkSampleCountFlagBits samples, VkImageAspectFlags aspects,
                    VkImageAspectFlags probe_aspect, int blend_case) {
    int success=0;
    if (getenv("PANVK_SUITE_TRACE")) {
        setenv("PANVK_DEBUG", "startup,trace_fault", 1);
        setenv("PANDECODE_DUMP_FILE", "stderr", 1);
    }
    VkInstance instance=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkImage images[2]={0};
    VkDeviceMemory memory[2]={0};
    VkImageView views[2]={0};
    VkRenderPass passes[2]={0};
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkBuffer output=VK_NULL_HANDLE;
    VkBuffer indices=VK_NULL_HANDLE;
    VkDeviceMemory index_memory=VK_NULL_HANDLE;
    VkDeviceMemory output_memory=VK_NULL_HANDLE;
    VkImageView sample_view=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool=VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout=VK_NULL_HANDLE;
    VkShaderModule shader=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule storage_shader=VK_NULL_HANDLE;
    VkPipeline storage_pipeline=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkQueryPool query_pool=VK_NULL_HANDLE;
    VkPipeline graphics=VK_NULL_HANDLE;
    VkShaderModule gfx_modules[2]={0};
    uint32_t *mapped=NULL;
    FUNCTIONS(DECLARE)
    void *library=dlopen(icd,RTLD_NOW|RTLD_LOCAL);
    if (!library) { fprintf(stderr,"%s\n",dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(library,"vk_icdGetInstanceProcAddr");
    if (!get) get=(PFN_vkGetInstanceProcAddr)dlsym(library,"vkGetInstanceProcAddr");
    if (!get) goto done;
    CreateInstance=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");
    if (!CreateInstance) goto done;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo inst={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(CreateInstance(&inst,NULL,&instance));
#define LOAD(name) name=(PFN_vk##name)get(instance,"vk" #name); if (!name) goto done;
    FUNCTIONS(LOAD)
#undef LOAD
    uint32_t count=64;
    VkPhysicalDevice physicals[64];
    CHECK(EnumeratePhysicalDevices(instance,&count,physicals));
    if (!count) goto done;
    VkPhysicalDevice physical=physicals[0];
    VkPhysicalDeviceProperties props;
    GetPhysicalDeviceProperties(physical,&props);
    printf("panvk probe: device=%s format=%d samples=%u aspect=%u\n",props.deviceName,format,samples,probe_aspect);
    VkImageFormatProperties format_props;
    int storage_case=blend_case>=12 && blend_case<=14;
    int copy_probe=blend_case==35;
    int robust_index=blend_case==33 || blend_case==34;
    int indexed=(blend_case>=15 && blend_case<=18) || blend_case==32 || robust_index;
    int indirect=blend_case>=19 && blend_case<=21;
    int vertex_inputs=blend_case==17 || blend_case==18 || blend_case==32 || robust_index;
    int query_case=blend_case>=23 && blend_case<=26;
    int layered=blend_case>=27 && blend_case<=32;
    uint32_t framebuffer_layers=blend_case==31 ? 1u : layered ? (blend_case==27 || blend_case==30 ? 4u : 2u) : 1u;
    uint32_t view_layers=blend_case==31 ? 4u : framebuffer_layers;
    uint32_t image_layers=layered ? (blend_case==27 || blend_case==30 || blend_case==31 ? 6u : blend_case==29 ? 3u : 4u) : 1u;
    uint32_t base_mip=blend_case==28 ? 1u : 0u;
    uint32_t pixel_bytes=64*64*samples*image_layers*16, pixel_words=pixel_bytes/4;
    VkImageUsageFlags image_usage=VK_IMAGE_USAGE_SAMPLED_BIT |
        (aspects==VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (storage_case) image_usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    if (copy_probe) image_usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (layered) image_usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageTiling tiling=blend_case==-3 ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    VkResult supported=GetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,
        tiling,image_usage,0,&format_props);
    if (supported==VK_ERROR_FORMAT_NOT_SUPPORTED || (supported==VK_SUCCESS && !(format_props.sampleCounts&samples))) {
        printf("panvk probe: SKIP unsupported format=%d samples=%u\n",format,samples);
        success=2;goto done;
    }
    CHECK(supported);
    VkQueueFamilyProperties families[16];
    count=16;
    GetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    if (count>16) goto done;
    uint32_t family=0;
    while (family<count && !(((families[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)))) family++;
    if (family==count) goto done;
    float priority=1;
    VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dev={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue};
    VkPhysicalDeviceFeatures enabled_features={0};
    if (robust_index) {
        VkPhysicalDeviceFeatures available;
        GetPhysicalDeviceFeatures(physical,&available);
        if (!available.robustBufferAccess) {fprintf(stderr,"panvk probe: mandatory robustBufferAccess unavailable\n");goto done;}
        enabled_features.robustBufferAccess=VK_TRUE;
        dev.pEnabledFeatures=&enabled_features;
    }
    CHECK(CreateDevice(physical,&dev,NULL,&device));
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64u<<base_mip,64u<<base_mip,1},.mipLevels=1+base_mip,.arrayLayers=image_layers,
        .samples=samples,.tiling=tiling,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    for (unsigned i=0;i<2;i++) {
        image.format=i==0 ? format : VK_FORMAT_R8G8B8A8_UNORM;
        image.usage=(i==0 && aspects!=VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (i==0 && storage_case) image.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
        if (i==0 && copy_probe) image.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (layered) image.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(CreateImage(device,&image,NULL,&images[i]));
        VkMemoryRequirements req;
        GetImageMemoryRequirements(device,images[i],&req);
        if (!req.memoryTypeBits) goto done;
        VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=req.size,.memoryTypeIndex=(uint32_t)__builtin_ctz(req.memoryTypeBits)};
        CHECK(AllocateMemory(device,&alloc,NULL,&memory[i]));
        CHECK(BindImageMemory(device,images[i],memory[i],0));
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images[i],
            .viewType=layered ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,.format=image.format,
            .subresourceRange={i==0 ? aspects : VK_IMAGE_ASPECT_COLOR_BIT,base_mip,1,layered ? 1u : 0u,view_layers}};
        CHECK(CreateImageView(device,&view,NULL,&views[i]));
    }
    VkAttachmentReference refs[2]={{0,VK_IMAGE_LAYOUT_GENERAL},{1,VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=aspects==VK_IMAGE_ASPECT_COLOR_BIT && blend_case==-2 ? 2 : 1,
        .pColorAttachments=aspects==VK_IMAGE_ASPECT_COLOR_BIT ? refs : &refs[1],
        .pDepthStencilAttachment=aspects==VK_IMAGE_ASPECT_COLOR_BIT ? NULL : &refs[0]};
    VkAttachmentDescription attachments[2];
    for (unsigned i=0;i<2;i++) attachments[i]=(VkAttachmentDescription){
        .format=i==0 ? format : VK_FORMAT_R8G8B8A8_UNORM,.samples=samples,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=i==0 && (aspects&VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_GENERAL,
        .finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkRenderPassCreateInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&subpass};
    CHECK(CreateRenderPass(device,&pass,NULL,&passes[1]));
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) attachments[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    CHECK(CreateRenderPass(device,&pass,NULL,&passes[0]));
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=passes[0],.attachmentCount=2,.pAttachments=views,.width=64,.height=64,.layers=framebuffer_layers};
    CHECK(CreateFramebuffer(device,&fb,NULL,&framebuffer));
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(CreateCommandPool(device,&cp,NULL,&pool));
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(AllocateCommandBuffers(device,&ca,&command));
    VkImageViewCreateInfo sv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images[0],
        .viewType=layered ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={probe_aspect,base_mip,1,0,image_layers}};
    CHECK(CreateImageView(device,&sv,NULL,&sample_view));
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=pixel_bytes+128,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(CreateBuffer(device,&bi,NULL,&output));
    VkMemoryRequirements req;
    GetBufferMemoryRequirements(device,output,&req);
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(physical,&mp);
    uint32_t type=0;
    while (type<mp.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) ||
           !(mp.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) type++;
    if (type==mp.memoryTypeCount) goto done;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    CHECK(AllocateMemory(device,&ma,NULL,&output_memory));
    CHECK(BindBufferMemory(device,output,output_memory,0));
    CHECK(MapMemory(device,output_memory,0,VK_WHOLE_SIZE,0,(void **)&mapped));
    memset(mapped,0xcc,bi.size);
    if (indexed || indirect) {
        uint32_t *staging=mapped+pixel_words;
        const uint16_t i16[4]={0xffff,4,2,3};
        const uint32_t i32[8]={0xdeadbeef,2,0,1};
        const uint32_t restart_indices[8]={0xdeadbeef,0,1,2,UINT32_MAX,2,1,0};
        const float positions[6]={-1,-1,3,-1,-1,3};
        memcpy(staging,blend_case==15 ? (const void *)i16 : blend_case==18 ? (const void *)restart_indices : (const void *)i32,blend_case==15 ? sizeof(i16) : sizeof(i32));
        if (blend_case==17 || blend_case==32 || robust_index) {const uint32_t vbo_indices[4]={0xdeadbeef,4,2,3};memcpy(staging,vbo_indices,sizeof(vbo_indices));}
        memcpy(staging+16,positions,sizeof(positions));
        if (indirect) {
            const uint32_t dimensions[3]={blend_case==20 ? 0u : 8u,8,blend_case==21 ? 0u : 1u};
            memcpy(staging,dimensions,sizeof(dimensions));
        }
        VkBufferCreateInfo index_bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=128,
            .usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
        CHECK(CreateBuffer(device,&index_bi,NULL,&indices));
        VkMemoryRequirements index_req;
        GetBufferMemoryRequirements(device,indices,&index_req);
        VkMemoryAllocateInfo index_alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=index_req.size,.memoryTypeIndex=(uint32_t)__builtin_ctz(index_req.memoryTypeBits)};
        CHECK(AllocateMemory(device,&index_alloc,NULL,&index_memory));
        CHECK(BindBufferMemory(device,indices,index_memory,0));
    }
    VkMappedMemoryRange mr={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=output_memory,.size=VK_WHOLE_SIZE};
    CHECK(FlushMappedMemoryRanges(device,1,&mr));
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.maxLod=0};
    CHECK(CreateSampler(device,&si,NULL,&sampler));
    VkDescriptorSetLayoutBinding bindings[3]={
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo sli={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=3,.pBindings=bindings};
    CHECK(CreateDescriptorSetLayout(device,&sli,NULL,&set_layout));
    VkDescriptorPoolSize sizes[3]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=3,.pPoolSizes=sizes};
    CHECK(CreateDescriptorPool(device,&dpi,NULL,&descriptor_pool));
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo sai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    CHECK(AllocateDescriptorSets(device,&sai,&set));
    VkDescriptorImageInfo di={sampler,sample_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo db={output,0,bi.size};
    VkWriteDescriptorSet writes[2]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&di},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db}};
    UpdateDescriptorSets(device,2,writes,0,NULL);
    if (storage_case) {
        VkDescriptorImageInfo storage_di={VK_NULL_HANDLE,sample_view,VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet storage_write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=set,.dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&storage_di};
        UpdateDescriptorSets(device,1,&storage_write,0,NULL);
    }
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    CHECK(CreatePipelineLayout(device,&pli,NULL,&pipeline_layout));
    int integer=probe_aspect==VK_IMAGE_ASPECT_STENCIL_BIT || (format==VK_FORMAT_R8G8B8A8_UINT || format==VK_FORMAT_R32_UINT);
    const uint32_t *code=integer ? (samples>1 ? probe_uint_ms : probe_uint) :
                                   (samples>1 ? probe_float_ms : probe_float);
    size_t code_size=integer ? (samples>1 ? sizeof(probe_uint_ms) : sizeof(probe_uint)) :
                              (samples>1 ? sizeof(probe_float_ms) : sizeof(probe_float));
    if (layered) {
        code=samples>1 ? probe_float_ms_array : probe_float_array;
        code_size=samples>1 ? sizeof(probe_float_ms_array) : sizeof(probe_float_array);
    }
    VkShaderModuleCreateInfo smi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=code_size,.pCode=code};
    CHECK(CreateShaderModule(device,&smi,NULL,&shader));
    uint32_t specialization[2]={samples,image_layers};
    VkSpecializationMapEntry map[2]={{0,0,4},{1,4,4}};
    VkSpecializationInfo spec={2,map,sizeof(specialization),specialization};
    VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
                .module=shader,.pName="main",.pSpecializationInfo=&spec},.layout=pipeline_layout};
    CHECK(CreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
    if (storage_case) {
        VkShaderModuleCreateInfo storage_mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=blend_case==14 ? sizeof(probe_storage_atomic) : blend_case==12 ? sizeof(probe_storage_float) : sizeof(probe_storage_unorm),
            .pCode=blend_case==14 ? probe_storage_atomic : blend_case==12 ? probe_storage_float : probe_storage_unorm};
        CHECK(CreateShaderModule(device,&storage_mi,NULL,&storage_shader));
        pi.stage.module=storage_shader;
        CHECK(CreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&storage_pipeline));
    }
    if (blend_case>=0) {
        VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vertex_inputs ? sizeof(probe_vertex_inputs) : sizeof(probe_vertex),
            .pCode=vertex_inputs ? probe_vertex_inputs : probe_vertex};
        CHECK(CreateShaderModule(device,&mi,NULL,&gfx_modules[0]));
        mi.codeSize=integer ? sizeof(probe_fragment_uint) : sizeof(probe_fragment);
        mi.pCode=integer ? probe_fragment_uint : probe_fragment;
        CHECK(CreateShaderModule(device,&mi,NULL,&gfx_modules[1]));
        VkPipelineShaderStageCreateInfo stages[2]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=gfx_modules[0],.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=gfx_modules[1],.pName="main"}};
        VkVertexInputBindingDescription gfx_binding={0,8,VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription gfx_attribute={0,0,VK_FORMAT_R32G32_SFLOAT,0};
        VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount=vertex_inputs ? 1 : 0,.pVertexBindingDescriptions=&gfx_binding,
            .vertexAttributeDescriptionCount=vertex_inputs ? 1 : 0,.pVertexAttributeDescriptions=&gfx_attribute};
        VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=blend_case==18 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable=blend_case==18};
        VkViewport viewport={0,0,64,64,0,1};
        VkRect2D scissor={{28,28},{8,8}};
        VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
        VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
        VkSampleMask coverage_mask=blend_case==22 || blend_case==29 ? 5u : ~0u;
        VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=samples,.pSampleMask=&coverage_mask};
        VkPipelineColorBlendAttachmentState ba={.colorWriteMask=0xf,.srcColorBlendFactor=VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor=VK_BLEND_FACTOR_ZERO,.colorBlendOp=VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO,.alphaBlendOp=VK_BLEND_OP_ADD};
        if (blend_case>=1 && blend_case<=6) ba.blendEnable=VK_TRUE;
        if (blend_case==1) {ba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;}
        if (blend_case==2) {ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;}
        if (blend_case==3) {ba.dstColorBlendFactor=ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ba.colorBlendOp=ba.alphaBlendOp=VK_BLEND_OP_REVERSE_SUBTRACT;}
        if (blend_case==4) ba.colorBlendOp=ba.alphaBlendOp=VK_BLEND_OP_MIN;
        if (blend_case==5) ba.colorBlendOp=ba.alphaBlendOp=VK_BLEND_OP_MAX;
        if (blend_case==6) {
            ba.srcColorBlendFactor=VK_BLEND_FACTOR_CONSTANT_COLOR;ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            ba.srcAlphaBlendFactor=VK_BLEND_FACTOR_CONSTANT_ALPHA;ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        }
        if (blend_case==8) ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT;
        VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&ba,.blendConstants={.25f,.5f,.75f,.125f}};
        VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,
            .pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&cb,
            .layout=pipeline_layout,.renderPass=passes[0]};
        CHECK(CreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,NULL,&graphics));
    }
    if (query_case) {
        VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=3};
        CHECK(CreateQueryPool(device,&qi,NULL,&query_pool));
        if (!query_pool) {fprintf(stderr,"panvk probe: CreateQueryPool returned a null pool\n");goto done;}
    }
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(BeginCommandBuffer(command,&begin));
    if (query_case) CmdResetQueryPool(command,query_pool,0,3);
    if (indexed || indirect) {
        VkBufferCopy copy={pixel_bytes,0,128};
        CmdCopyBuffer(command,output,indices,1,&copy);
        VkMemoryBarrier index_barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_INDEX_READ_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
        CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&index_barrier,0,NULL,0,NULL);
    }
    VkImageMemoryBarrier ib[2];
    for (unsigned i=0;i<2;i++) ib[i]=(VkImageMemoryBarrier){
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=images[i],.subresourceRange={i==0 ? aspects : VK_IMAGE_ASPECT_COLOR_BIT,0,1+base_mip,0,image_layers}};
    VkPipelineStageFlags attachment_stages=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    if (layered) for (unsigned i=0;i<2;i++) ib[i].dstAccessMask|=VK_ACCESS_TRANSFER_WRITE_BIT;
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,attachment_stages|(layered ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0),0,0,NULL,0,NULL,2,ib);
    if (layered) {
        VkClearColorValue canary={{.0625f,.125f,.75f,1}};
        VkImageSubresourceRange all={VK_IMAGE_ASPECT_COLOR_BIT,0,1+base_mip,0,image_layers};
        CmdClearColorImage(command,images[0],VK_IMAGE_LAYOUT_GENERAL,&canary,1,&all);
        VkMemoryBarrier cleared={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,attachment_stages,0,1,&cleared,0,NULL,0,NULL);
    }
    VkClearValue clear[2]={{{{.125f,.25f,.5f,1}}},{{{0,0,0,1}}}};
    if (integer && aspects==VK_IMAGE_ASPECT_COLOR_BIT) {clear[0].color.uint32[0]=1;clear[0].color.uint32[1]=2;clear[0].color.uint32[2]=3;clear[0].color.uint32[3]=4;}
    if (aspects!=VK_IMAGE_ASPECT_COLOR_BIT) clear[0].depthStencil=(VkClearDepthStencilValue){.25f,0x5a};
    VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=passes[1],
        .framebuffer=framebuffer,.renderArea={{0,0},{64,64}},.clearValueCount=2,.pClearValues=clear};
    if (blend_case==30) rp.renderArea=(VkRect2D){{4,5},{48,46}};
    CmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
    CmdEndRenderPass(command);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    CmdPipelineBarrier(command,attachment_stages,attachment_stages,0,1,&mb,0,NULL,0,NULL);
    rp.renderPass=passes[0];
    CmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment patch={.aspectMask=aspects,.colorAttachment=0,.clearValue={.color={{.8f,.4f,.2f,1}}}};
    if (aspects!=VK_IMAGE_ASPECT_COLOR_BIT) patch.clearValue.depthStencil=(VkClearDepthStencilValue){.75f,0xc3};
    VkClearRect rect={{{28,28},{8,8}},0,1};
    if (blend_case<0) CmdClearAttachments(command,1,&patch,1,&rect);
    else {
        CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics);
        const float color[4]={.8f,.2f,.4f,.5f};
        const uint32_t uint_color[4]={17,34,51,68};
        CmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,integer ? (const void *)uint_color : (const void *)color);
        if (query_case) CmdBeginQuery(command,query_pool,0,0);
        if (blend_case==24) {
            /* An empty query must become available with a zero result. */
        } else if (blend_case==25) {
            /* Meta clear draws must not contribute to the active query. */
            CmdClearAttachments(command,1,&patch,1,&rect);
        } else if (blend_case==26) {
            CmdDraw(command,0,1,0,0);
        } else if (indexed) {
            CmdBindIndexBuffer(command,indices,0,blend_case==15 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            if (vertex_inputs) {VkDeviceSize offset=64;CmdBindVertexBuffers(command,0,1,&indices,&offset);}
            CmdDrawIndexed(command,blend_case==18 ? 7 : 3,blend_case==16 ? 2 : 1,1,
                blend_case==15 || blend_case==17 || blend_case==32 || robust_index ? -2 : 0,blend_case==16 ? 3 : 0);
        } else CmdDraw(command,3,1,0,0);
        if (query_case) {
            CmdEndQuery(command,query_pool,0);
            CmdBeginQuery(command,query_pool,1,0);
            CmdEndQuery(command,query_pool,1);
        }
    }
    if (layered && framebuffer_layers>1) {
        VkClearRect slice={{{12,40},{6,9}},1,1};
        CmdClearAttachments(command,1,&patch,1,&slice);
    }
    CmdEndRenderPass(command);
    if (layered) {
        CmdPipelineBarrier(command,attachment_stages,attachment_stages,0,1,&mb,0,NULL,0,NULL);
        CmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
        VkClearRect point={{{blend_case==30 ? 8 : 0,blend_case==30 ? 8 : 0},{1,1}},0,framebuffer_layers};
        CmdClearAttachments(command,1,&patch,1,&point);
        CmdEndRenderPass(command);
    }
    if (blend_case==22) {
        /* A third pass must reload samples with different stored values. */
        CmdPipelineBarrier(command,attachment_stages,attachment_stages,0,1,&mb,0,NULL,0,NULL);
        CmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
        VkClearRect point={{{0,0},{1,1}},0,1};
        CmdClearAttachments(command,1,&patch,1,&point);
        CmdEndRenderPass(command);
    }
    if (copy_probe) {
        ib[0].oldLayout=VK_IMAGE_LAYOUT_GENERAL;
        ib[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib[0].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        ib[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        CmdPipelineBarrier(command,attachment_stages,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,ib);
        VkBufferImageCopy region={.bufferOffset=pixel_bytes+16,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .imageOffset={30,30,0},.imageExtent={4,4,1}};
        CmdCopyImageToBuffer(command,images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output,1,&region);
        ib[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        ib[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        ib[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib[0].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,ib);
    }
    ib[0].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|(layered ? VK_ACCESS_TRANSFER_WRITE_BIT : 0);
    ib[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    ib[0].oldLayout=copy_probe ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    ib[0].newLayout=storage_case ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (storage_case) ib[0].dstAccessMask|=VK_ACCESS_SHADER_WRITE_BIT;
    CmdPipelineBarrier(command,attachment_stages|(layered ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0),VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,ib);
    if (storage_case) {
        CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,storage_pipeline);
        CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL);
        CmdDispatch(command,8,8,1);
        ib[0].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        ib[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        ib[0].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,ib);
    }
    CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL);
    if (indirect) CmdDispatchIndirect(command,indices,0);
    else CmdDispatch(command,8,8,image_layers);
    if (query_case) {
        CmdCopyQueryPoolResults(command,query_pool,0,2,output,pixel_bytes+32,16,
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        CmdCopyQueryPoolResults(command,query_pool,2,1,output,pixel_bytes+80,8,
            VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        CmdCopyQueryPoolResults(command,query_pool,2,1,output,pixel_bytes+96,8,
            VK_QUERY_RESULT_PARTIAL_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    }
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT | ((query_case || copy_probe) ? VK_ACCESS_TRANSFER_WRITE_BIT : 0);
    mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | ((query_case || copy_probe) ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0),VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);
    CHECK(EndCommandBuffer(command));
    if (record_only) {
        printf("panvk probe: recorded format=%d samples=%u aspect=%u; no GPU execution\n",format,samples,probe_aspect);
    } else {
        VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CHECK(CreateFence(device,&fi,NULL,&fence));
        VkQueue gpu_queue;
        GetDeviceQueue(device,family,0,&gpu_queue);
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
        for (unsigned round=0;round<((indexed || indirect) ? 2u : 1u);round++) {
        int null_dispatch=indirect && ((round==0 && blend_case!=19) || (round==1 && blend_case==19));
        if (round) {
            CHECK(ResetFences(device,1,&fence));
            if (indexed) {
                uint32_t *staging=mapped+pixel_words;
                if (blend_case==15) {
                    const uint16_t degenerate[4]={0xffff,2,2,2};
                    memcpy(staging,degenerate,sizeof(degenerate));
                } else {
                    const uint32_t degenerate[8]={0xdeadbeef,blend_case==17 || blend_case==32 || robust_index ? 2u : 0u,
                        blend_case==17 || blend_case==32 || robust_index ? 2u : 0u,blend_case==17 || blend_case==32 || robust_index ? 2u : 0u,UINT32_MAX,0,0,0};
                    memcpy(staging,degenerate,sizeof(degenerate));
                    if (robust_index) {
                        /* Values become either negative or overflow a 32-bit
                         * byte product after adding baseVertex=-2. */
                        const uint32_t invalid[4]={0xdeadbeef,
                            blend_case==33 ? 0x20000002u : 0u,
                            blend_case==33 ? 0x20000003u : 1u,
                            blend_case==33 ? 0x20000004u : 0u};
                        memcpy(staging,invalid,sizeof(invalid));
                    }
                }
                CHECK(FlushMappedMemoryRanges(device,1,&mr));
            }
            if (indirect) {
                memset(mapped,0xcc,pixel_bytes);
                const uint32_t dimensions[3]={8,8,null_dispatch ? 0u : 1u};
                memcpy(mapped+pixel_words,dimensions,sizeof(dimensions));
                CHECK(FlushMappedMemoryRanges(device,1,&mr));
            }
        }
        CHECK(QueueSubmit(gpu_queue,1,&submit,fence));
        CHECK(WaitForFences(device,1,&fence,VK_TRUE,UINT64_C(5000000000)));
        CHECK(InvalidateMappedMemoryRanges(device,1,&mr));
        for (unsigned layer=0;layer<image_layers;layer++) for (unsigned y=0;y<64;y++) for (unsigned x=0;x<64;x++) for (unsigned sample=0;sample<samples;sample++) {
            unsigned offset=(((layer*64+y)*64+x)*samples+sample)*4;
            int inside=x>=28 && x<36 && y>=28 && y<36;
            if ((blend_case==22 || blend_case==29) && (sample&1)) inside=0;
            if (layered && layer!=1) inside=0;
            if (indexed && round) inside=0;
            if (blend_case==24 || blend_case==26) inside=0;
            for (unsigned c=0;c<(probe_aspect==VK_IMAGE_ASPECT_COLOR_BIT ? 4u : 1u);c++) {
                uint32_t value=mapped[offset+c];
                if (null_dispatch) {
                    if (value!=0xccccccccu) {fprintf(stderr,"panvk probe: zero indirect dispatch wrote at %u,%u\n",x,y);goto done;}
                    continue;
                }
                if (integer) {
                    uint32_t expected_int=probe_aspect==VK_IMAGE_ASPECT_STENCIL_BIT ? (inside ? 0xc3u : 0x5au) :
                                          inside ? (c+1)*17 : c+1;
                    if (blend_case==14) expected_int=c==0 ? (inside ? 70 : 1) : c==3 ? 1 : 0;
                    if (value!=expected_int) {
                        fprintf(stderr,"panvk probe: stencil mismatch at %u,%u sample %u: %u\n",x,y,sample,value); goto done;
                    }
                } else {
                    float actual;memcpy(&actual,&value,sizeof(actual));
                    const float old_color[4]={32/255.f,64/255.f,128/255.f,1};
                    const float new_color[4]={204/255.f,102/255.f,51/255.f,1};
                    float expected=probe_aspect==VK_IMAGE_ASPECT_DEPTH_BIT ? (inside ? .75f : .25f) :
                                   inside ? new_color[c] : old_color[c];
                    float tolerance=probe_aspect==VK_IMAGE_ASPECT_DEPTH_BIT ? 2/65535.f : .001f;
                    if (blend_case>=0) {
                        float old[4]={32/255.f,64/255.f,128/255.f,1}, source[4]={.8f,.2f,.4f,.5f};
                        const float constants[4]={.25f,.5f,.75f,.125f};
                        if (format!=VK_FORMAT_R8G8B8A8_UNORM) {old[0]=.125f;old[1]=.25f;old[2]=.5f;}
                        expected=old[c];
                        if (inside && !(blend_case==8 && c!=0)) {
                            expected=source[c];
                            if (storage_case) expected+=.05f;
                            if (blend_case==1 && c<3) expected=source[c]*.5f+old[c]*.5f;
                            if (blend_case==2) expected=source[c]+old[c];
                            if (blend_case==3) expected=old[c]-source[c];
                            if (blend_case==4) expected=fminf(old[c],source[c]);
                            if (blend_case==5) expected=fmaxf(old[c],source[c]);
                            if (blend_case==6) expected=source[c]*constants[c]+old[c]*(1-constants[c]);
                        }
                        if (format==VK_FORMAT_R8G8B8A8_UNORM) {expected=fminf(fmaxf(expected,0),1);tolerance=2/255.f;}
                    }
                    /* Robust access may return another value from the bound
                     * allocation. It need not return zero. Arbitrary vertex
                     * positions can cover any part of the draw scissor; the
                     * only possible fragment outputs here are source or clear. */
                    if (robust_index && round && x>=28 && x<36 && y>=28 && y<36) {
                        const float source[4]={204/255.f,51/255.f,102/255.f,128/255.f};
                        if (fabsf(actual-source[c])<=tolerance) expected=source[c];
                    }
                    if (blend_case==25 && inside) expected=new_color[c];
                    if (blend_case==22 && x==0 && y==0) {expected=new_color[c];tolerance=2/255.f;}
                    if (layered) {
                        const float canary[4]={16/255.f,32/255.f,191/255.f,1};
                        if (layer==0 || layer>=1+framebuffer_layers ||
                            (blend_case==30 && (x<4 || x>=52 || y<5 || y>=51))) expected=canary[c];
                        if (framebuffer_layers>1 && layer==2 && x>=12 && x<18 && y>=40 && y<49) expected=new_color[c];
                        if (layer>=1 && layer<1+framebuffer_layers && x==(blend_case==30 ? 8u : 0u) &&
                            y==(blend_case==30 ? 8u : 0u)) expected=new_color[c];
                    }
                    if (!isfinite(actual) || fabsf(actual-expected)>tolerance) {
                        uint32_t expected_bits;memcpy(&expected_bits,&expected,4);
                        fprintf(stderr,"panvk probe: mismatch layer %u at %u,%u sample %u component %u: bits=%08x expected=%08x\n",layer,x,y,sample,c,value,expected_bits); goto done;
                    }
                }
            }
        }
        if (copy_probe) {
            const uint32_t *tail=mapped+pixel_words;
            for (unsigned word=0;word<32;word++) {
                uint32_t expected=word>=4 && word<20 ? 0x806633ccu : 0xccccccccu;
                if (tail[word]!=expected) {fprintf(stderr,"panvk probe: image-copy word %u got=%08x expected=%08x\n",word,tail[word],expected);goto done;}
            }
        }
        if (query_case) {
            uint64_t results[4]={0};
            CHECK(GetQueryPoolResults(device,query_pool,0,2,sizeof(results),results,16,
                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
            CHECK(GetQueryPoolResults(device,query_pool,0,2,sizeof(results),results,16,
                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
            const uint32_t *tail=mapped+pixel_words;
            uint64_t copied[4];memcpy(copied,tail+8,sizeof(copied));
            for (unsigned q=0;q<2;q++) {
                int expected=q==0 && blend_case==23;
                if (!!results[q*2]!=expected || !results[q*2+1] ||
                    !!copied[q*2]!=expected || !copied[q*2+1]) {
                    fprintf(stderr,"panvk probe: query %u result=%llu available=%llu copy=%llu available=%llu expected_nonzero=%d\n",
                        q,(unsigned long long)results[q*2],(unsigned long long)results[q*2+1],
                        (unsigned long long)copied[q*2],(unsigned long long)copied[q*2+1],expected);goto done;
                }
            }
            uint32_t unavailable[2]={0xabababab,0xbbbbbbbb};
            VkResult not_ready=GetQueryPoolResults(device,query_pool,2,1,sizeof(unavailable),unavailable,8,VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            /* Reset leaves numerical results undefined, including PARTIAL reads.
             * Only availability and the API status are defined for query 2. */
            if ((not_ready!=VK_NOT_READY && not_ready!=VK_SUCCESS) || unavailable[1] || tail[21]) {
                fprintf(stderr,"panvk probe: unavailable query availability corrupted\n");goto done;
            }
            not_ready=GetQueryPoolResults(device,query_pool,2,1,sizeof(unavailable),unavailable,8,
                VK_QUERY_RESULT_PARTIAL_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if ((not_ready!=VK_NOT_READY && not_ready!=VK_SUCCESS) || unavailable[1] || tail[25]) {
                fprintf(stderr,"panvk probe: partial unavailable query mismatch\n");goto done;
            }
            printf("panvk probe: occlusion/availability/CPU+GPU result copy verified case=%d\n",blend_case);
        }
        printf("panvk probe: pixels verified format=%d samples=%u aspect=%u round=%u\n",format,samples,probe_aspect,round);
        }
    }
    success=1;
done:
    if (device && DestroyDevice) {
        if (DeviceWaitIdle) (void)DeviceWaitIdle(device);
        if (graphics) DestroyPipeline(device,graphics,NULL);
        for (unsigned i=0;i<2;i++) if (gfx_modules[i]) DestroyShaderModule(device,gfx_modules[i],NULL);
        if (query_pool) DestroyQueryPool(device,query_pool,NULL);
        if (fence) DestroyFence(device,fence,NULL);
        if (storage_pipeline) DestroyPipeline(device,storage_pipeline,NULL);
        if (storage_shader) DestroyShaderModule(device,storage_shader,NULL);
        if (pipeline) DestroyPipeline(device,pipeline,NULL);
        if (shader) DestroyShaderModule(device,shader,NULL);
        if (pipeline_layout) DestroyPipelineLayout(device,pipeline_layout,NULL);
        if (descriptor_pool) DestroyDescriptorPool(device,descriptor_pool,NULL);
        if (set_layout) DestroyDescriptorSetLayout(device,set_layout,NULL);
        if (sampler) DestroySampler(device,sampler,NULL);
        if (sample_view) DestroyImageView(device,sample_view,NULL);
        if (mapped) UnmapMemory(device,output_memory);
        if (indices) DestroyBuffer(device,indices,NULL);
        if (index_memory) FreeMemory(device,index_memory,NULL);
        if (output) DestroyBuffer(device,output,NULL);
        if (output_memory) FreeMemory(device,output_memory,NULL);
        if (pool) DestroyCommandPool(device,pool,NULL);
        if (framebuffer) DestroyFramebuffer(device,framebuffer,NULL);
        for (unsigned i=0;i<2;i++) {
            if (passes[i]) DestroyRenderPass(device,passes[i],NULL);
            if (views[i]) DestroyImageView(device,views[i],NULL);
            if (images[i]) DestroyImage(device,images[i],NULL);
            if (memory[i]) FreeMemory(device,memory[i],NULL);
        }
        DestroyDevice(device,NULL);
    }
    if (instance && DestroyInstance) DestroyInstance(instance,NULL);
    dlclose(library);
    return success==2 ? 2 : !success;
}

int panvk_suite_run(const char *icd,int record_only) {
    static const struct {VkFormat format;VkImageAspectFlags aspects,probe;} cases[]={
        {VK_FORMAT_R8G8B8A8_UNORM,1,1},
        {VK_FORMAT_D16_UNORM,2,2},
        {VK_FORMAT_D32_SFLOAT,2,2},
        {VK_FORMAT_S8_UINT,4,4},
        {VK_FORMAT_D24_UNORM_S8_UINT,6,2},
        {VK_FORMAT_D24_UNORM_S8_UINT,6,4},
        {VK_FORMAT_D32_SFLOAT_S8_UINT,6,2},
        {VK_FORMAT_D32_SFLOAT_S8_UINT,6,4},
    };
    unsigned passed=0, skipped=0;
    printf("panvk probe: linear layout control\n");
    int linear=run_case(icd,record_only,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,-3);
    if (linear==2) skipped++;
    else if (linear) return -1;
    else passed++;
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        for (unsigned j=0;j<2;j++) {
            int result=run_case(icd,record_only,cases[i].format,j ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,cases[i].aspects,cases[i].probe,-1);
            if (result==2) skipped++;
            else if (result) return -1;
            else passed++;
        }
    }
    for (int blend=0;blend<=21;blend++) {
        if (blend==7) continue; // LogicOp feature exposure is tested separately.
        VkFormat format=blend==14 ? VK_FORMAT_R32_UINT : blend==9 || blend==12 ? VK_FORMAT_R32G32B32A32_SFLOAT :
                        blend==10 ? VK_FORMAT_R16G16B16A16_SFLOAT :
                        blend==11 ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM;
        printf("panvk probe: blend case=%d\n",blend);
        int result=run_case(icd,record_only,format,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,blend);
        if (result==2) skipped++;
        else if (result) return -1;
        else passed++;
    }
    printf("panvk probe: per-sample MSAA history case\n");
    int sample_history=run_case(icd,record_only,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_4_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,22);
    if (sample_history==2) skipped++;
    else if (sample_history) return -1;
    else passed++;
    for (int query=23;query<=26;query++) {
        printf("panvk probe: query case=%d\n",query);
        int result=run_case(icd,record_only,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,query);
        if (result==2) skipped++;
        else if (result) return -1;
        else passed++;
    }
    for (int layer_case=27;layer_case<=32;layer_case++) {
        printf("panvk probe: array-layer/mip LOAD case=%d\n",layer_case);
        int result=run_case(icd,record_only,VK_FORMAT_R8G8B8A8_UNORM,
                            layer_case==29 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,layer_case);
        if (result==2) skipped++;
        else if (result) return -1;
        else passed++;
    }
    for (int robust=33;robust<=34;robust++) {
        printf("panvk probe: robust indexed VBO case=%d\n",robust);
        int result=run_case(icd,record_only,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,robust);
        if (result==2) skipped++;
        else if (result) return -1;
        else passed++;
    }
    printf("panvk probe: tiled image readback with guarded buffer offset\n");
    {
        int result=run_case(icd,record_only,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,35);
        if (result==2) skipped++;
        else if (result) return -1;
        else passed++;
    }
    printf("panvk probe: MRT LOAD case\n");
    int mrt=run_case(icd,record_only,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,-2);
    if (mrt==2) skipped++;
    else if (mrt) return -1;
    else passed++;
    printf("panvk suite: %u preload/sample/clear/compute/blend cases passed, %u skipped (%s)\n",passed,skipped,record_only ? "recording only" : "GPU pixels verified");
    return 0;
}
#ifdef PANVK_SUITE_STANDALONE
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    const char *icd="libvulkan_panfrost.so";int record_only=0;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--host")) icd="libvulkan.so.1";
        else if (!strcmp(argv[i],"--record-only")) record_only=1;
        else return 2;
    }
    return panvk_suite_run(icd,record_only) ? 1 : 0;
}
#endif
