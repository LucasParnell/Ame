/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "renderer.h"
#include "shaders.h"
#include "midix/abi/protocols/gpu_diag.h"

#define FUNCTIONS(X) \
 X(GetInstanceProcAddr) X(CreateInstance) X(DestroyInstance) \
 X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
 X(GetPhysicalDeviceFormatProperties) X(CreateDevice) X(DestroyDevice) \
 X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
 X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
 X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
 X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
 X(GetImageSubresourceLayout) X(CreateImageView) X(DestroyImageView) \
 X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdPipelineBarrier) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
 X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdDraw) \
 X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit) X(DeviceWaitIdle)

struct panvk_triangle {
    void *library;
#define DECLARE(name) PFN_vk##name name;
    FUNCTIONS(DECLARE)
#undef DECLARE
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    VkBuffer vertices;
    VkDeviceMemory vertex_memory, image_memory;
    VkImage image;
    VkImageView view;
    VkRenderPass pass;
    VkFramebuffer framebuffer;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkSubresourceLayout pixels_layout;
    void *vertex_map, *pixel_map;
    int rendered, verified;
};

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "panvk triangle: %s: VkResult=%d\n", #call, r_); goto fail; } } while (0)
#define WIDTH AME_GPU_DIAG_WIDTH
#define HEIGHT AME_GPU_DIAG_HEIGHT

static uint32_t memory_type(struct panvk_triangle *t, uint32_t bits) {
    for (uint32_t i=0; i<t->memory.memoryTypeCount; i++)
        if ((bits & (1u<<i)) && (t->memory.memoryTypes[i].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return i;
    return UINT32_MAX;
}

static int allocate(struct panvk_triangle *t, VkMemoryRequirements req,
                    VkDeviceMemory *memory, void **mapping) {
    uint32_t type=memory_type(t, req.memoryTypeBits);
    if (type==UINT32_MAX) return -1;
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size, .memoryTypeIndex=type};
    CHECK(t->AllocateMemory(t->device,&alloc,NULL,memory));
    CHECK(t->MapMemory(t->device,*memory,0,VK_WHOLE_SIZE,0,mapping));
    return 0;
fail:
    return -1;
}

struct panvk_triangle *panvk_triangle_create(const char *library, int hardware) {
    struct panvk_triangle *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->library=dlopen(library,RTLD_NOW|RTLD_LOCAL);
    if (!t->library) { fprintf(stderr,"panvk triangle: %s\n",dlerror()); goto fail; }
    t->GetInstanceProcAddr=(PFN_vkGetInstanceProcAddr)dlsym(t->library,"vk_icdGetInstanceProcAddr");
    if (!t->GetInstanceProcAddr)
        t->GetInstanceProcAddr=(PFN_vkGetInstanceProcAddr)dlsym(t->library,"vkGetInstanceProcAddr");
    if (!t->GetInstanceProcAddr) goto fail;
    t->CreateInstance=(PFN_vkCreateInstance)t->GetInstanceProcAddr(NULL,"vkCreateInstance");
    if (!t->CreateInstance) goto fail;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="Ame Vulkan GPU diagnostic",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo instance={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(t->CreateInstance(&instance,NULL,&t->instance));
#define LOAD(name) do { t->name=(PFN_vk##name)t->GetInstanceProcAddr(t->instance,"vk" #name); \
    if (!t->name) { fprintf(stderr,"panvk triangle: missing vk%s\n",#name); goto fail; } } while(0);
    FUNCTIONS(LOAD)
#undef LOAD
    uint32_t count=0;
    CHECK(t->EnumeratePhysicalDevices(t->instance,&count,NULL));
    if (!count || count>64) goto fail;
    VkPhysicalDevice devices[64];
    CHECK(t->EnumeratePhysicalDevices(t->instance,&count,devices));
    for (uint32_t i=0;i<count;i++) {
        VkPhysicalDeviceProperties properties;
        t->GetPhysicalDeviceProperties(devices[i],&properties);
        if (!hardware || (properties.vendorID==0x13b5 &&
            properties.deviceID==(0x830u<<16) && strstr(properties.deviceName,"T830"))) {
            t->physical=devices[i]; t->properties=properties; break;
        }
    }
    if (!t->physical) { fprintf(stderr,"panvk triangle: required Mali-T830 unavailable\n"); goto fail; }
    t->GetPhysicalDeviceMemoryProperties(t->physical,&t->memory);
    VkFormatProperties format;
    t->GetPhysicalDeviceFormatProperties(t->physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
    if (!(format.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        fprintf(stderr,"panvk triangle: linear RGBA8 attachment unavailable\n"); goto fail;
    }
    uint32_t families=0;
    t->GetPhysicalDeviceQueueFamilyProperties(t->physical,&families,NULL);
    if (!families || families>64) goto fail;
    VkQueueFamilyProperties queues[64];
    t->GetPhysicalDeviceQueueFamilyProperties(t->physical,&families,queues);
    uint32_t family=0;
    while (family<families && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    if (family==families) goto fail;
    float priority=1;
    VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue};
    CHECK(t->CreateDevice(t->physical,&device,NULL,&t->device));
    t->GetDeviceQueue(t->device,family,0,&t->queue);
    const float vertices[]={-.69282f,-.4f,1,0,0, .69282f,-.4f,0,1,0, 0,.8f,0,0,1};
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=sizeof(vertices),.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    CHECK(t->CreateBuffer(t->device,&buffer,NULL,&t->vertices));
    VkMemoryRequirements req;
    t->GetBufferMemoryRequirements(t->device,t->vertices,&req);
    if (allocate(t,req,&t->vertex_memory,&t->vertex_map)) goto fail;
    CHECK(t->BindBufferMemory(t->device,t->vertices,t->vertex_memory,0));
    memcpy(t->vertex_map,vertices,sizeof(vertices));
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=t->vertex_memory,.size=VK_WHOLE_SIZE};
    CHECK(t->FlushMappedMemoryRanges(t->device,1,&range));
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={WIDTH,HEIGHT,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(t->CreateImage(t->device,&image,NULL,&t->image));
    t->GetImageMemoryRequirements(t->device,t->image,&req);
    VkDeviceSize image_size=req.size;
    if (allocate(t,req,&t->image_memory,&t->pixel_map)) goto fail;
    CHECK(t->BindImageMemory(t->device,t->image,t->image_memory,0));
    VkImageSubresource sub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};
    t->GetImageSubresourceLayout(t->device,t->image,&sub,&t->pixels_layout);
    if (t->pixels_layout.rowPitch<WIDTH*4 || t->pixels_layout.offset>image_size ||
        t->pixels_layout.rowPitch> (image_size-t->pixels_layout.offset)/HEIGHT) goto fail;
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=t->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=image.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(t->CreateImageView(t->device,&view,NULL,&t->view));
    VkAttachmentDescription attachment={.format=image.format,.samples=image.samples,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&color};
    VkRenderPassCreateInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
    CHECK(t->CreateRenderPass(t->device,&pass,NULL,&t->pass));
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=t->pass,.attachmentCount=1,.pAttachments=&t->view,
        .width=WIDTH,.height=HEIGHT,.layers=1};
    CHECK(t->CreateFramebuffer(t->device,&fb,NULL,&t->framebuffer));
    VkPushConstantRange push={VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(float)};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push};
    CHECK(t->CreatePipelineLayout(t->device,&layout,NULL,&t->layout));
    VkShaderModule modules[2]={0};
    VkShaderModuleCreateInfo module={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(triangle_vert),.pCode=triangle_vert};
    VkResult module_result=t->CreateShaderModule(t->device,&module,NULL,&modules[0]);
    if (module_result!=VK_SUCCESS) goto fail;
    module.codeSize=sizeof(triangle_frag); module.pCode=triangle_frag;
    module_result=t->CreateShaderModule(t->device,&module,NULL,&modules[1]);
    if (module_result!=VK_SUCCESS) { t->DestroyShaderModule(t->device,modules[0],NULL); goto fail; }
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,
         .module=modules[0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,
         .module=modules[1],.pName="main"}};
    VkVertexInputBindingDescription binding={0,5*sizeof(float),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[]={{0,0,VK_FORMAT_R32G32_SFLOAT,0},
        {1,0,VK_FORMAT_R32G32B32_SFLOAT,2*sizeof(float)}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding,
        .vertexAttributeDescriptionCount=2,.pVertexAttributeDescriptions=attributes};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,WIDTH,HEIGHT,0,1};
    VkRect2D scissor={{0,0},{WIDTH,HEIGHT}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=0xf};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend};
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,
        .pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,
        .pColorBlendState=&cb,.layout=t->layout,.renderPass=t->pass};
    VkResult pipeline_result=t->CreateGraphicsPipelines(t->device,VK_NULL_HANDLE,1,&pipeline,NULL,&t->pipeline);
    for (unsigned i=0;i<2;i++) t->DestroyShaderModule(t->device,modules[i],NULL);
    if (pipeline_result!=VK_SUCCESS) {
        fprintf(stderr,"panvk triangle: pipeline: VkResult=%d\n",pipeline_result); goto fail;
    }
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(t->CreateCommandPool(t->device,&pool,NULL,&t->pool));
    VkCommandBufferAllocateInfo command={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=t->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(t->AllocateCommandBuffers(t->device,&command,&t->command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(t->CreateFence(t->device,&fence,NULL,&t->fence));
    return t;
fail:
    panvk_triangle_destroy(t);
    return NULL;
}

int panvk_triangle_record(struct panvk_triangle *t,float angle) {
    if (!t || !isfinite(angle)) return -1;
    CHECK(t->ResetCommandPool(t->device,t->pool,0));
    CHECK(t->ResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(t->BeginCommandBuffer(t->command,&begin));
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=t->rendered ? VK_ACCESS_HOST_READ_BIT : 0,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout=t->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    t->CmdPipelineBarrier(t->command,t->rendered ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkClearValue clear={.color={{.02f,.03f,.06f,1}}};
    VkRenderPassBeginInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass=t->pass,.framebuffer=t->framebuffer,.renderArea={{0,0},{WIDTH,HEIGHT}},
        .clearValueCount=1,.pClearValues=&clear};
    t->CmdBeginRenderPass(t->command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    t->CmdBindPipeline(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,t->pipeline);
    VkDeviceSize offset=0;
    t->CmdBindVertexBuffers(t->command,0,1,&t->vertices,&offset);
    t->CmdPushConstants(t->command,t->layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(angle),&angle);
    t->CmdDraw(t->command,3,1,0,0);
    t->CmdEndRenderPass(t->command);
    barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    t->CmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,0,NULL,1,&barrier);
    CHECK(t->EndCommandBuffer(t->command));
    return 0;
fail:
    return -1;
}

int panvk_triangle_frame(struct panvk_triangle *t,float angle,uint32_t *argb,uint32_t *checksum) {
    if (!argb || !checksum || panvk_triangle_record(t,angle)) return -1;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    CHECK(t->QueueSubmit(t->queue,1,&submit,t->fence));
    CHECK(t->WaitForFences(t->device,1,&t->fence,VK_TRUE,UINT64_C(5000000000)));
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=t->image_memory,.size=VK_WHOLE_SIZE};
    CHECK(t->InvalidateMappedMemoryRanges(t->device,1,&range));
    uint32_t hash=2166136261u;
    for (unsigned y=0;y<HEIGHT;y++) {
        const unsigned char *row=(const unsigned char *)t->pixel_map+t->pixels_layout.offset+y*t->pixels_layout.rowPitch;
        for (unsigned x=0;x<WIDTH;x++) {
            uint32_t pixel=0xff000000u | (uint32_t)row[x*4]<<16 | (uint32_t)row[x*4+1]<<8 | row[x*4+2];
            argb[y*WIDTH+x]=pixel;
            hash=(hash^pixel)*16777619u;
        }
    }
    t->rendered=1;
    *checksum=hash;
    return 0;
fail:
    return -1;
}

int panvk_triangle_verify_pixels(const uint32_t *pixels) {
    uint32_t r=pixels[180*512+150], g=pixels[180*512+362], b=pixels[380*512+256];
    uint32_t bg=pixels[16*512+16];
    return ((r>>16)&255)>((r>>8)&255)+40 &&
           ((g>>8)&255)>((g>>16)&255)+40 &&
           (b&255)>((b>>16)&255)+40 &&
           ((bg>>16)&255)<12 && ((bg>>8)&255)<16 && (bg&255)<24;
}

const char *panvk_triangle_device(const struct panvk_triangle *t) { return t->properties.deviceName; }

void panvk_triangle_destroy(struct panvk_triangle *t) {
    if (!t) return;
    if (t->device && t->DestroyDevice) {
        if (t->DeviceWaitIdle) (void)t->DeviceWaitIdle(t->device);
        if (t->fence) t->DestroyFence(t->device,t->fence,NULL);
        if (t->pool) t->DestroyCommandPool(t->device,t->pool,NULL);
        if (t->pipeline) t->DestroyPipeline(t->device,t->pipeline,NULL);
        if (t->layout) t->DestroyPipelineLayout(t->device,t->layout,NULL);
        if (t->framebuffer) t->DestroyFramebuffer(t->device,t->framebuffer,NULL);
        if (t->pass) t->DestroyRenderPass(t->device,t->pass,NULL);
        if (t->view) t->DestroyImageView(t->device,t->view,NULL);
        if (t->image) t->DestroyImage(t->device,t->image,NULL);
        if (t->pixel_map) t->UnmapMemory(t->device,t->image_memory);
        if (t->image_memory) t->FreeMemory(t->device,t->image_memory,NULL);
        if (t->vertices) t->DestroyBuffer(t->device,t->vertices,NULL);
        if (t->vertex_map) t->UnmapMemory(t->device,t->vertex_memory);
        if (t->vertex_memory) t->FreeMemory(t->device,t->vertex_memory,NULL);
        t->DestroyDevice(t->device,NULL);
    }
    if (t->instance && t->DestroyInstance) t->DestroyInstance(t->instance,NULL);
    if (t->library) dlclose(t->library);
    free(t);
}
