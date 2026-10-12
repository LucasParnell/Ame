#pragma once
#include "midix/abi/protocols/usb_transfer.h"

constexpr mx_u32_t MX_PROTOCOL_USB_CONTROLLER = 0x55534331u; // USC1
enum MxUsbControllerMsg : mx_u32_t {
    MX_USB_CONTROLLER_NEGOTIATE = 1, MX_USB_CONTROLLER_PORT_STATUS = 2,
    MX_USB_CONTROLLER_RESET_PORT = 3, MX_USB_CONTROLLER_ADDRESS = 4,
    MX_USB_CONTROLLER_CONFIGURE_ENDPOINT = 5, MX_USB_CONTROLLER_INSTALL_GRANT = 6,
    MX_USB_CONTROLLER_REVOKE_GRANT = 7, MX_USB_CONTROLLER_QUIESCE = 8,
    MX_USB_CONTROLLER_ACTIVATE_ROLE = 9, MX_USB_CONTROLLER_EVENT = 10,
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
