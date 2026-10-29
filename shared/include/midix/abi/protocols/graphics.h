#pragma once
/* API-neutral bounded stream transport. No pointers or GL enums in this layer.
 * A batch consists of contiguous CHUNK records then one SUBMIT. Replies retire
 * the batch before its storage may be reused. One provisioned SPSC client. */
#define AME_GFX_CHUNK 0x47584601U
#define AME_GFX_SHARED 0x47584604U
#define AME_GFX_COMMAND_SLOTS 2U
#define AME_GFX_COMMAND_BYTES (2U * 16384U)
#define AME_GFX_SUBMIT 0x47584602U
#define AME_GFX_REPLY 0x47584603U
// 0x05/0x06 belong to graphics_cache.h.
#define AME_GFX_DISPLAY_DRAIN 0x47584607U
#define AME_GFX_DISPLAY_DRAIN_REPLY 0x47584608U
#define AME_GFX_VERSION 2U
#define AME_GFX_API_GLES2 1U
// API id 2 was the removed guest-only triangle diagnostic; do not reuse it.
// Registry-encoded Vulkan commands with a bounded shared-arena response.
#define AME_GFX_API_VULKAN 3U
#define AME_GFX_BATCH_BYTES 16384U
#define AME_GFX_PRESENT 1U
// Streaming frames acknowledge immediately; restore ends the overlay explicitly.
#define AME_GFX_PRESENT_STREAM 2U
#define AME_GFX_PRESENT_RESTORE 4U
static inline int ame_gfx_flags_valid(unsigned flags) {
    return flags == 0 || flags == AME_GFX_PRESENT ||
           flags == (AME_GFX_PRESENT | AME_GFX_PRESENT_STREAM) ||
           flags == (AME_GFX_PRESENT | AME_GFX_PRESENT_RESTORE);
}
struct ame_gfx_chunk {
    unsigned long long id;
    unsigned int offset, size;
    unsigned char data[32];
};
struct ame_gfx_submit {
    unsigned long long id;
    unsigned int version, api, bytes, flags;
};
struct ame_gfx_shared_submit {
    struct ame_gfx_submit request;
    unsigned int slot, reserved;
};
struct ame_gfx_reply {
    unsigned long long id;
    int status;
    unsigned int value;
    unsigned long long render_ns;
    unsigned int checksum, gpu_id;
    unsigned int present_target, scanout_count;
};
// Guest requests retirement before destroying/recreating a render context.
// The manager restores the native display before acknowledging this request.
struct ame_gfx_display_drain {
    unsigned long long id;
    int status;
    unsigned int reserved;
};
#ifdef __cplusplus
static_assert(sizeof(ame_gfx_chunk) == 48);
static_assert(sizeof(ame_gfx_submit) == 24);
static_assert(sizeof(ame_gfx_reply) == 40);
#endif
