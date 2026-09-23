#pragma once

#include "libk/str.h"
#include "mochi/channel.h"
#include "midix/abi/status.h"

namespace mochi::svcctl {

constexpr mx_u32_t kRequestStart   = 0x53564301u;
constexpr mx_u32_t kRequestStop    = 0x53564302u;
constexpr mx_u32_t kRequestRestart = 0x53564303u;
constexpr mx_u32_t kRequestShutdown = 0x53564304u;
constexpr mx_u32_t kRequestReboot   = 0x53564305u;
constexpr mx_u32_t kReply          = 0x53564380u;
constexpr mx_u32_t kError          = 0x53564381u;

struct Request {
    char name[32];
};
static_assert(sizeof(Request) <= MX_RING_INLINE_PAYLOAD, "svc control request must fit in one ring slot");

struct Reply {
    mx_i32_t status;
    mx_u32_t state;
};
static_assert(sizeof(Reply) <= MX_RING_INLINE_PAYLOAD, "svc control reply must fit in one ring slot");

static inline void copy_name(char* dst, mx_u32_t cap, const char* src) {
    (void)libk::str::copy_into(dst, cap, src);
}

// Client and Server are Channels: the ring pair and doorbells are inherited, and
// this header adds only the protocol. Typed send/receive (mochi/channel.h) is the
// whole body — no slot handling, no byte copies.
struct Client : Channel {
    bool request(mx_u32_t type, const char* name, Reply* out) const {
        if (!valid()) return false;
        Request req{};
        copy_name(req.name, sizeof(req.name), name);
        if (!send(type, req)) return false;
        MxRingSlot slot{};
        for (;;) {
            if (!wait_recv(&slot)) return false;
            if ((slot.type == kReply || slot.type == kError) && slot.length >= sizeof(Reply)) {
                return message_get(slot, out, 0);
            }
        }
    }
};

struct Server : Channel {
    bool send_reply(mx_i32_t status, mx_u32_t state) const {
        if (!valid()) return false;
        const Reply reply{status, state};
        const mx_u32_t type = status == MX_OK ? kReply : kError;
        return send(type, reply);
    }
};

} // namespace mochi::svcctl
