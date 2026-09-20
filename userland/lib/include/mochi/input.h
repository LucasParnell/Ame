#pragma once
// mochi/input.h — ui.input event protocol.
//
// InputEvents flow: drv.usb.hid → ui.input → tty.fb0.
// Ring message payload is a single InputEvent.

#include <cstdint>
#include "mochi/ring.h"

enum InputEventType : uint32_t {
    INPUT_KEY_DOWN = 1,
    INPUT_KEY_UP   = 2,
    INPUT_TEXT     = 3,  // unicode code point in `unicode` field
};

struct InputEvent {
    uint32_t type;      // InputEventType
    uint32_t keycode;   // USB HID usage ID (boot keyboard)
    uint32_t modifiers; // modifier bitmask (shift/ctrl/alt/gui)
    uint32_t unicode;   // Unicode code point for INPUT_TEXT, else 0
};
static_assert(sizeof(InputEvent) == 16);

// Modifier bits (USB HID boot keyboard report byte 0).
enum InputMod : uint32_t {
    INPUT_MOD_LCTRL  = 1u << 0,
    INPUT_MOD_LSHIFT = 1u << 1,
    INPUT_MOD_LALT   = 1u << 2,
    INPUT_MOD_LGUI   = 1u << 3,
    INPUT_MOD_RCTRL  = 1u << 4,
    INPUT_MOD_RSHIFT = 1u << 5,
    INPUT_MOD_RALT   = 1u << 6,
    INPUT_MOD_RGUI   = 1u << 7,
};

// Ring message types for ui.input subscription endpoint.
enum InputMsg : uint32_t {
    INPUT_MSG_SUBSCRIBE = 1,  // request event stream; payload: none
    INPUT_MSG_EVENT     = 2,  // pushed event; payload: InputEvent
};

namespace mochi::input {

struct EventSink {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring && doorbell != MX_NULL_HANDLE;
    }

    bool try_send(const InputEvent& event) const {
        return valid() &&
               ring_try_push(ring, order, INPUT_MSG_EVENT, &event, sizeof(event), doorbell);
    }

    bool send_spinning(const InputEvent& event, uint32_t max_yields = 64) const {
        return valid() &&
               ring_send_spinning(ring, order, INPUT_MSG_EVENT, &event, sizeof(event),
                                  doorbell, max_yields);
    }
};

struct EventSource {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring && doorbell != MX_NULL_HANDLE;
    }

    bool try_read(InputEvent* out) const {
        if (!out || !ring) return false;
        RingSlot slot{};
        if (!ring_pop(ring, order, &slot)) return false;
        if (slot.type != INPUT_MSG_EVENT || slot.length < sizeof(InputEvent)) return false;
        const auto& event = *reinterpret_cast<const InputEvent*>(slot.payload);
        *out = event;
        return true;
    }

    bool wait_read(InputEvent* out) const {
        if (!out || !valid()) return false;
        for (;;) {
            RingSlot slot{};
            if (!ring_wait_pop(ring, order, &slot, doorbell)) return false;
            if (slot.type == INPUT_MSG_EVENT && slot.length >= sizeof(InputEvent)) {
                const auto& event = *reinterpret_cast<const InputEvent*>(slot.payload);
                *out = event;
                return true;
            }
        }
    }
};

} // namespace mochi::input
