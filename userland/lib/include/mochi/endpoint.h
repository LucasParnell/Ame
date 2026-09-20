#pragma once
#include "midix/abi/service.h"
#include "mochi/ring.h"
#include "mochi/status.h"

namespace mochi {

// A SPSC endpoint is explicit about which operations may block. try_send and
// try_recv never sleep and never ask the kernel for progress unless a sleeping
// peer requested a doorbell notify via the shared ring flag.
struct SpscEndpoint {
    RingPage* tx_ring;
    RingPage* rx_ring;
    uint32_t order;
    CapHandle tx_doorbell;
    CapHandle rx_doorbell;

    bool valid() const {
        return tx_ring && rx_ring && tx_doorbell != kNullCap && rx_doorbell != kNullCap;
    }

    bool try_send(uint32_t type, const void* data, uint32_t len) const {
        return valid() && ring_try_push(tx_ring, order, type, data, len, tx_doorbell);
    }

    bool send_spinning(uint32_t type, const void* data, uint32_t len, uint32_t max_yields = 1024) const {
        return valid() && ring_send_spinning(tx_ring, order, type, data, len, tx_doorbell, max_yields);
    }

    bool try_recv(RingSlot* out) const {
        return rx_ring && ring_pop(rx_ring, order, out);
    }

    bool wait_recv(RingSlot* out) const {
        return rx_ring && rx_doorbell != kNullCap && ring_wait_pop(rx_ring, order, out, rx_doorbell);
    }

    // Bounded push/pop. Unlike ring_send_spinning()/ring_wait_pop() these never
    // block past an absolute nanosecond deadline, so a server can bound how long
    // one unresponsive peer holds its service thread. On expiry they return
    // false and leave the rings untouched; a dropped reply is the caller's
    // defined return path (see mochi/sessions.h).
    bool send_bounded(uint32_t type, const void* data, uint32_t len,
                      mx_u64_t deadline_ns) const {
        if (!tx_ring || tx_doorbell == kNullCap) return false;
        for (;;) {
            if (ring_try_push(tx_ring, order, type, data, len, tx_doorbell)) return true;
            if (mx_clock_ns() >= deadline_ns) return false;
            sys_yield();
        }
    }

    bool recv_bounded(RingSlot* out, mx_u64_t deadline_ns) const {
        if (!rx_ring) return false;
        for (;;) {
            if (ring_pop(rx_ring, order, out)) return true;
            if (mx_clock_ns() >= deadline_ns) return false;
            sys_yield();
        }
    }
};

static inline bool endpoint_desc_valid(const MxEndpointDesc& desc, uint32_t protocol) {
    return desc.magic == MX_ENDPOINT_MAGIC &&
           desc.version == MX_ENDPOINT_VERSION &&
           desc.protocol == protocol &&
           desc.handle_count <= MX_ENDPOINT_MAX_HANDLES;
}

} // namespace mochi
