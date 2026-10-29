#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "midix/vulkan/codec.h"
#include "vulkan_rpc.h"
#include "vk-rpc-types.h"
static void* remote_library;
static PFN_vkGetInstanceProcAddr remote_get;
static ame::vk::Handles remote_handles;
static PFN_vkVoidFunction instance_proc(const char* name) {
    VkInstance instance=VK_NULL_HANDLE;
    for(const auto& e:remote_handles.entries)if(e.type==ame::vk::T_VkInstance && e.token) {instance=reinterpret_cast<VkInstance>(e.actual);break;}
    return remote_get(instance,name);
}
static PFN_vkVoidFunction device_proc(const char* name) {return instance_proc(name);}
#include "vk-rpc-types.h"
#include "vulkan_rpc_memory.h"
#include "vulkan_rpc_present.h"
#include "vk-rpc-guest.inc"
#include "vulkan_rpc_cts.h"
extern "C" int ame_vulkan_rpc(void* packet,unsigned bytes) {
    if(bytes<sizeof(ame::vk::Header) || bytes>AME_VK_WIRE_BYTES)return -1;
    auto* header=static_cast<ame::vk::Header*>(packet);
    if(header->version!=AME_VK_WIRE_VERSION || header->bytes!=bytes || header->reserved)return -1;
    if(!remote_get) {
        if(getenv("AME_VK_RPC_TRACE"))setenv("PANDECODE_DUMP_FILE","stderr",1);
        const char* library=getenv("AME_VK_RPC_LIBRARY");
        if(!library)library="libvulkan_panfrost.so";
        remote_library=dlopen(library,RTLD_NOW|RTLD_LOCAL);
        if(remote_library) {
            remote_get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(remote_library,"vk_icdGetInstanceProcAddr"));
            if(!remote_get)remote_get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(remote_library,"vkGetInstanceProcAddr"));
        }
        if(!remote_get){header->result=VK_ERROR_INCOMPATIBLE_DRIVER;return 0;}
    }
    if(header->command>=ame::vk::MemoryMap && header->command<=ame::vk::MemoryDownload) {
        header->result=memory_dispatch(reinterpret_cast<ame::vk::MemoryRequest*>(packet),bytes);return 0;
    }
    if(header->command>=ame::vk::CTSStart && header->command<=ame::vk::CTSLog) {
        header->result=cts_dispatch(reinterpret_cast<ame::vk::CTSRequest*>(packet),bytes);return 0;
    }
    ame::vk::Decoder decoder{static_cast<uint8_t*>(packet),bytes,remote_handles};
    header->result=dispatch(decoder,header->command,header->root);
    decoder.finish();
    remote_handles.commit();
    return 0;
}
