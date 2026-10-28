#pragma once
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Ame's temporary 512x512 display overlay, analogous to the EGL diagnostic
 * window. The app supplies a bound, linear R8G8B8A8_UNORM image, sample count 1,
 * mip/layer 0 in GENERAL layout. The queue must own the image. finish restores
 * the console; pass null handles when finishing. No implicit image rendering.
 * Standard surface/swapchain WSI is not exposed by this initial native port. */
VkResult ameVkPresentDiagnosticImage(VkDevice device,VkQueue queue,VkImage image,VkBool32 finish);
#ifdef __cplusplus
}
#endif
