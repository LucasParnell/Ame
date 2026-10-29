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

constexpr mx_u32_t MX_PROTOCOL_INPUT = 0x494e5032u; // INP2; rebuild producers/consumers together
constexpr mx_u16_t MX_INPUT_ABI_VERSION = 2;

using MxInputFrame = MxRingSlot;
static_assert(sizeof(MxInputFrame) == 64, "input frame must be one ring slot");

// Ring message types for the ui.input subscription endpoint.
enum MxInputMsg : mx_u32_t {
    INPUT_MSG_SUBSCRIBE = 1, // request the event stream; no payload
    INPUT_MSG_EVENT     = 2, // one InputEvent
    INPUT_MSG_LEDS      = 3, // reverse policy channel: one InputLedCommand
};

enum InputEventType : mx_u32_t {
    INPUT_KEY_DOWN = 1,
    INPUT_KEY_UP   = 2,
    INPUT_KEY_REPEAT = 7, // ui.input-derived; producer sequence remains unchanged
    INPUT_TEXT     = 3, // Unicode code point in `unicode`
    INPUT_SOURCE_ATTACHED = 4,
    INPUT_SOURCE_DETACHED = 5,
    INPUT_SOURCE_RESET = 6, // release this source's held state before resynchronizing
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
    mx_u32_t keycode;   // usage in usage_page; keyboard usages are page 7
    mx_u32_t modifiers; // modifier bitmask (shift/ctrl/alt/gui)
    mx_u32_t unicode;   // Unicode code point for INPUT_TEXT, else 0
    mx_u64_t source = 0;     // opaque interface token, never a USB bus address
    mx_u64_t generation = 0; // source producer incarnation
    mx_u64_t sequence = 0;   // monotonically increases per source/incarnation
    mx_u32_t usage_page = 7;
    mx_u32_t reserved = 0;
};
static_assert(sizeof(InputEvent) <= MX_RING_INLINE_PAYLOAD, "input event must fit one payload");
static_assert(sizeof(InputEvent) == 48, "input event size drift");

// LED usage 1 = Num Lock, 2 = Caps Lock, 3 = Scroll Lock. A source of zero
// updates every keyboard owned by this sink; it grants no controller authority.
struct InputLedCommand {
    mx_u64_t source;
    mx_u32_t mask;
    mx_u32_t reserved;
};
static_assert(sizeof(InputLedCommand) <= MX_RING_INLINE_PAYLOAD);
