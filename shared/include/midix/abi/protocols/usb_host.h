#pragma once
#include "midix/abi/protocols/usb_transfer.h"

constexpr mx_u32_t MX_PROTOCOL_USB_HOST = 0x55534831u; // USH1
enum MxUsbHostMsg : mx_u32_t {
    MX_USB_HOST_NEGOTIATE = 1, MX_USB_HOST_LIST = 2, MX_USB_HOST_WATCH = 3,
    MX_USB_HOST_DESCRIPTORS = 4, MX_USB_HOST_CLAIM = 5, MX_USB_HOST_RELEASE = 6,
    MX_USB_HOST_OPEN_ENDPOINT = 7, MX_USB_HOST_CLOSE_ENDPOINT = 8,
    MX_USB_HOST_SET_ALTERNATE = 9, MX_USB_HOST_CLEAR_HALT = 10,
    MX_USB_HOST_EVENT = 11,
};
enum class MxUsbDeviceEvent : mx_u32_t { Attached = 1, Detached = 2, Reset = 3, Resnapshot = 4 };
struct MxUsbDeviceIdentity {
    MxUsbToken controller;
    MxUsbToken device;
    mx_u64_t topology_sequence;
    mx_u8_t port_path[8];
    mx_u8_t depth;
    MxUsbSpeed speed;
    mx_u8_t reserved[6];
};
struct MxUsbInterfaceClaim {
    mx_u64_t request_id;
    MxUsbToken device;
    mx_u8_t interface_number;
    mx_u8_t alternate;
    mx_u8_t reserved[6];
};
struct MxUsbTopologyEvent {
    MxUsbDeviceIdentity identity;
    MxUsbDeviceEvent event;
    mx_u32_t reserved;
};
static_assert(sizeof(MxUsbTopologyEvent) <= MX_RING_INLINE_PAYLOAD);
