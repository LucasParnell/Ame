#include "client.h"
#include "ame/graphics.h"
#include "midix/vulkan/memory.h"
#include "midix/vulkan/codec.h"
namespace ame::vk {
constexpr uint64_t map_limit=1024*1024;
struct Mapping {VkDevice device;VkDeviceMemory memory;uint64_t offset,size;};
static Mapping maps[8];
alignas(4096) static uint8_t storage[8][map_limit+4096];
alignas(16) static uint8_t packet[AME_VK_WIRE_BYTES];
static VkResult request(uint32_t op,VkDevice device,VkDeviceMemory memory,uint64_t offset,uint64_t& size,void* bytes=nullptr) {
    auto* r=reinterpret_cast<MemoryRequest*>(packet);
    *r={{AME_VK_WIRE_VERSION,op,sizeof(*r),0,0,0},reinterpret_cast<uint64_t>(device),reinterpret_cast<uint64_t>(memory),offset,size,0,0};
    if(op==MemoryUpload || op==MemoryDownload) {
        if(size>MemoryPayload)return VK_ERROR_MEMORY_MAP_FAILED;
        r->header.bytes+=size;
        if(op==MemoryUpload)bytes_copy(packet+sizeof(*r),bytes,size);
    }
    ame_gfx_reply reply{};
    if(!graphics::exchange(packet,r->header.bytes,AME_GFX_API_VULKAN,0,reply))return VK_ERROR_DEVICE_LOST;
    if(r->header.result!=VK_SUCCESS)return static_cast<VkResult>(r->header.result);
    if(op==MemoryDownload)bytes_copy(bytes,packet+sizeof(*r),size);
    size=r->size;return VK_SUCCESS;
}
static VkResult transfer(VkDevice device,VkDeviceMemory memory,uint64_t offset,uint64_t size,bool upload) {
    for(unsigned i=0;i<8;i++) {
        const auto& m=maps[i];if(m.memory!=memory || m.device!=device)continue;
        if(offset<m.offset || offset-m.offset>m.size)return VK_ERROR_MEMORY_MAP_FAILED;
        uint64_t relative=offset-m.offset;
        if(size==VK_WHOLE_SIZE)size=m.size-relative;
        if(size>m.size-relative)return VK_ERROR_MEMORY_MAP_FAILED;
        auto* p=storage[i]+(m.offset&4095)+relative;
        while(size) {
            uint64_t chunk=size>MemoryPayload ? MemoryPayload : size;
            VkResult result=request(upload ? MemoryUpload : MemoryDownload,device,memory,offset,chunk,p);
            if(result!=VK_SUCCESS)return result;
            size-=chunk;offset+=chunk;p+=chunk;
        }
        return VK_SUCCESS;
    }
    return VK_ERROR_MEMORY_MAP_FAILED;
}
void forget_mapping(VkDeviceMemory memory) {for(auto& m:maps)if(m.memory==memory)m={};}
}
extern "C" {
VkResult vkMapMemory(VkDevice device,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** data) {
    using namespace ame::vk;
    if(!data || flags)return VK_ERROR_MEMORY_MAP_FAILED;
    for(const auto& m:maps)if(m.memory==memory)return VK_ERROR_MEMORY_MAP_FAILED;
    for(unsigned i=0;i<8;i++)if(!maps[i].memory) {
        VkResult result=request(MemoryMap,device,memory,offset,size);
        if(result!=VK_SUCCESS)return result;
        if(size>map_limit) {uint64_t zero=0;request(MemoryUnmap,device,memory,0,zero);return VK_ERROR_MEMORY_MAP_FAILED;}
        maps[i]={device,memory,offset,size};
        *data=storage[i]+(offset&4095);
        return VK_SUCCESS;
    }
    return VK_ERROR_MEMORY_MAP_FAILED;
}
void vkUnmapMemory(VkDevice device,VkDeviceMemory memory) {
    uint64_t zero=0;ame::vk::request(ame::vk::MemoryUnmap,device,memory,0,zero);
    ame::vk::forget_mapping(memory);
}
VkResult vkFlushMappedMemoryRanges(VkDevice device,uint32_t count,const VkMappedMemoryRange* ranges) {
    for(uint32_t i=0;i<count;i++) {
        if(ranges[i].pNext)return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkResult result=ame::vk::transfer(device,ranges[i].memory,ranges[i].offset,ranges[i].size,true);
        if(result!=VK_SUCCESS)return result;
    }
    return VK_SUCCESS;
}
VkResult vkInvalidateMappedMemoryRanges(VkDevice device,uint32_t count,const VkMappedMemoryRange* ranges) {
    for(uint32_t i=0;i<count;i++) {
        if(ranges[i].pNext)return VK_ERROR_EXTENSION_NOT_PRESENT;
        VkResult result=ame::vk::transfer(device,ranges[i].memory,ranges[i].offset,ranges[i].size,false);
        if(result!=VK_SUCCESS)return result;
    }
    return VK_SUCCESS;
}
}
