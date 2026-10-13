#pragma once
// Composition policy, not controller register knowledge. The development PID
// is pid.codes' shared test PID; use an allocated PID for a distributed product.
#include "midix/abi/types.h"
#include "libk/bytes.h"
#include "libk/str.h"
#include "libk/endian.h"
namespace mochi::usb::gadget {
inline libk::const_bytes device_descriptor() {
    static constexpr mx_u8_t bytes[] = {18,   1, 0, 2, 0xef, 2, 1, 64, 0x09,
                                        0x12, 1, 0, 0, 1,    1, 2, 3,  1};
    return libk::as_bytes(bytes);
}
inline libk::const_bytes qualifier() {
    static constexpr mx_u8_t bytes[] = {10, 6, 0, 2, 0xef, 2, 1, 64, 1, 0};
    return libk::as_bytes(bytes);
}
// IAD + CDC control/data + independent vendor bulk interface. Endpoint
// assignment belongs to this composition, never to the DWC2 class.
inline mx_u32_t configuration(libk::bytes destination, bool high, bool other = false) {
    mx_u8_t bytes[] = {
        9,    2,    98, 0, 3, 1, 0, 0x80, 50, 8,    11,   0, 2,    2,    2, 1, 0, 9,    4, 0,
        0,    1,    2,  2, 1, 0, 5, 0x24, 0,  0x10, 1,    5, 0x24, 1,    0, 1, 4, 0x24, 2, 2,
        5,    0x24, 6,  0, 1, 7, 5, 0x83, 3,  16,   0,    9, 9,    4,    1, 0, 2, 0x0a, 0, 0,
        0,    7,    5,  1, 2, 0, 2, 0,    7,  5,    0x82, 2, 0,    2,    0, 9, 4, 2,    0, 2,
        0xff, 0,    0,  0, 7, 5, 4, 2,    0,  2,    0,    7, 5,    0x85, 2, 0, 2, 0,
    };
    static_assert(sizeof(bytes) == 98);
    bytes[1] = other ? 7 : 2;
    // Descriptor walkers make packet-size patching independent of byte offsets.
    for (mx_u32_t at = 0; at < sizeof(bytes); at += bytes[at]) {
        if (bytes[at + 1] == 5 && bytes[at + 3] == 2) {
            libk::write_le<mx_u16_t>(libk::as_bytes(bytes + at + 4, sizeof(mx_u16_t)),
                                     high ? 512 : 64);
        }
    }
    return libk::copy_n(destination, libk::as_bytes(bytes), sizeof(bytes)) ? sizeof(bytes) : 0;
}
inline mx_u32_t string_descriptor(libk::bytes destination, mx_u8_t index) {
    if (index == 0) {
        static constexpr mx_u8_t language[] = {4, 3, 9, 4};
        return libk::copy_n(destination, libk::as_bytes(language), 4) ? 4 : 0;
    }
    const char* text = index == 1   ? "Ame"
                       : index == 2 ? "Ame USB diagnostics"
                       : index == 3 ? "anne-native-01"
                                    : nullptr;
    if (!text) {
        return 0;
    }
    const auto count = libk::str::length(text), length = 2 + count * 2;
    if (length > destination.size || length > 255) {
        return 0;
    }
    destination.data[0] = length;
    destination.data[1] = 3;
    for (mx_u32_t i = 0; i < count; ++i) {
        destination.data[2 + i * 2] = text[i];
        destination.data[3 + i * 2] = 0;
    }
    return length;
}
} // namespace mochi::usb::gadget
