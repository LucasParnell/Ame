#pragma once
#include "midix/abi/protocols/usb_transfer.h"
// PIO device controller packet contract. io.usb.gadget owns EP0 policy;
// functions own their streams. Each endpoint borrows its fixed arena slice
// until a terminal completion, bus reset, or role fence.
constexpr mx_u32_t MX_DEVICE_CONFIGURE = 0x55445001, MX_DEVICE_SEND = 0x55445002;
constexpr mx_u32_t MX_DEVICE_RECEIVE = 0x55445003, MX_DEVICE_STALL = 0x55445004;
constexpr mx_u32_t MX_DEVICE_ADDRESS = 0x55445005, MX_DEVICE_SETUP = 0x55445006;
// ADDRESS stages a seven-bit address for this EP0 setup id/generation. Send it
// before the zero-length status IN SEND. The backend owns hardware-specific
// register ordering; a new SETUP or reset discards an uncommitted stage.
constexpr mx_u32_t MX_DEVICE_COMPLETE = 0x55445007, MX_DEVICE_RESET = 0x55445008;
constexpr mx_u32_t MX_DEVICE_REPLY = 0x55445009, MX_DEVICE_DISCONNECT = 0x5544500a;
struct MxDevicePacket {
    mx_u64_t id, generation;
    mx_u32_t offset, length;
    mx_u16_t max_packet;
    mx_u8_t endpoint;
    MxUsbTransferType type;
    mx_u32_t value, reserved;
};
struct MxDeviceEvent {
    mx_u64_t id, generation;
    MxUsbStatus status;
    mx_u32_t length;
    mx_u8_t endpoint, speed;
    mx_u16_t reserved;
    MxUsbSetup setup;
};
static_assert(sizeof(MxDeviceEvent) <= MX_RING_INLINE_PAYLOAD);
