#pragma once
#include "midix/abi/protocols/usb_transfer.h"

constexpr mx_u32_t MX_PROTOCOL_USB_HOST = 0x55534831u; // USH1
enum MxUsbHostMsg : mx_u32_t {
    MX_USB_HOST_NEGOTIATE = 1, MX_USB_HOST_LIST = 2, MX_USB_HOST_WATCH = 3,
    MX_USB_HOST_DESCRIPTORS = 4, MX_USB_HOST_CLAIM = 5, MX_USB_HOST_RELEASE = 6,
    MX_USB_HOST_OPEN_ENDPOINT = 7, MX_USB_HOST_CLOSE_ENDPOINT = 8,
    MX_USB_HOST_SET_ALTERNATE = 9, MX_USB_HOST_CLEAR_HALT = 10,
    MX_USB_HOST_EVENT = 11, MX_USB_HOST_REPLY = 12,
    MX_USB_HOST_CONTROL = 13, MX_USB_HOST_SUBMIT = 14, MX_USB_HOST_COMPLETE = 15,
    MX_USB_HOST_NEGOTIATE_REPLY = 16,
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

// Offers describe interfaces; class selection is entirely in class drivers.
struct MxUsbInterfaceOffer {
    MxUsbToken interface;
    MxUsbToken device;
    mx_u64_t sequence;
    mx_u16_t descriptor_bytes;
    mx_u8_t number;
    mx_u8_t alternate;
    MxUsbSpeed speed;
    mx_u8_t interface_class;
    mx_u8_t subclass;
    mx_u8_t protocol;
    MxUsbDeviceEvent event;
    mx_u32_t reserved;
};
struct MxUsbHostRequest {
    mx_u64_t request_id;
    MxUsbToken interface;
    mx_u32_t offset;
    mx_u32_t length;
    mx_u32_t value;
    mx_u32_t reserved;
};
struct MxUsbHostReply {
    mx_u64_t request_id;
    MxUsbToken token;
    mx_status_t status;
    mx_u32_t length;
};
static_assert(sizeof(MxUsbInterfaceOffer) <= MX_RING_INLINE_PAYLOAD);

// A claimed interface may issue class requests and standard read requests
// addressed to that interface. Descriptor types belong to the class driver;
// bus-wide mutations (address/configuration) and endpoint halt operations use
// the bus's dedicated lease-aware operations instead of this control path.
inline bool mx_usb_interface_control_allowed(const MxUsbSetup& setup, mx_u8_t number) {
    if ((setup.request_type & 0x1fu) != 1 || setup.index != number) return false;
    const auto type = setup.request_type & 0x60u;
    if (type == 0x20) return true;
    if (type != 0 || !(setup.request_type & 0x80u)) return false;
    if (setup.request == 6) return true; // GET_DESCRIPTOR, class-defined descriptor type
    if (setup.request == 0) return setup.value == 0 && setup.length == 2; // GET_STATUS
    return setup.request == 10 && setup.value == 0 && setup.length == 1; // GET_INTERFACE
}
