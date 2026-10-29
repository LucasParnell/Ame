#pragma once
#include <vulkan/vulkan.h>
namespace ame::vk {
VkResult call(uint32_t command,uint32_t type,const void* args);
bool same(const char* a,const char* b);
void forget_mapping(VkDeviceMemory memory);
void record_error(VkCommandBuffer command,VkResult result);
VkResult record_status(VkCommandBuffer command);
void record_clear(VkCommandBuffer command);
PFN_vkVoidFunction proc(const char* name);
}
