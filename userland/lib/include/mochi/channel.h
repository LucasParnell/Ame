#pragma once
// mochi/channel.h — the ergonomic face of one SPSC endpoint.
//
// A Channel is what most users want: a request/reply ring pair with its
// doorbells, wrapped so a caller sends and receives *typed messages* instead of
// pushing raw slots and decoding payload bytes by hand. The ring geometry, the
// inline payload budget and the doorbell sleep/wake protocol are handled here.
//
// Nothing is hidden that a driver may need. `ep` is the raw SpscEndpoint, and
// try_recv()/wait_recv() return the raw MxRingSlot, so code that wants the metal
// keeps using mochi/endpoint.h and mochi/ring.h directly. The Channel is a
// convenience, not a gate.

#include "mochi/endpoint.h"
#include "mochi/message.h"

namespace mochi {

struct Channel {
    SpscEndpoint ep;

    bool valid() const { return ep.valid(); }

    // --- send ---------------------------------------------------------------
    // Non-blocking: false if the ring is full.
    template <typename T>
    bool try_send(mx_u32_t type, const T& msg) const {
        return ep.try_send(type, &msg, message_size<T>());
    }

    // Back-pressuring: yields in userspace (bounded) rather than blocking.
    template <typename T>
    bool send(mx_u32_t type, const T& msg, mx_u32_t max_yields = 1024) const {
        return ep.send_spinning(type, &msg, message_size<T>(), max_yields);
    }

    // Bounded by an absolute nanosecond deadline.
    template <typename T>
    bool send_bounded(mx_u32_t type, const T& msg, mx_u64_t deadline_ns) const {
        return ep.send_bounded(type, &msg, message_size<T>(), deadline_ns);
    }

    // Raw byte send, for a protocol whose payload is not one POD.
    bool send_bytes(mx_u32_t type, const void* data, mx_u32_t len,
                    mx_u32_t max_yields = 1024) const {
        return ep.send_spinning(type, data, len, max_yields);
    }
    bool send_bytes_bounded(mx_u32_t type, const void* data, mx_u32_t len,
                            mx_u64_t deadline_ns) const {
        return ep.send_bounded(type, data, len, deadline_ns);
    }

    // --- receive ------------------------------------------------------------
    bool try_recv(MxRingSlot* out) const { return ep.try_recv(out); }
    bool wait_recv(MxRingSlot* out) const { return ep.wait_recv(out); }

    // Receive the next message of `type`, skipping slots with any other opcode.
    // `deadline_ns == 0` blocks on the doorbell; otherwise it gives up at the
    // absolute deadline. False means invalid output, timeout, invalid payload
    // length, or peer death.
    template <typename T>
    bool receive(mx_u32_t type, T* out, mx_u64_t deadline_ns = 0) const {
        if (!out) return false;
        MxRingSlot slot{};
        for (;;) {
            const bool got = (deadline_ns != 0) ? ep.recv_bounded(&slot, deadline_ns)
                                                : ep.wait_recv(&slot);
            if (!got) return false;
            if (slot.type == type) return message_get(slot, out, type);
            if (deadline_ns != 0 && mx_clock_ns() >= deadline_ns) return false;
        }
    }

    // --- request/reply ------------------------------------------------------
    // Send `req` of `req_type` and wait for a `reply_type` message: the whole
    // ring dance, with no slot, length or byte copy at the call site.
    template <typename TReq, typename TReply>
    bool call(mx_u32_t req_type, const TReq& req, mx_u32_t reply_type, TReply* reply,
              mx_u64_t deadline_ns = 0) const {
        if (!reply) return false;
        const bool sent = deadline_ns != 0 ? send_bounded(req_type, req, deadline_ns)
                                          : send(req_type, req);
        return sent && receive(reply_type, reply, deadline_ns);
    }

    // Slots queued on the receive ring but not yet consumed.
    mx_u32_t depth() const { return ep.rx().depth(); }
};

} // namespace mochi
