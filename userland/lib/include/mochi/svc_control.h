#pragma once

#include "libk/str.h"
#include "mochi/channel.h"
#include "midix/abi/status.h"
#include "midix/abi/protocols/svc_control.h"

namespace mochi::svcctl {

using namespace midix::svcctl;

// Client and Server are Channels: the ring pair and doorbells are inherited, and
// this header adds only the protocol. Typed send/receive (mochi/channel.h) is the
// whole body — no slot handling, no byte copies.
struct Client : Channel {
    bool request(mx_u32_t type, const char* name, Reply* out) const {
        if (!valid() || !out) return false;
        Request req{};
        if (!libk::str::copy_exact_fit(req.name, sizeof(req.name), name)) return false;
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
