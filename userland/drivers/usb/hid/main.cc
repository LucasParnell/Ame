// userland/drivers/usb/hid/main.cc — drv.usb.hid service (M5).
//
// Polls xHCI interrupt-IN endpoint for USB HID boot keyboard reports.
// Decodes 8-byte boot keyboard reports into raw key up/down InputEvents.
// Text/layout conversion belongs to ui.input.
//
// Cap policy: USB interface endpoint from drv.usb.xhci + ui.input write ring.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/input.h"
#include "mochi/usb.h"
#include "mochi/va_layout.h"

using namespace mochi::va::hid;
constexpr uint32_t kRingOrder = MX_DEFAULT_RING_ORDER;

static mochi::usb::BootKeyboardReportSource g_xhci{};
static mochi::input::EventSink g_input{};
static UsbBootKeyboardReport g_prev{};

static bool key_present(const uint8_t keys[6], uint8_t key) {
    if (key == 0 || key == 1) return true;
    for (uint32_t i = 0; i < 6; ++i) {
        if (keys[i] == key) return true;
    }
    return false;
}

static bool send_input_event(uint32_t type, uint32_t keycode,
                             uint32_t modifiers, uint32_t unicode) {
    const InputEvent event{type, keycode, modifiers, unicode};
    return g_input.send_spinning(event);
}

static void emit_modifier_changes(uint8_t prev, uint8_t current) {
    const uint8_t changed = static_cast<uint8_t>(prev ^ current);
    for (uint32_t bit = 0; bit < 8; ++bit) {
        const uint8_t mask = static_cast<uint8_t>(1u << bit);
        if ((changed & mask) == 0) continue;
        const uint32_t type = (current & mask) ? INPUT_KEY_DOWN : INPUT_KEY_UP;
        (void)send_input_event(type, 0xE0u + bit, current, 0);
    }
}

static void handle_report(const UsbBootKeyboardReport& report) {
    emit_modifier_changes(g_prev.modifiers, report.modifiers);

    for (uint32_t i = 0; i < 6; ++i) {
        const uint8_t key = g_prev.keys[i];
        if (key != 0 && key != 1 && !key_present(report.keys, key)) {
            (void)send_input_event(INPUT_KEY_UP, key, report.modifiers, 0);
        }
    }

    for (uint32_t i = 0; i < 6; ++i) {
        const uint8_t key = report.keys[i];
        if (key == 0 || key == 1 || key_present(g_prev.keys, key)) continue;
        (void)send_input_event(INPUT_KEY_DOWN, key, report.modifiers, 0);
    }

    g_prev = report;
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "drv.usb.hid");

    const mx_handle_t xhci_ring = mochi::boot::handle(boot, "xhci.hid.ring");
    if (xhci_ring != MX_NULL_HANDLE &&
        mx_map_page(xhci_ring, kVaXhciRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_xhci.ring = reinterpret_cast<RingPage*>(kVaXhciRing);
    }
    g_xhci.order = kRingOrder;
    g_xhci.doorbell = mochi::boot::handle(boot, "xhci.hid.doorbell");

    const mx_handle_t input_ring = mochi::boot::handle(boot, "hid.input.ring");
    if (input_ring != MX_NULL_HANDLE &&
        mx_map_page(input_ring, kVaInputRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_input.ring = reinterpret_cast<RingPage*>(kVaInputRing);
    }
    g_input.order = kRingOrder;
    g_input.doorbell = mochi::boot::handle(boot, "hid.input.doorbell");

    mochi::log_raw_marker("drv.usb.hid: ready\n");

    mochi::boot::ready(boot);
    while (true) {
        if (!g_xhci.valid()) {
            mx_yield();
            continue;
        }

        UsbBootKeyboardReport report{};
        while (g_xhci.try_read(&report)) {
            handle_report(report);
        }
        if (g_xhci.wait_read(&report)) {
            handle_report(report);
        } else {
            mx_yield();
        }
    }
}
