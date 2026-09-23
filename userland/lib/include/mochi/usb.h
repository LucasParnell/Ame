#pragma once
// mochi/usb.h — USB boot-keyboard report endpoints over a mochi::Ring.
//
// The wire contract (opcode, report layout) lives in the ABI header
// midix/abi/protocols/usb.h; these are the ergonomic endpoints the xHCI driver
// and the HID decoder use.

#include "midix/abi/protocols/usb.h"
#include "mochi/message.h"
#include "mochi/ring.h"

namespace mochi::usb {

// xHCI driver -> HID decoder.
struct BootKeyboardReportSink : Ring {
    bool try_send(const UsbBootKeyboardReport& report) const {
        return try_push_msg(USB_MSG_BOOT_KEYBOARD_REPORT, report);
    }

    bool send_spinning(const UsbBootKeyboardReport& report, uint32_t max_yields = 64) const {
        return valid() && push_msg(USB_MSG_BOOT_KEYBOARD_REPORT, report, max_yields);
    }
};

// HID decoder <- xHCI driver.
struct BootKeyboardReportSource : Ring {
    bool try_read(UsbBootKeyboardReport* out) const {
        if (!out) return false;
        RingSlot slot{};
        return try_pop(&slot) && message_get(slot, out, USB_MSG_BOOT_KEYBOARD_REPORT);
    }

    bool wait_read(UsbBootKeyboardReport* out) const {
        if (!out || !valid()) return false;
        for (;;) {
            RingSlot slot{};
            if (!wait_pop(&slot)) return false;
            if (message_get(slot, out, USB_MSG_BOOT_KEYBOARD_REPORT)) return true;
        }
    }
};

} // namespace mochi::usb
