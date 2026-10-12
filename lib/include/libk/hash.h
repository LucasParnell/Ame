#pragma once
// Non-cryptographic checksums for caller-owned byte ranges. CRC state is kept
// before the final complement, allowing a stream to be folded across buffers.
#include "libk/bytes.h"

namespace libk {

inline constexpr std::uint32_t kCrc32IeeeInit = 0xFFFFFFFFu;
inline constexpr std::uint32_t kFnv1a32Basis = 2166136261u;

inline std::uint32_t crc32_ieee_update(std::uint32_t state, const_bytes data) {
    if (!data.data) return state;
    for (std::size_t i = 0; i < data.size; ++i) {
        state ^= data.data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = std::uint32_t{0} - (state & 1u);
            state = (state >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return state;
}

inline std::uint32_t crc32_ieee(const_bytes data) {
    return ~crc32_ieee_update(kCrc32IeeeInit, data);
}

inline std::uint32_t fnv1a32(const_bytes data, std::uint32_t state = kFnv1a32Basis) {
    if (!data.data) return state;
    for (std::size_t i = 0; i < data.size; ++i) {
        state = (state ^ data.data[i]) * 16777619u;
    }
    return state;
}

} // namespace libk
