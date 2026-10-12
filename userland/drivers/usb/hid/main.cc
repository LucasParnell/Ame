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
#include "midix/usb/hid_boot.h"
#include "mochi/va_layout.h"

using namespace mochi::va::hid;

static mochi::usb::BootKeyboardReportSource g_xhci{};
static mochi::input::EventSink g_input{};
static midix::usb::BootKeyboardDecoder g_decoder{};

static void drain_input() {
    while (const auto* event = g_decoder.event()) {
        if (!g_input.try_send(*event)) return;
#ifdef MOCHI_USB_TRACE_INPUT
        mochi::log_counter(event->type == INPUT_KEY_DOWN ? "usb.hid.key_down" : "usb.hid.key_up", event->keycode);
#endif
        (void)g_decoder.advance();
    }
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "drv.usb.hid");

    g_xhci = {mochi::boot::map_ring(boot, {"xhci.hid.ring", kVaXhciRing, "xhci.hid.doorbell"})};

    g_input = {mochi::boot::map_ring(boot, {"hid.input.ring", kVaInputRing, "hid.input.doorbell"})};

    mochi::log_raw_marker("drv.usb.hid: ready\n");

    mochi::boot::ready(boot);
    while (true) {
        if (!g_xhci.valid()) {
            mx_yield();
            continue;
        }

        UsbBootKeyboardReport report{};
        drain_input();
        if (g_decoder.pending()) { mx_yield(); continue; }
        while (!g_decoder.pending() && g_xhci.try_read(&report)) {
            (void)g_decoder.prepare(libk::as_bytes(&report, sizeof(report)));
            drain_input();
        }
        if (g_decoder.pending()) { mx_yield(); continue; }
        if (g_xhci.wait_read(&report)) {
            (void)g_decoder.prepare(libk::as_bytes(&report, sizeof(report)));
            drain_input();
        } else {
            mx_yield();
        }
    }
}
