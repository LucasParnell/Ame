#pragma once
// Generic service endpoint description. This is the unit nameservice should return.

#include "midix/abi/types.h"

constexpr mx_u32_t MX_ENDPOINT_MAGIC = 0x454E4450u; // 'ENDP'
constexpr mx_u16_t MX_ENDPOINT_VERSION = 1;
constexpr mx_u32_t MX_ENDPOINT_MAX_HANDLES = 4;

enum MxEndpointFlags : mx_u32_t {
    MX_ENDPOINT_FLAG_NONE = 0,
    MX_ENDPOINT_FLAG_SPSC = 1u << 0,
    MX_ENDPOINT_FLAG_CLIENT_ALLOCATED = 1u << 1,
};

struct MxEndpointDesc {
    mx_u32_t magic;
    mx_u16_t version;
    mx_u16_t handle_count;
    mx_u32_t protocol;
    mx_u32_t flags;
    mx_handle_t handles[MX_ENDPOINT_MAX_HANDLES];
};
static_assert(sizeof(MxEndpointDesc) == 48, "endpoint descriptor must fit ring payload");
