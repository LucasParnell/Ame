// userland/drivers/input/main.cc — ui.input normalization service (M6).
//
// Accepts raw key InputEvents from drv.usb.hid, normalizes US boot-keyboard
// layout to text events, and forwards to a subscriber ring (V0: tty.fb0).
//
// Protocol: INPUT_MSG_SUBSCRIBE to register, INPUT_MSG_EVENT pushed to subscriber.
// Cap policy: drv.usb.hid read ring + tty.fb0 write ring.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/input.h"
#include "mochi/va_layout.h"

using namespace mochi::va::input;
constexpr uint32_t kRingOrder = MX_DEFAULT_RING_ORDER;

static mochi::input::EventSource g_hid{};
static mochi::input::EventSink g_tty{};

// USB HID boot keyboard usage ID -> US ASCII, unshifted and shifted.
static const char kHidToAscii[128] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
    'y', 'z', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\n', 0,
    '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/',
};

static const char kHidToAsciiShifted[128] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
    'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '\n', 0,
    '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
};

static char ascii_for_key(uint32_t keycode, uint32_t modifiers) {
    if (keycode >= 128) return 0;
    const bool shifted = (modifiers & (INPUT_MOD_LSHIFT | INPUT_MOD_RSHIFT)) != 0;
    return shifted ? kHidToAsciiShifted[keycode] : kHidToAscii[keycode];
}

static char control_for_key(uint32_t keycode, uint32_t modifiers) {
    const bool ctrl = (modifiers & (INPUT_MOD_LCTRL | INPUT_MOD_RCTRL)) != 0;
    if (!ctrl) return 0;
    if (keycode >= 4 && keycode <= 29) {
        return static_cast<char>(1 + (keycode - 4)); // Ctrl-A..Ctrl-Z
    }
    return 0;
}

static bool send_to_tty(const InputEvent& event) {
    return g_tty.send_spinning(event);
}

static bool forward_input_event(const InputEvent& event) {
    bool sent = send_to_tty(event);
    if (event.type != INPUT_KEY_DOWN) return sent;

    const char control = control_for_key(event.keycode, event.modifiers);
    if (control != 0) {
        const InputEvent text{
            INPUT_TEXT,
            event.keycode,
            event.modifiers,
            static_cast<uint32_t>(static_cast<unsigned char>(control)),
        };
        return send_to_tty(text) || sent;
    }

    const bool ctrl = (event.modifiers & (INPUT_MOD_LCTRL | INPUT_MOD_RCTRL)) != 0;
    const bool alt = (event.modifiers & (INPUT_MOD_LALT | INPUT_MOD_RALT)) != 0;
    if (ctrl || alt) return sent;

    const char ascii = ascii_for_key(event.keycode, event.modifiers);
    if (ascii == 0) return sent;

    const InputEvent text{
        INPUT_TEXT,
        event.keycode,
        event.modifiers,
        static_cast<uint32_t>(static_cast<unsigned char>(ascii)),
    };
    return send_to_tty(text) || sent;
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "ui.input");

    const mx_handle_t hid_ring = mochi::boot::handle(boot, "hid.input.ring");
    if (hid_ring != MX_NULL_HANDLE &&
        mx_map_page(hid_ring, kVaHidRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_hid.ring = reinterpret_cast<RingPage*>(kVaHidRing);
    }
    g_hid.order = kRingOrder;
    g_hid.doorbell = mochi::boot::handle(boot, "hid.input.doorbell");

    const mx_handle_t tty_ring = mochi::boot::handle(boot, "input.tty.ring");
    if (tty_ring != MX_NULL_HANDLE &&
        mx_map_page(tty_ring, kVaTtyRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_tty.ring = reinterpret_cast<RingPage*>(kVaTtyRing);
    }
    g_tty.order = kRingOrder;
    g_tty.doorbell = mochi::boot::handle(boot, "input.tty.doorbell");

    mochi::log_raw_marker("ui.input: ready\n");

    mochi::boot::ready(boot);
    while (true) {
        if (!g_hid.valid()) {
            mx_yield();
            continue;
        }

        InputEvent event{};
        while (g_hid.try_read(&event)) {
            (void)forward_input_event(event);
        }
        if (g_hid.wait_read(&event)) {
            (void)forward_input_event(event);
        } else {
            mx_yield();
        }
    }
}
