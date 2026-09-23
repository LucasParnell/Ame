#pragma once
// Midix USB driver-private protocol ABI.
//
// The boot-keyboard report the xHCI driver forwards to the HID decoder. Kept in
// the shared ABI like every other ring protocol so the producer and consumer
// cannot drift on the report layout.
//
// Transport envelope: MxRingSlot.

#include "midix/abi/types.h"
#include "midix/abi/ring.h"

constexpr mx_u32_t MX_PROTOCOL_USB = 0x55534200u; // 'USB\0'

using MxUsbFrame = MxRingSlot;
static_assert(sizeof(MxUsbFrame) == 64, "usb frame must be one ring slot");

enum UsbMsg : mx_u32_t {
    USB_MSG_BOOT_KEYBOARD_REPORT = 1,
};

struct UsbBootKeyboardReport {
    mx_u8_t modifiers;
    mx_u8_t reserved;
    mx_u8_t keys[6];
};
static_assert(sizeof(UsbBootKeyboardReport) <= MX_RING_INLINE_PAYLOAD,
              "keyboard report must fit one payload");
static_assert(sizeof(UsbBootKeyboardReport) == 8, "keyboard report size drift");
