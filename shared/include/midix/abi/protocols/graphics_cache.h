#pragma once
/* API-neutral opaque cache bytes. The host chooses the only storage path;
 * guest offsets are bounded, and the transfer arena is never GPU accessible. */
#define AME_GFX_CACHE_REQUEST 0x47584605U
#define AME_GFX_CACHE_REPLY 0x47584606U
#define AME_GFX_CACHE_LOAD 1U
#define AME_GFX_CACHE_SAVE 2U
#define AME_GFX_CACHE_TRANSFER 16384U
#define AME_GFX_CACHE_MAX (16U + 16U * (24U + 16384U))
struct ame_gfx_cache_io {
    unsigned long long id;
    unsigned int operation, offset, size, total;
    int status;
    unsigned int reserved;
};
#ifdef __cplusplus
static_assert(sizeof(ame_gfx_cache_io) == 32);
#endif
