#include "ame/vulkan.h"
#include "ame/graphics.h"
#include "midix/vulkan/present.h"
extern "C" VkResult ameVkPresentDiagnosticImage(VkDevice device,VkQueue queue,VkImage image,VkBool32 finish) {
    ame::vk::PresentRequest request{{AME_VK_WIRE_VERSION,ame::vk::Present,sizeof(request),0,0,0},
        reinterpret_cast<uint64_t>(device),reinterpret_cast<uint64_t>(queue),reinterpret_cast<uint64_t>(image),finish,0};
    ame_gfx_reply reply{};
    unsigned flags=AME_GFX_PRESENT|(finish ? AME_GFX_PRESENT_RESTORE : AME_GFX_PRESENT_STREAM);
    if(!ame::graphics::exchange(&request,sizeof(request),AME_GFX_API_VULKAN,flags,reply))return VK_ERROR_DEVICE_LOST;
    return static_cast<VkResult>(request.header.result);
}
