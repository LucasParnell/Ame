#pragma once
// Raw CDC-to-terminal bridge. Both directions retain owned bytes until the
// next ring accepts them. Session generations are terminal metadata, not USB
// controller authority; this module needs only ring views and arena storage.
#include "mochi/driver.h"
#include "mochi/terminal_session.h"
#include "midix/abi/protocols/usb_function.h"
#include "libk/byte_cursor.h"
namespace mochi::usb::serial {
struct Runtime {
    SpscEndpoint stream;
    terminal::SessionSink input;
    Ring output;
    MxFunctionChunk rx;
    MxRingSlot tx;
    libk::ByteCursor tx_bytes;
    mx_u64_t generation;
    mx_u64_t dropped_output;
    mx_u32_t tx_sequence;
    bool configured, rx_held, lifecycle_pending;
};
inline bool accept_host(Runtime& r) {
    bool progress = false;
    if (r.lifecycle_pending) {
        const auto control = terminal::session_control(r.configured ? MX_TERM_CONTROL_FLOW_ON
                                                                    : MX_TERM_CONTROL_FLOW_OFF,
                                                       r.configured ? r.generation : 0);
        if (!r.input.try_push_msg(MX_TERM_CONTROL, control)) {
            return false;
        }
        r.lifecycle_pending = false;
        progress = true;
    }
    if (!r.rx_held) {
        MxRingSlot slot{};
        if (!r.stream.try_recv(&slot)) {
            return progress;
        }
        progress = true;
        if (slot.type == MX_FUNCTION_RESET) {
            MxFunctionChunk reset{};
            if (message_get(slot, &reset, slot.type) && reset.generation &&
                !(reset.flags & ~MX_FUNCTION_CONFIGURED) && !reset.length) {
                r.generation = reset.generation;
                r.configured = reset.flags & MX_FUNCTION_CONFIGURED;
                r.tx_bytes.reset(); // never re-label a retained output suffix
                r.tx_sequence = 0;
                r.lifecycle_pending = true;
            }
            return progress;
        }
        if (!message_get(slot, &r.rx, MX_FUNCTION_RX) || !r.configured ||
            r.rx.generation != r.generation || r.rx.flags || !r.rx.length ||
            r.rx.length > MX_FUNCTION_INLINE_BYTES) {
            return progress;
        }
        r.rx_held = true;
    }
    if (r.input.try_write(MX_TERM_SESSION_INPUT_BYTES, r.generation,
                          libk::as_bytes(r.rx.data, r.rx.length)) == r.rx.length) {
        r.rx_held = false;
        progress = true;
    }
    return progress; // a retained, blocked row is not forward progress
}
inline bool deliver_shell(Runtime& r) {
    bool progress = false;
    if (r.tx_bytes.empty()) {
        if (!r.output.try_pop(&r.tx)) {
            return false;
        }
        progress = true;
        mx_u64_t generation = 0;
        libk::const_bytes bytes;
        if (!terminal::session_bytes(r.tx, MX_TERM_SESSION_OUTPUT_BYTES, &generation, &bytes)) {
            return progress;
        }
        if (!r.configured || generation != r.generation) {
            r.dropped_output += bytes.size;
            return progress;
        }
        r.tx_bytes.reset(bytes); // view points into the stable arena-owned tx slot
    }
    // One terminal slot is at most 40 bytes: at most two function chunks.
    while (!r.tx_bytes.empty()) {
        const auto bytes = r.tx_bytes.next(MX_FUNCTION_INLINE_BYTES);
        MxFunctionChunk chunk{
            r.generation, r.tx_sequence + 1, static_cast<mx_u16_t>(bytes.size), 0, 0, {}};
        libk::copy_n(libk::as_bytes(chunk.data), bytes, bytes.size);
        if (!r.stream.tx().try_push_msg(MX_FUNCTION_TX, chunk)) {
            break;
        }
        ++r.tx_sequence;
        (void)r.tx_bytes.advance(bytes.size);
        progress = true;
    }
    return progress;
}
inline bool service(Runtime& r) {
    const bool incoming = accept_host(r);
    return deliver_shell(r) || incoming;
}
inline bool retained(const Runtime& r) {
    return r.rx_held || r.lifecycle_pending || !r.tx_bytes.empty();
}
inline mx_u64_t readable_mask(const Runtime& r) {
    return (!r.rx_held && !r.lifecycle_pending ? 1u : 0u) | (r.tx_bytes.empty() ? 2u : 0u);
}
} // namespace mochi::usb::serial
