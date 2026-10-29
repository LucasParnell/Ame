#pragma once
#include "midix/vulkan/present.h"
struct RemoteImage {VkDevice device;VkImage image;VkImageCreateInfo info;VkDeviceMemory memory;uint64_t offset;};
static RemoteImage remote_images[256];
static bool remote_image_created(VkDevice device,VkImage image,const VkImageCreateInfo* info) {
    for(auto& i:remote_images)if(!i.image){i={device,image,*info,VK_NULL_HANDLE,0};i.info.pNext=nullptr;i.info.pQueueFamilyIndices=nullptr;return true;}
    return false;
}
static void remote_image_bound(VkImage image,VkDeviceMemory memory,uint64_t offset) {
    for(auto& i:remote_images)if(i.image==image){i.memory=memory;i.offset=offset;}
}
static void remote_image_destroyed(VkImage image) {for(auto& i:remote_images)if(i.image==image)i={};}
extern "C" int ame_vulkan_rpc_present(void* packet,unsigned bytes,uint32_t* pixels,uint32_t* checksum,int* finish) {
    using namespace ame::vk;
    if(bytes!=sizeof(PresentRequest) || !pixels || !checksum || !finish)return -1;
    auto* r=static_cast<PresentRequest*>(packet);
    if(r->header.version!=AME_VK_WIRE_VERSION || r->header.bytes!=bytes || r->header.reserved ||
       r->header.command!=Present || r->reserved || r->finish>1)return -1;
    *finish=r->finish;
    if(r->finish) {r->header.result=VK_SUCCESS;return 0;}
    bool good=true;
    VkDevice device=reinterpret_cast<VkDevice>(remote_handles.actual(T_VkDevice,r->device,good));
    VkQueue queue=reinterpret_cast<VkQueue>(remote_handles.actual(T_VkQueue,r->queue,good));
    VkImage image=reinterpret_cast<VkImage>(remote_handles.actual(T_VkImage,r->image,good));
    r->header.result=VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!good || !device || !queue || !image)return 0;
    for(const auto& i:remote_images)if(i.image==image && i.device==device) {
        if(i.info.imageType!=VK_IMAGE_TYPE_2D || i.info.extent.width!=512 || i.info.extent.height!=512 ||
           i.info.extent.depth!=1 || i.info.format!=VK_FORMAT_R8G8B8A8_UNORM || i.info.tiling!=VK_IMAGE_TILING_LINEAR ||
           i.info.samples!=VK_SAMPLE_COUNT_1_BIT)return 0;
        for(auto& m:remote_memory)if(m.memory==i.memory && m.device==device) {
            auto idle=reinterpret_cast<PFN_vkQueueWaitIdle>(device_proc("vkQueueWaitIdle"));
            VkResult result=idle(queue);
            if(result!=VK_SUCCESS){r->header.result=result;return 0;}
            bool temporary=!m.mapping;
            void* mapping=m.mapping;
            if(temporary) {
                auto map=reinterpret_cast<PFN_vkMapMemory>(device_proc("vkMapMemory"));
                result=map(device,m.memory,0,VK_WHOLE_SIZE,0,&mapping);
                if(result!=VK_SUCCESS){r->header.result=result;return 0;}
            }
            VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};
            auto get_layout=reinterpret_cast<PFN_vkGetImageSubresourceLayout>(device_proc("vkGetImageSubresourceLayout"));
            get_layout(device,image,&sub,&layout);
            if(i.offset>m.size || layout.offset>m.size-i.offset || layout.rowPitch<512*4 || layout.rowPitch>(m.size-i.offset-layout.offset)/512)result=VK_ERROR_MEMORY_MAP_FAILED;
            if(result==VK_SUCCESS) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,nullptr,m.memory,0,VK_WHOLE_SIZE};
                auto invalidate=reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(device_proc("vkInvalidateMappedMemoryRanges"));
                result=invalidate(device,1,&range);
            }
            if(result==VK_SUCCESS) {
                uint32_t hash=2166136261u;
                for(unsigned y=0;y<512;y++) {
                    const auto* row=static_cast<const uint8_t*>(mapping)+i.offset+layout.offset+y*layout.rowPitch;
                    for(unsigned x=0;x<512;x++) {
                        uint32_t p=0xff000000u|(uint32_t(row[x*4])<<16)|(uint32_t(row[x*4+1])<<8)|row[x*4+2];
                        pixels[y*512+x]=p;hash=(hash^p)*16777619u;
                    }
                }
                *checksum=hash;
            }
            if(temporary) {auto unmap=reinterpret_cast<PFN_vkUnmapMemory>(device_proc("vkUnmapMemory"));unmap(device,m.memory);}
            r->header.result=result;return 0;
        }
    }
    return 0;
}
