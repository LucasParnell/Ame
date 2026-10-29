#pragma once
// Terminal transport ownership only. tty.fb0 is the sole producer of the
// shell input ring; keyboard and serial bytes merge in this retained buffer.
// Rendering, USB class handling and controller state are independent.
#include "mochi/terminal_session.h"
#include "libk/byte_cursor.h"
namespace mochi::tty {
struct InputBuffer {
    mx_u8_t storage[MX_TERM_INLINE_BYTES];
    libk::ByteCursor pending;
    MxTermControl control;
    bool control_pending;
    bool retained() const { return control_pending || !pending.empty(); }
    bool hold_control(const MxTermControl& next) {
        if (retained()) return false;
        control = next;
        control_pending = true;
        return true;
    }
    bool hold(libk::const_bytes bytes) {
        if (retained() || !bytes.size || bytes.size > sizeof(storage)) {
            return false;
        }
        if (!libk::copy_n(libk::as_bytes(storage), bytes, bytes.size)) {
            return false;
        }
        pending.reset(libk::as_bytes(storage, bytes.size));
        return true;
    }
    bool flush(const terminal::InputSink& sink) {
        if (control_pending) {
            if (!sink.try_push_msg(MX_TERM_CONTROL, control)) return false;
            control_pending = false;
            return true;
        }
        if (pending.empty()) {
            return false;
        }
        const auto bytes = pending.next(MX_TERM_INLINE_BYTES);
        const auto accepted = sink.push_bytes(MX_TERM_INPUT_BYTES, bytes.data, bytes.size);
        (void)pending.advance(accepted);
        return accepted != 0;
    }
};
struct SerialMirror {
    static constexpr mx_u64_t kStallBudgetNs = 1000000000ull;
    terminal::SessionSink output;
    MxRingSlot slot;
    libk::ByteCursor pending;
    mx_u64_t generation;
    mx_u64_t blocked_at, dropped_output;
    bool backpressured, output_stalled;
    void change(mx_u64_t next) {
        generation = next;
        pending.reset(); // delayed old output retains its old generation or is discarded
        backpressured = output_stalled = false;
    }
    bool hold(const MxRingSlot& incoming) {
        if (!pending.empty()) {
            return false;
        }
        if (!generation || !output.valid()) {
            return true; // local terminal still works offline
        }
        if (!message_valid(incoming) || incoming.type != MX_TERM_OUTPUT_BYTES) {
            return false;
        }
        if (output_stalled) {
            dropped_output += incoming.length;
            return true; // the local terminal remains the primary output
        }
        slot = incoming;
        pending.reset(libk::as_bytes(slot.payload, slot.length));
        return true;
    }
    bool flush(mx_u64_t now = 0) {
        bool progress = false;
        while (!pending.empty()) {
            const auto accepted = output.try_write(MX_TERM_SESSION_OUTPUT_BYTES, generation,
                                                   pending.next(terminal::kSessionRunBytes));
            if (!accepted) {
                // Preserve ordinary serial backpressure briefly, then stop
                // mirroring for this session. A dead USB consumer must not
                // hold the phone's renderer and shell producer indefinitely.
                if (!backpressured) {
                    blocked_at = now;
                    backpressured = true;
                } else if (now - blocked_at >= kStallBudgetNs) {
                    dropped_output += pending.remaining();
                    pending.reset();
                    backpressured = false;
                    output_stalled = true;
                    return true;
                }
                break;
            }
            (void)pending.advance(accepted);
            backpressured = false;
            progress = true;
        }
        return progress;
    }
};
inline bool accept_serial(const MxRingSlot& slot, SerialMirror& mirror, InputBuffer& input) {
    if (slot.type == MX_TERM_CONTROL) {
        MxTermControl control{};
        if (!message_get(slot, &control, slot.type) || control.reserved0 || input.retained()) {
            return false;
        }
        if (control.code == MX_TERM_CONTROL_FLOW_OFF) {
            mirror.change(0);
        } else if (control.code == MX_TERM_CONTROL_FLOW_ON) {
            const auto generation = terminal::session_generation(control);
            if (!generation) {
                return false;
            }
            mirror.change(generation);
        } else {
            return false;
        }
        return input.hold_control(control);
    }
    mx_u64_t generation = 0;
    libk::const_bytes bytes;
    return terminal::session_bytes(slot, MX_TERM_SESSION_INPUT_BYTES, &generation, &bytes) &&
           generation == mirror.generation && input.hold(bytes);
}
} // namespace mochi::tty
