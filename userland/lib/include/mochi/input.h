#pragma once
// mochi/input.h — ui.input event endpoints over a mochi::Ring.
//
// The wire contract (opcodes, event types, modifier bits, InputEvent) lives in
// the ABI header midix/abi/protocols/input.h; these are the ergonomic endpoints,
// each a Ring with the input opcode. Events flow drv.usb.hid -> ui.input ->
// tty.fb0.

#include "midix/abi/protocols/input.h"
#include "mochi/message.h"
#include "mochi/ring.h"

namespace mochi::input {

// Push one event to a subscriber (ui.input -> tty.fb0).
struct EventSink : Ring {
    bool try_send(const InputEvent& event) const {
        return try_push_msg(INPUT_MSG_EVENT, event);
    }

    bool send_spinning(const InputEvent& event, uint32_t max_yields = 64) const {
        return valid() && push_msg(INPUT_MSG_EVENT, event, max_yields);
    }
};

// Read the event stream (tty.fb0 <- ui.input).
struct EventSource : Ring {
    bool try_read(InputEvent* out) const {
        if (!out) return false;
        RingSlot slot{};
        return try_pop(&slot) && message_get(slot, out, INPUT_MSG_EVENT);
    }

    bool wait_read(InputEvent* out) const {
        if (!out || !valid()) return false;
        for (;;) {
            RingSlot slot{};
            if (!wait_pop(&slot)) return false;
            if (message_get(slot, out, INPUT_MSG_EVENT)) return true;
        }
    }
};

} // namespace mochi::input
