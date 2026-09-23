#pragma once
// mochi/terminal.h — programmer-facing terminal views over the generic SPSC
// endpoint. Terminal traffic is intentionally separate from log rings: logs are
// diagnostic records, terminal bytes are user/session data.
//
// Every view below is the same three-value endpoint shape (a mapped ring page,
// its slot order, and the doorbell that wakes the consumer) with one fixed
// opcode. The byte-run splitting lives once in ring_push_bytes() (mochi/ring.h),
// so a sink and a source differ only in the opcode they name.

#include "mochi/endpoint.h"
#include "midix/abi/protocols/terminal.h"

namespace mochi::terminal {

// The shared shape of a one-directional terminal byte endpoint.
struct ByteEndpoint {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring != nullptr && doorbell != kNullCap;
    }

    // Slots queued but not yet consumed, bounded by the ring capacity.
    uint32_t depth() const {
        if (!ring) return 0;
        const uint64_t tail = __atomic_load_n(&ring->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t head = __atomic_load_n(&ring->head.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        const uint64_t capacity = mx_ring_capacity(order);
        return pending > capacity ? static_cast<uint32_t>(capacity)
                                  : static_cast<uint32_t>(pending);
    }
};

// Write side: bytes out to the display (msh/io.console -> tty.fb0).
struct OutputSink : ByteEndpoint {
    bool try_write_bytes(const void* data, uint32_t len) const {
        return valid() &&
               ring_push_bytes(ring, order, MX_TERM_OUTPUT_BYTES, data, len, doorbell) == len;
    }

    bool write_bytes_spinning(const void* data, uint32_t len,
                              uint32_t max_yields_per_chunk = 1024) const {
        return valid() &&
               ring_push_bytes_spinning(ring, order, MX_TERM_OUTPUT_BYTES, data, len,
                                        doorbell, max_yields_per_chunk) == len;
    }
};

// Read side: the rendered display byte stream (io.console/tty.fb0 <- msh).
struct OutputSource : ByteEndpoint {
    bool try_read(RingSlot* out) const {
        if (!out || !ring) return false;
        if (!ring_pop(ring, order, out)) return false;
        return out->type == MX_TERM_OUTPUT_BYTES;
    }

    bool wait_read(RingSlot* out) const {
        if (!out || !valid()) return false;
        for (;;) {
            if (!ring_wait_pop(ring, order, out, doorbell)) return false;
            if (out->type == MX_TERM_OUTPUT_BYTES) return true;
        }
    }
};

// Write side: input bytes into the shell (ui.input/tty.fb0 -> msh).
struct InputSink : ByteEndpoint {
    bool try_write_bytes(const void* data, uint32_t len) const {
        return valid() &&
               ring_push_bytes(ring, order, MX_TERM_INPUT_BYTES, data, len, doorbell) == len;
    }

    bool write_input_byte_spinning(char ch, uint32_t max_yields) const {
        return valid() &&
               ring_push_bytes_spinning(ring, order, MX_TERM_INPUT_BYTES, &ch, 1,
                                        doorbell, max_yields) == 1;
    }
};

// A full-duplex terminal session (the shell's view): input and output rings on
// one SPSC endpoint.
struct Terminal {
    mochi::SpscEndpoint endpoint;

    bool valid() const { return endpoint.valid(); }

    bool try_write_bytes(const void* data, uint32_t len) const {
        return endpoint.valid() &&
               ring_push_bytes(endpoint.tx_ring, endpoint.order, MX_TERM_OUTPUT_BYTES,
                               data, len, endpoint.tx_doorbell) == len;
    }

    bool write_bytes_spinning(const void* data, uint32_t len,
                              uint32_t max_yields_per_chunk = 1024) const {
        return endpoint.valid() &&
               ring_push_bytes_spinning(endpoint.tx_ring, endpoint.order,
                                        MX_TERM_OUTPUT_BYTES, data, len,
                                        endpoint.tx_doorbell, max_yields_per_chunk) == len;
    }

    bool try_read(RingSlot* out) const {
        if (!endpoint.try_recv(out)) return false;
        return out && (out->type == MX_TERM_INPUT_BYTES || out->type == MX_TERM_CONTROL);
    }

    bool wait_read(RingSlot* out) const {
        for (;;) {
            if (!endpoint.wait_recv(out)) return false;
            if (out && (out->type == MX_TERM_INPUT_BYTES || out->type == MX_TERM_CONTROL)) return true;
        }
    }
};

} // namespace mochi::terminal
