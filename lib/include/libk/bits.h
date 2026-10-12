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

} // namespace libk
