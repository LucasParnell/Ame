#pragma once
// Midix input service protocol ABI (`ui.input`).
//
// Input events flow drv.usb.hid -> ui.input -> tty.fb0. One ring message carries
// one InputEvent; SUBSCRIBE asks for the stream and has no payload.
//
// Transport envelope: MxRingSlot. Payloads are sized for the 48-byte inline
// budget.

#include "midix/abi/types.h"
#include "midix/abi/ring.h"

constexpr mx_u32_t MX_PROTOCOL_INPUT = 0x494E5054u; // 'INPT'

using MxInputFrame = MxRingSlot;
static_assert(sizeof(MxInputFrame) == 64, "input frame must be one ring slot");

// Ring message types for the ui.input subscription endpoint.
enum MxInputMsg : mx_u32_t {
    INPUT_MSG_SUBSCRIBE = 1, // request the event stream; no payload
    INPUT_MSG_EVENT     = 2, // one InputEvent
};

enum InputEventType : mx_u32_t {
    INPUT_KEY_DOWN = 1,
    INPUT_KEY_UP   = 2,
    INPUT_TEXT     = 3, // Unicode code point in `unicode`
};

// Modifier bits (USB HID boot-keyboard report byte 0).
enum InputMod : mx_u32_t {
    INPUT_MOD_LCTRL  = 1u << 0,
    INPUT_MOD_LSHIFT = 1u << 1,
    INPUT_MOD_LALT   = 1u << 2,
    INPUT_MOD_LGUI   = 1u << 3,
    INPUT_MOD_RCTRL  = 1u << 4,
    INPUT_MOD_RSHIFT = 1u << 5,
    INPUT_MOD_RALT   = 1u << 6,
    INPUT_MOD_RGUI   = 1u << 7,
};

struct InputEvent {
    mx_u32_t type;      // InputEventType
    mx_u32_t keycode;   // USB HID usage ID (boot keyboard)
    mx_u32_t modifiers; // modifier bitmask (shift/ctrl/alt/gui)
    mx_u32_t unicode;   // Unicode code point for INPUT_TEXT, else 0
};
static_assert(sizeof(InputEvent) <= MX_RING_INLINE_PAYLOAD, "input event must fit one payload");
static_assert(sizeof(InputEvent) == 16, "input event size drift");
