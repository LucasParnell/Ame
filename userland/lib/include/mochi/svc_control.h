#pragma once

#include "mochi/endpoint.h"
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
    if (!dst || cap == 0) return;
    mx_u32_t i = 0;
    if (src) {
        while (i + 1 < cap && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

struct Client {
    mochi::SpscEndpoint ep;

    bool valid() const { return ep.valid(); }

    bool request(mx_u32_t type, const char* name, Reply* out) const {
        if (!valid()) return false;
        Request req{};
        copy_name(req.name, sizeof(req.name), name);
        if (!ep.send_spinning(type, &req, sizeof(req))) return false;
        RingSlot slot{};
        while (ep.wait_recv(&slot)) {
            if ((slot.type == kReply || slot.type == kError) && slot.length >= sizeof(Reply)) {
                Reply reply{};
                const auto* src = static_cast<const mx_u8_t*>(static_cast<const void*>(slot.payload));
                auto* dst = static_cast<mx_u8_t*>(static_cast<void*>(&reply));
                for (mx_u32_t i = 0; i < sizeof(Reply); ++i) dst[i] = src[i];
                if (out) *out = reply;
                return true;
            }
        }
        return false;
    }
};

struct Server {
    mochi::SpscEndpoint ep;

    bool valid() const { return ep.valid(); }

    bool try_recv(RingSlot* out) const { return ep.try_recv(out); }

    bool send_reply(mx_i32_t status, mx_u32_t state) const {
        if (!valid()) return false;
        const Reply reply{status, state};
        const mx_u32_t type = status == MX_OK ? kReply : kError;
        return ep.send_spinning(type, &reply, sizeof(reply));
    }
};

} // namespace mochi::svcctl
