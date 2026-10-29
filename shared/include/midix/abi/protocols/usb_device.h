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

// Read-only controller-owned endpoint observation. No caller-supplied register
// address or memory range is accepted; the request selects one endpoint row.
constexpr mx_u32_t MX_DEVICE_OBSERVE = 0x5544500b, MX_DEVICE_SNAPSHOT = 0x5544500c;
// OBSERVE value 1 selects EP0's retained pre-diagnostic control trace. Its eight
// words are: SETUPDONE OUT control/size, last RX status, RECEIVE OUT control/size,
// software flags, total DATA OUT bytes, and type|request<<8|length<<16.
// Software flag bit 9 marks post-trial RECEIVE registers; bits 10:11 hold the
// consumed trial mode. Without bit 9, RECEIVE registers precede programming.
constexpr mx_u32_t MX_DEVICE_OBSERVE_CONTROL_HISTORY = 1;
// Explicit development trial, separate from read-only OBSERVE. Value 0 cancels;
// 1 clears NAK, 2 programs a full OUT window then clears NAK. Only the next
// nonempty EP0 RECEIVE is affected; reset cancels it. No register address or
// arbitrary value is accepted. The reply is a SNAPSHOT with flags=armed mode.
constexpr mx_u32_t MX_DEVICE_EP0_TRIAL = 0x5544500d;
struct MxDeviceSnapshot {
    mx_u64_t id, generation;
    mx_u32_t control, interrupt, transfer_size, global_interrupt;
    mx_u32_t flags, length, actual, tx_bytes;
};
static_assert(sizeof(MxDeviceSnapshot) == 48);
