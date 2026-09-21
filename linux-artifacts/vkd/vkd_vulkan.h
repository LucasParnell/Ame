#pragma once
/*
 * vkd_vulkan.h — the Vulkan execution half of dango-vkd (spec §14.6, Phase N).
 *
 * dango-vkd serves the Midix remote Vulkan session protocol over the Dango
 * channel; this header is the seam where the command stream in the session arena
 * is executed by a real Vulkan device. Two implementations sit behind it:
 *
 *   * with VKD_HAVE_VULKAN (a libc guest image that carries the Vulkan loader
 *     and the Mesa lavapipe ICD), it drives lavapipe through the standard Vulkan
 *     API;
 *   * without it (the current freestanding, no-libc initramfs), it reports no
 *     device, so dango-vkd answers a submit with MX_ERR_SERVICE_UNAVAILABLE and
 *     the native client keeps its local path. It never fakes a result.
 *
 * The command vocabulary is midix/abi/protocols/vulkan_session.h; every operand
 * is a session-arena byte offset, so the executor needs no id translation.
 */
#ifndef MIDIX_VKD_VULKAN_H
#define MIDIX_VKD_VULKAN_H

#include <stdint.h>

#define VKD_SUBMIT_TIMEOUT_NS UINT64_C(5000000000)

/* 1 when a real Vulkan device is present and usable. */
int vkd_vulkan_available(void);

/* Create the device (instance -> physical device -> device + queue). Returns 0
 * on success and leaves the device usable, or -1 when unavailable. Idempotent. */
int vkd_vulkan_init(void);

/* Execute the command stream [command_offset, command_offset+command_bytes) of
 * `arena` (the session arena). Returns 0 on success, -1 on failure. */
int vkd_vulkan_execute(uint8_t* arena, uint32_t arena_bytes, uint32_t command_offset,
                       uint32_t command_bytes);

/* WSI seam: currently unsupported, returns -1 and reports zero copied bytes. */
int vkd_vulkan_present(const uint8_t* image, uint32_t image_bytes, uint32_t* copied_out);

void vkd_vulkan_shutdown(void);

#endif /* MIDIX_VKD_VULKAN_H */
