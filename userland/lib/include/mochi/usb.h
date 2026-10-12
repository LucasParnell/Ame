#pragma once
// mochi/usb.h — USB boot-keyboard report endpoints over a mochi::Ring.
//
// The wire contract (opcode, report layout) lives in the ABI header
// midix/abi/protocols/usb.h; these are the ergonomic endpoints the xHCI driver
// and the HID decoder use.

#include "midix/abi/protocols/usb.h"
#include "mochi/message.h"
#include "mochi/ring.h"
#include "mochi/endpoint.h"
#include "midix/abi/protocols/usb_transfer.h"

namespace mochi::usb {

// Scoped class -> controller transfer endpoint. Setup/delegation is cold;
// active try_* operations use the ordinary resident SPSC ring contract.
struct TransferClient : SpscEndpoint {
    bool try_submit(const MxUsbSubmit& request) const {
        return valid() && tx().try_push_msg(MX_USB_TRANSFER_SUBMIT, request);
    }
    bool try_control(const MxUsbControlSubmit& request) const {
        return valid() && tx().try_push_msg(MX_USB_TRANSFER_CONTROL, request);
    }
    bool try_cancel(mx_u64_t request_id) const {
        return valid() && tx().try_push_msg(MX_USB_TRANSFER_CANCEL, MxUsbCancel{request_id});
    }
    bool try_completion(MxUsbCompletion* out) const {
        return valid() && rx().try_pop_msg(MX_USB_TRANSFER_COMPLETE, out);
    }
};

struct TransferServer : SpscEndpoint {
    bool try_complete(const MxUsbCompletion& completion) const {
        return valid() && tx().try_push_msg(MX_USB_TRANSFER_COMPLETE, completion);
    }
};

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
        return try_pop_msg(USB_MSG_BOOT_KEYBOARD_REPORT, out);
    }

    bool wait_read(UsbBootKeyboardReport* out) const {
        return wait_pop_msg(USB_MSG_BOOT_KEYBOARD_REPORT, out);
    }
};

} // namespace mochi::usb
