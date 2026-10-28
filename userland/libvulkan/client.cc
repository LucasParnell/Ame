#include "client.h"
#include "ame/graphics.h"
#include "midix/vulkan/codec.h"
namespace ame::vk {
static Encoder encoder;
struct RecordingError {VkCommandBuffer command;VkResult result;};
static RecordingError errors[64];
static bool error_overflow;
void record_error(VkCommandBuffer command,VkResult result) {
    if(result==VK_SUCCESS)return;
    for(const auto& e:errors)if(e.command==command)return;
    for(auto& e:errors)if(!e.command){e={command,result};return;}
    error_overflow=true;
}
VkResult record_status(VkCommandBuffer command) {
    for(const auto& e:errors)if(e.command==command)return e.result;
    return error_overflow ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
void record_clear(VkCommandBuffer command) {for(auto& e:errors)if(e.command==command)e={};}
bool same(const char* a,const char* b) {
    if(!a||!b)return false;
    while(*a&&*a==*b){a++;b++;}return *a==*b;
}
VkResult call(uint32_t command,uint32_t root_type,const void* args) {
    encoder.used=sizeof(Header);encoder.copy_count=0;encoder.good=true;
    uint32_t root=encoder.object(root_type,args,1,true,false);
    if(!encoder.good)return VK_ERROR_OUT_OF_HOST_MEMORY;
    auto* header=reinterpret_cast<Header*>(encoder.data);
    *header={AME_VK_WIRE_VERSION,command,encoder.used,root,0,0};
    ame_gfx_reply reply{};
    if(!graphics::exchange(encoder.data,encoder.used,AME_GFX_API_VULKAN,0,reply))
        return VK_ERROR_DEVICE_LOST;
    if(header->version!=AME_VK_WIRE_VERSION || header->command!=command || header->bytes!=encoder.used)
        return VK_ERROR_DEVICE_LOST;
    encoder.copy_back();
    return static_cast<VkResult>(header->result);
}
}
extern "C" {
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance,const char* name) {
    (void)instance;
    if(ame::vk::same(name,"vkGetInstanceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if(ame::vk::same(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    return ame::vk::proc(name);
}
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device,const char* name) {
    (void)device;return vkGetInstanceProcAddr(VK_NULL_HANDLE,name);
}
}
