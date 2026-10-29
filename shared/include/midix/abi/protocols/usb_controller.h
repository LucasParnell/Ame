#pragma once
#include "midix/abi/protocols/usb_transfer.h"

constexpr mx_u32_t MX_PROTOCOL_USB_CONTROLLER = 0x55534331u; // USC1
enum MxUsbControllerMsg : mx_u32_t {
    MX_USB_CONTROLLER_NEGOTIATE = 0x100, MX_USB_CONTROLLER_PORT_STATUS = 0x101,
    MX_USB_CONTROLLER_RESET_PORT = 0x102, MX_USB_CONTROLLER_ADDRESS = 0x103,
    MX_USB_CONTROLLER_CONFIGURE_ENDPOINT = 0x104, MX_USB_CONTROLLER_INSTALL_GRANT = 0x105,
    MX_USB_CONTROLLER_REVOKE_GRANT = 0x106, MX_USB_CONTROLLER_QUIESCE = 0x107,
    MX_USB_CONTROLLER_ACTIVATE_ROLE = 0x108, MX_USB_CONTROLLER_EVENT = 0x109,
    MX_USB_CONTROLLER_REPLY = 0x10a, MX_USB_CONTROLLER_EP0_PACKET = 0x10b,
    MX_USB_CONTROLLER_REMOVE_DEVICE = 0x10c,
};
enum MxUsbControllerFeatures : mx_u32_t {
    MX_USB_FEATURE_HOST = 1u << 0, MX_USB_FEATURE_DEVICE = 1u << 1,
    MX_USB_FEATURE_DIRECT_DMA = 1u << 2, MX_USB_FEATURE_SCATTER_GATHER = 1u << 3,
    MX_USB_FEATURE_ISOCHRONOUS = 1u << 4, MX_USB_FEATURE_HUBS = 1u << 5,
    MX_USB_FEATURE_SPLIT_TRANSACTIONS = 1u << 6,
};
struct MxUsbControllerInfo {
    mx_u16_t version;
    mx_u16_t size;
    mx_u32_t features;
    mx_u64_t generation;
    mx_u32_t speed_mask;
    mx_u32_t transfer_mask;
    mx_u32_t max_transfer_bytes;
    mx_u16_t max_devices;
    mx_u16_t max_endpoints;
    mx_u16_t max_inflight;
    mx_u16_t root_ports;
    mx_u32_t reserved[3];
};
struct MxUsbEndpointConfig {
    mx_u64_t request_id;
    MxUsbToken device;
    mx_u16_t max_packet;
    mx_u16_t max_esit_payload;
    mx_u8_t address;
    MxUsbTransferType type;
    mx_u8_t interval;
    mx_u8_t max_burst;
    mx_u8_t mult;
    mx_u8_t reserved[7];
};
static_assert(sizeof(MxUsbControllerInfo) == MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxUsbEndpointConfig) <= MX_RING_INLINE_PAYLOAD);

// Cold controller operations and port events; data-plane submissions use UST1.
struct MxUsbControllerRequest {
    mx_u64_t request_id;
    MxUsbToken device;
    mx_u32_t offset;
    mx_u32_t length;
    mx_u32_t value;
    mx_u32_t reserved;
};
struct MxUsbControllerReply {
    mx_u64_t request_id;
    MxUsbToken device;
    MxUsbToken endpoint;
    mx_status_t status;
    mx_u32_t value;
    MxUsbSpeed speed;
    mx_u8_t reserved[7];
};
struct MxUsbPortEvent {
    mx_u64_t sequence;
    mx_u16_t port;
    MxUsbSpeed speed;
    mx_u8_t connected;
    mx_u32_t reserved;
};
static_assert(sizeof(MxUsbControllerReply) <= MX_RING_INLINE_PAYLOAD);
