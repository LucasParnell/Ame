#pragma once
// Programmer-facing terminal API built on the generic SPSC endpoint.
// Terminal traffic is intentionally separate from log rings: logs are diagnostic
// records, terminal bytes are user/session data.

#include "mochi/endpoint.h"
#include "midix/abi/protocols/terminal.h"

namespace mochi::terminal {

struct OutputSink {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring && doorbell != kNullCap;
    }

    bool try_write_bytes(const void* data, uint32_t len) const {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t off = 0;
        while (off < len) {
            const uint32_t chunk = (len - off > MX_RING_INLINE_PAYLOAD)
                                       ? MX_RING_INLINE_PAYLOAD
                                       : (len - off);
            if (!valid() ||
                !ring_try_push(ring, order, MX_TERM_OUTPUT_BYTES,
                               p + off, chunk, doorbell)) {
                return false;
            }
            off += chunk;
        }
        return true;
    }

    bool write_bytes_spinning(const void* data, uint32_t len,
                              uint32_t max_yields_per_chunk = 1024) const {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t off = 0;
        while (off < len) {
            const uint32_t chunk = (len - off > MX_RING_INLINE_PAYLOAD)
                                       ? MX_RING_INLINE_PAYLOAD
                                       : (len - off);
            if (!valid() ||
                !ring_send_spinning(ring, order, MX_TERM_OUTPUT_BYTES,
                                    p + off, chunk, doorbell,
                                    max_yields_per_chunk)) {
                return false;
            }
            off += chunk;
        }
        return true;
    }
};

struct OutputSource {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring && doorbell != kNullCap;
    }

    bool try_read(RingSlot* out) const {
        if (!out || !ring) return false;
        if (!ring_pop(ring, order, out)) return false;
        return out->type == MX_TERM_OUTPUT_BYTES;
    }

    uint32_t depth() const {
        if (!ring) return 0;
        const uint64_t tail = __atomic_load_n(&ring->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t head = __atomic_load_n(&ring->head.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        const uint64_t capacity = 1ull << order;
        return pending > capacity ? static_cast<uint32_t>(capacity)
                                  : static_cast<uint32_t>(pending);
    }

    bool wait_read(RingSlot* out) const {
        if (!out || !valid()) return false;
        for (;;) {
            if (!ring_wait_pop(ring, order, out, doorbell)) return false;
            if (out->type == MX_TERM_OUTPUT_BYTES) return true;
        }
    }
};

struct Terminal {
    mochi::SpscEndpoint endpoint;

    bool valid() const { return endpoint.valid(); }

    bool try_write_bytes(const void* data, uint32_t len) const {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t off = 0;
        while (off < len) {
            const uint32_t chunk = (len - off > MX_RING_INLINE_PAYLOAD)
                                       ? MX_RING_INLINE_PAYLOAD
                                       : (len - off);
            if (!endpoint.try_send(MX_TERM_OUTPUT_BYTES, p + off, chunk)) return false;
            off += chunk;
        }
        return true;
    }

    bool write_bytes_spinning(const void* data, uint32_t len, uint32_t max_yields_per_chunk = 1024) const {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t off = 0;
        while (off < len) {
            const uint32_t chunk = (len - off > MX_RING_INLINE_PAYLOAD)
                                       ? MX_RING_INLINE_PAYLOAD
                                       : (len - off);
            if (!endpoint.send_spinning(MX_TERM_OUTPUT_BYTES, p + off, chunk, max_yields_per_chunk)) {
                return false;
            }
            off += chunk;
        }
        return true;
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
