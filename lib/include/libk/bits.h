#pragma once
// Bounded, little-endian bit fields (USB HID, packed storage and wire formats).
#include "libk/bytes.h"
#include "libk/integer.h"

namespace libk {

inline bool read_bits(const_bytes source, std::uint64_t offset, std::uint32_t width,
                      std::uint64_t* out) {
    if (!out || width > 64) return false;
    if (width == 0) { *out = 0; return true; }
    std::uint64_t last = 0;
    if (!source.data || !checked_add(offset, width - 1, &last) || last / 8 >= source.size)
        return false;
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < width; ++i) {
        const auto bit = offset + i;
        value |= static_cast<std::uint64_t>((source.data[bit / 8] >> (bit % 8)) & 1u) << i;
    }
    *out = value;
    return true;
}

// Validate the entire field before changing any byte. Bits outside the field
// are preserved; a value that does not fit is rejected rather than truncated.
inline bool write_bits(bytes destination, std::uint64_t offset, std::uint32_t width,
                       std::uint64_t value) {
    if (width > 64 || (width < 64 && (value >> width))) return false;
    if (width == 0) return true;
    std::uint64_t last = 0;
    if (!destination.data || !checked_add(offset, width - 1, &last) ||
        last / 8 >= destination.size) return false;
    for (std::uint32_t i = 0; i < width; ++i) {
        const auto bit = offset + i;
        const auto mask = static_cast<std::uint8_t>(1u << (bit % 8));
        auto& byte = destination.data[bit / 8];
        byte = static_cast<std::uint8_t>((byte & ~mask) |
                                      (((value >> i) & 1u) ? mask : 0));
    }
    return true;
}

} // namespace libk
