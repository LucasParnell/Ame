#pragma once
#include "midix/vulkan/memory.h"
struct RemoteMemory {VkDevice device;VkDeviceMemory memory;uint64_t size;void* mapping;};
static RemoteMemory remote_memory[256];
static bool remote_allocated(VkDevice device,VkDeviceMemory memory,uint64_t size) {
    for(auto& m:remote_memory)if(!m.memory){m={device,memory,size,nullptr};return true;}
    return false;
}
static void remote_freeing(VkDeviceMemory memory) {
    for(auto& m:remote_memory)if(m.memory==memory)m={};
}
static VkResult memory_dispatch(ame::vk::MemoryRequest* r,unsigned bytes) {
    using namespace ame::vk;
    if(bytes<sizeof(*r) || r->reserved || r->flags)return VK_ERROR_MEMORY_MAP_FAILED;
    bool good=true;
    VkDevice device=reinterpret_cast<VkDevice>(remote_handles.actual(T_VkDevice,r->device,good));
    VkDeviceMemory memory=reinterpret_cast<VkDeviceMemory>(remote_handles.actual(T_VkDeviceMemory,r->memory,good));
    if(!good || !device || !memory)return VK_ERROR_MEMORY_MAP_FAILED;
    for(auto& m:remote_memory)if(m.device==device && m.memory==memory) {
        if(r->header.command==MemoryMap) {
            if(m.mapping || r->offset>=m.size)return VK_ERROR_MEMORY_MAP_FAILED;
            if(r->size==VK_WHOLE_SIZE)r->size=m.size-r->offset;
            if(!r->size || r->size>m.size-r->offset)return VK_ERROR_MEMORY_MAP_FAILED;
            auto fn=reinterpret_cast<PFN_vkMapMemory>(device_proc("vkMapMemory"));
            return fn(device,memory,0,VK_WHOLE_SIZE,0,&m.mapping);
        }
        if(!m.mapping)return VK_ERROR_MEMORY_MAP_FAILED;
        if(r->header.command==MemoryUnmap) {
            auto fn=reinterpret_cast<PFN_vkUnmapMemory>(device_proc("vkUnmapMemory"));
            fn(device,memory);m.mapping=nullptr;return VK_SUCCESS;
        }
        if(r->size>MemoryPayload || r->size>bytes-sizeof(*r) || r->offset>m.size || r->size>m.size-r->offset)
            return VK_ERROR_MEMORY_MAP_FAILED;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,nullptr,memory,0,VK_WHOLE_SIZE};
        if(r->header.command==MemoryUpload) {
            bytes_copy(static_cast<uint8_t*>(m.mapping)+r->offset,r+1,r->size);
            auto fn=reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(device_proc("vkFlushMappedMemoryRanges"));
            return fn(device,1,&range);
        }
        if(r->header.command==MemoryDownload) {
            auto fn=reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(device_proc("vkInvalidateMappedMemoryRanges"));
            VkResult result=fn(device,1,&range);
            if(result==VK_SUCCESS)bytes_copy(r+1,static_cast<uint8_t*>(m.mapping)+r->offset,r->size);
            return result;
        }
    }
    return VK_ERROR_MEMORY_MAP_FAILED;
}
