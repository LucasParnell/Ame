#pragma once
#define AME_VK_RPC_PRESENT 0x10005u
#ifdef __cplusplus
extern "C" {
#endif
int ame_vulkan_rpc(void* packet,unsigned bytes);
int ame_vulkan_rpc_present(void* packet,unsigned bytes,unsigned* pixels,unsigned* checksum,int* finish);
#ifdef __cplusplus
}
#endif
