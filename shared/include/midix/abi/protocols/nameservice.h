#pragma once
#include "midix/abi/types.h"
#include "midix/abi/status.h"
#include "midix/abi/service.h"

constexpr mx_u32_t MX_PROTOCOL_NAMESERVICE = 0x4E530000u; // 'NS\0\0'
constexpr mx_u16_t MX_NS_VERSION = 1;
constexpr mx_u32_t MX_NS_NAME_MAX = 32;

enum MxNsOpcode : mx_u32_t {
    MX_NS_RESOLVE       = 0x4E530001u,
    MX_NS_RESOLVE_REPLY = 0x4E530002u,
    MX_NS_REGISTER      = 0x4E530003u,
    MX_NS_REGISTER_REPLY = 0x4E530004u,
    MX_NS_ERROR         = 0x4E5300FFu,
};

struct MxNsResolveRequest {
    char name[MX_NS_NAME_MAX];
};
static_assert(sizeof(MxNsResolveRequest) == 32, "NS request must fit ring payload");

struct MxNsResolveReply {
    mx_status_t status;
    MxEndpointDesc endpoint;
};
static_assert(sizeof(MxNsResolveReply) == 56, "NS rich reply does not fit one V0 ring slot; use endpoint-only replies for now");

// V0 wire reply: status is carried by slot.type (reply vs error); payload is an endpoint desc.
using MxNsEndpointReply = MxEndpointDesc;

constexpr mx_u32_t MSG_NS_RESOLVE = MX_NS_RESOLVE;
constexpr mx_u32_t MSG_NS_RESOLVE_REPLY = MX_NS_RESOLVE_REPLY;
constexpr mx_u32_t MSG_NS_ERROR = MX_NS_ERROR;
