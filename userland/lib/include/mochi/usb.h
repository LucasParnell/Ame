#pragma once
// mochi/usb.h - minimal USB driver-private protocols.

#include <cstdint>
#include "mochi/ring.h"

enum UsbMsg : uint32_t {
    USB_MSG_BOOT_KEYBOARD_REPORT = 1,
};

struct UsbBootKeyboardReport {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
};
static_assert(sizeof(UsbBootKeyboardReport) == 8);

namespace mochi::usb {

struct BootKeyboardReportSink {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring && doorbell != MX_NULL_HANDLE;
    }

    bool try_send(const UsbBootKeyboardReport& report) const {
        return valid() &&
               ring_try_push(ring, order, USB_MSG_BOOT_KEYBOARD_REPORT,
                             &report, sizeof(report), doorbell);
    }

    bool send_spinning(const UsbBootKeyboardReport& report,
                       uint32_t max_yields = 64) const {
        return valid() &&
               ring_send_spinning(ring, order, USB_MSG_BOOT_KEYBOARD_REPORT,
                                  &report, sizeof(report), doorbell, max_yields);
    }
};

struct BootKeyboardReportSource {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring && doorbell != MX_NULL_HANDLE;
    }

    bool try_read(UsbBootKeyboardReport* out) const {
        if (!out || !ring) return false;
        RingSlot slot{};
        if (!ring_pop(ring, order, &slot)) return false;
        if (slot.type != USB_MSG_BOOT_KEYBOARD_REPORT ||
            slot.length < sizeof(UsbBootKeyboardReport)) {
            return false;
        }
        const auto& report = *reinterpret_cast<const UsbBootKeyboardReport*>(slot.payload);
        *out = report;
        return true;
    }

    bool wait_read(UsbBootKeyboardReport* out) const {
        if (!out || !valid()) return false;
        for (;;) {
            RingSlot slot{};
            if (!ring_wait_pop(ring, order, &slot, doorbell)) return false;
            if (slot.type == USB_MSG_BOOT_KEYBOARD_REPORT &&
                slot.length >= sizeof(UsbBootKeyboardReport)) {
                const auto& report = *reinterpret_cast<const UsbBootKeyboardReport*>(slot.payload);
                *out = report;
                return true;
            }
        }
    }
};

} // namespace mochi::usb
