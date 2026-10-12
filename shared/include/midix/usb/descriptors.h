#pragma once
// Pure descriptor validation into caller-owned flat tables. No allocation,
// class binding or hardware access. Unknown descriptors remain in the raw blob.
#include "libk/bytes.h"
#include "libk/endian.h"
#include "midix/abi/protocols/usb_transfer.h"

namespace midix::usb {

constexpr mx_u8_t kDescriptorDevice = 1;
constexpr mx_u8_t kDescriptorConfiguration = 2;
constexpr mx_u8_t kDescriptorInterface = 4;
constexpr mx_u8_t kDescriptorEndpoint = 5;
constexpr mx_u8_t kDescriptorHid = 0x21;
constexpr mx_u8_t kDescriptorReport = 0x22;
constexpr mx_u8_t kDescriptorSuperSpeedCompanion = 0x30;

struct DeviceDescriptor {
    mx_u16_t usb_version;
    mx_u16_t vendor;
    mx_u16_t product;
    mx_u16_t revision;
    mx_u16_t ep0_max_packet;
    mx_u8_t device_class;
    mx_u8_t subclass;
    mx_u8_t protocol;
    mx_u8_t configurations;
};

inline bool valid_speed(MxUsbSpeed speed) {
    return speed >= MxUsbSpeed::Low && speed <= MxUsbSpeed::SuperPlus;
}

inline bool parse_device(libk::const_bytes raw, MxUsbSpeed speed, DeviceDescriptor* out) {
    if (!out || !valid_speed(speed) || !raw.data || raw.size < 18 || raw.data[0] != 18 ||
        raw.data[1] != kDescriptorDevice || raw.data[17] == 0) return false;
    const auto encoded = raw.data[7];
    mx_u16_t packet = encoded;
    if (speed >= MxUsbSpeed::Super) {
        if (encoded != 9) return false;
        packet = 512;
    } else if (speed == MxUsbSpeed::Low) {
        if (encoded != 8) return false;
    } else if (speed == MxUsbSpeed::High) {
        if (encoded != 64) return false;
    } else if (encoded != 8 && encoded != 16 && encoded != 32 && encoded != 64) return false;
    *out = {libk::load_le<mx_u16_t>(raw.data + 2), libk::load_le<mx_u16_t>(raw.data + 8),
            libk::load_le<mx_u16_t>(raw.data + 10), libk::load_le<mx_u16_t>(raw.data + 12),
            packet, raw.data[4], raw.data[5], raw.data[6], raw.data[17]};
    return true;
}

struct InterfaceDescriptor {
    mx_u32_t first_endpoint;
    mx_u16_t hid_report_bytes;
    mx_u8_t number;
    mx_u8_t alternate;
    mx_u8_t endpoint_count;
    mx_u8_t interface_class;
    mx_u8_t subclass;
    mx_u8_t protocol;
};
struct EndpointDescriptor {
    mx_u32_t interface_index;
    mx_u16_t max_packet;
    mx_u16_t bytes_per_interval;
    mx_u8_t address;
    MxUsbTransferType type;
    mx_u8_t interval;
    mx_u8_t transactions;
    mx_u8_t max_burst;
    mx_u8_t companion_attributes;
    bool has_companion;
};
struct Configuration {
    InterfaceDescriptor* interfaces;
    EndpointDescriptor* endpoints;
    mx_u32_t interface_capacity;
    mx_u32_t endpoint_capacity;
    mx_u32_t interface_count;
    mx_u32_t endpoint_count;
    mx_u16_t total_bytes;
    mx_u8_t value;
    mx_u8_t attributes;
    mx_u8_t max_power;
};

inline bool endpoint_valid(const EndpointDescriptor& ep, MxUsbSpeed speed) {
    if (!valid_speed(speed) || ep.type > MxUsbTransferType::Interrupt ||
        (ep.address & 0x70u) || !(ep.address & 0x0fu) || !ep.max_packet ||
        ep.type == MxUsbTransferType::Control) return false;
    const bool periodic = ep.type == MxUsbTransferType::Interrupt ||
                          ep.type == MxUsbTransferType::Isochronous;
    if (speed == MxUsbSpeed::Low)
        return ep.type == MxUsbTransferType::Interrupt && ep.max_packet <= 8 && ep.interval != 0 && ep.transactions == 1;
    if (speed == MxUsbSpeed::Full) {
        if (ep.transactions != 1) return false;
        if (ep.type == MxUsbTransferType::Bulk)
            return ep.max_packet == 8 || ep.max_packet == 16 || ep.max_packet == 32 || ep.max_packet == 64;
        return ep.interval != 0 && (ep.type == MxUsbTransferType::Interrupt ? ep.max_packet <= 64 :
                                   ep.max_packet <= 1023 && ep.interval <= 16);
    }
    if (speed == MxUsbSpeed::High) {
        if (!periodic) return ep.max_packet == 512 && ep.transactions == 1;
        return ep.max_packet <= 1024 && ep.transactions <= 3 && ep.interval >= 1 && ep.interval <= 16;
    }
    return ep.transactions == 1 && ep.max_packet <= 1024 &&
           (ep.type != MxUsbTransferType::Bulk || ep.max_packet == 1024) &&
           (!periodic || (ep.interval >= 1 && ep.interval <= 16));
}

inline bool parse_configuration(libk::const_bytes raw, MxUsbSpeed speed, Configuration* out) {
    if (!out) return false;
    out->interface_count = out->endpoint_count = 0;
    if (!valid_speed(speed) || !raw.data || raw.size < 9 || raw.data[0] != 9 ||
        raw.data[1] != kDescriptorConfiguration || !raw.data[4] || !raw.data[5] ||
        (raw.data[7] & 0x9fu) != 0x80u) return false;
    const auto total = libk::load_le<mx_u16_t>(raw.data + 2);
    if (total != raw.size || !out->interfaces || !out->endpoints) return false;

    mx_u32_t interfaces = 0, endpoints = 0, unique_interfaces = 0;
    mx_u32_t current = ~mx_u32_t{0};
    mx_u32_t seen_endpoints = 0;
    mx_u8_t interface_seen[256]{};
    mx_u8_t interface_default[256]{};
    mx_u32_t previous_type = 0;
    for (mx_u32_t offset = 9; offset < total;) {
        if (total - offset < 2) return false;
        const auto* bytes = raw.data + offset;
        const mx_u32_t length = bytes[0], type = bytes[1];
        if (length < 2 || length > total - offset) return false;
        if (type == kDescriptorDevice || type == kDescriptorConfiguration) return false;
        if (type == kDescriptorInterface) {
            if (length != 9 || interfaces == out->interface_capacity ||
                (current != ~mx_u32_t{0} && seen_endpoints != out->interfaces[current].endpoint_count))
                return false;
            for (mx_u32_t i = 0; i < interfaces; ++i)
                if (out->interfaces[i].number == bytes[2] && out->interfaces[i].alternate == bytes[3])
                    return false;
            current = interfaces++;
            out->interfaces[current] = {endpoints, 0, bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]};
            if (!interface_seen[bytes[2]]) { interface_seen[bytes[2]] = 1; ++unique_interfaces; }
            if (bytes[3] == 0) interface_default[bytes[2]] = 1;
            seen_endpoints = 0;
        } else if (type == kDescriptorEndpoint) {
            if ((length != 7 && length != 9) || current == ~mx_u32_t{0} ||
                endpoints == out->endpoint_capacity ||
                seen_endpoints == out->interfaces[current].endpoint_count) return false;
            const auto packet = libk::load_le<mx_u16_t>(bytes + 4);
            if ((packet & 0xe000u) || ((packet >> 11) & 3u) == 3u) return false;
            EndpointDescriptor ep{};
            ep.interface_index = current;
            ep.address = bytes[2];
            ep.type = static_cast<MxUsbTransferType>(bytes[3] & 3u);
            ep.max_packet = packet & 0x7ffu;
            ep.transactions = static_cast<mx_u8_t>(1 + ((packet >> 11) & 3u));
            ep.interval = bytes[6];
            if (!endpoint_valid(ep, speed)) return false;
            for (mx_u32_t i = 0; i < endpoints; ++i) {
                const auto& previous = out->endpoints[i];
                const auto& owner = out->interfaces[previous.interface_index];
                if (previous.address == ep.address &&
                    (previous.interface_index == current || owner.number != out->interfaces[current].number))
                    return false;
            }
            out->endpoints[endpoints++] = ep;
            ++seen_endpoints;
        } else if (type == kDescriptorSuperSpeedCompanion) {
            if (length != 6 || speed < MxUsbSpeed::Super || previous_type != kDescriptorEndpoint ||
                !endpoints || bytes[2] > 15) return false;
            auto& ep = out->endpoints[endpoints - 1];
            if (ep.type == MxUsbTransferType::Bulk) {
                if ((bytes[3] & 0xe0u) || (bytes[3] & 0x1fu) > 16 || bytes[4] || bytes[5]) return false;
            } else if ((bytes[3] & 0xfcu) || (bytes[3] & 3u) > 2 ||
                       (ep.type == MxUsbTransferType::Interrupt && bytes[3] != 0)) return false;
            ep.max_burst = bytes[2];
            ep.companion_attributes = bytes[3];
            ep.bytes_per_interval = libk::load_le<mx_u16_t>(bytes + 4);
            if (ep.type != MxUsbTransferType::Bulk) {
                const auto mult = ep.type == MxUsbTransferType::Isochronous ? 1u + (bytes[3] & 3u) : 1u;
                const auto capacity = static_cast<mx_u32_t>(ep.max_packet) * (ep.max_burst + 1u) * mult;
                if (!ep.bytes_per_interval || ep.bytes_per_interval > capacity) return false;
            }
            ep.has_companion = true;
        } else if (type == kDescriptorHid && current != ~mx_u32_t{0} &&
                   out->interfaces[current].interface_class == 3) {
            if (length < 6 || bytes[5] == 0 || length != 6u + 3u * bytes[5] ||
                out->interfaces[current].hid_report_bytes) return false;
            for (mx_u32_t i = 0; i < bytes[5]; ++i) {
                const auto at = 6 + i * 3;
                if (bytes[at] != kDescriptorReport) continue;
                if (out->interfaces[current].hid_report_bytes) return false;
                out->interfaces[current].hid_report_bytes = libk::load_le<mx_u16_t>(bytes + at + 1);
                if (!out->interfaces[current].hid_report_bytes) return false;
            }
        }
        previous_type = type;
        offset += length;
    }
    if (current == ~mx_u32_t{0} || seen_endpoints != out->interfaces[current].endpoint_count ||
        unique_interfaces != raw.data[4]) return false;
    for (mx_u32_t i = 0; i < 256; ++i)
        if (interface_seen[i] && !interface_default[i]) return false;
    if (speed >= MxUsbSpeed::Super)
        for (mx_u32_t i = 0; i < endpoints; ++i) if (!out->endpoints[i].has_companion) return false;
    out->interface_count = interfaces;
    out->endpoint_count = endpoints;
    out->total_bytes = total;
    out->value = raw.data[5];
    out->attributes = raw.data[7];
    out->max_power = raw.data[8];
    return true;
}

} // namespace midix::usb
