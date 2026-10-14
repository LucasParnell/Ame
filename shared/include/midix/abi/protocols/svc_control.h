#pragma once
// Native service lifecycle control protocol (init server, shell client).
#include "midix/abi/ring.h"
#include "midix/abi/platform.h"

namespace midix::svcctl {

constexpr mx_u32_t kRequestStart   = 0x53564301u;
constexpr mx_u32_t kRequestStop    = 0x53564302u;
constexpr mx_u32_t kRequestRestart = 0x53564303u;
constexpr mx_u32_t kRequestShutdown = 0x53564304u;
constexpr mx_u32_t kRequestReboot   = 0x53564305u;
constexpr mx_u32_t kRequestFastboot = 0x53564306u;
constexpr mx_u32_t kReply          = 0x53564380u;
constexpr mx_u32_t kError          = 0x53564381u;

constexpr mx_u32_t power_operation(mx_u32_t request) {
    switch (request) {
    case kRequestShutdown: return MX_PLATFORM_SYSTEM_OFF;
    case kRequestReboot: return MX_PLATFORM_SYSTEM_RESET;
    case kRequestFastboot: return MX_PLATFORM_SYSTEM_FASTBOOT;
    default: return 0;
    }
}

struct Request {
    char name[32];
};
static_assert(sizeof(Request) <= MX_RING_INLINE_PAYLOAD, "svc control request must fit in one ring slot");

struct Reply {
    mx_i32_t status;
    mx_u32_t state;
};
static_assert(sizeof(Reply) <= MX_RING_INLINE_PAYLOAD, "svc control reply must fit in one ring slot");

} // namespace midix::svcctl
