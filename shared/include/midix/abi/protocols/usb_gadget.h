#pragma once
#include "midix/abi/protocols/usb_transfer.h"

constexpr mx_u32_t MX_PROTOCOL_USB_GADGET = 0x55534731u; // USG1
enum MxUsbGadgetMsg : mx_u32_t {
    MX_USB_GADGET_REGISTER_FUNCTION = 1, MX_USB_GADGET_BIND = 2,
    MX_USB_GADGET_UNBIND = 3, MX_USB_GADGET_SETUP = 4,
    MX_USB_GADGET_SETUP_REPLY = 5, MX_USB_GADGET_EVENT = 6,
};
struct MxUsbGadgetSetup {
    mx_u64_t request_id;
    mx_u64_t generation;
    MxUsbSetup setup;
    MxUsbToken buffer;
    mx_u32_t offset;
    mx_u32_t reserved;
};
static_assert(sizeof(MxUsbGadgetSetup) <= MX_RING_INLINE_PAYLOAD);
