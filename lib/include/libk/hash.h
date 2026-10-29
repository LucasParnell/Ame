#pragma once
// Non-cryptographic checksums for caller-owned byte ranges. CRC state is kept
// before the final complement, allowing a stream to be folded across buffers.
#include "libk/bytes.h"

namespace libk {

inline constexpr std::uint32_t kCrc32IeeeInit = 0xFFFFFFFFu;
inline constexpr std::uint32_t kFnv1a32Basis = 2166136261u;

namespace detail {
template <std::uint32_t Polynomial>
struct Crc32Table {
    std::uint32_t values[256]{};
    constexpr Crc32Table() {
        for (unsigned i = 0; i < 256; ++i) {
            auto value = static_cast<std::uint32_t>(i);
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (Polynomial & (0u - (value & 1u)));
            values[i] = value;
        }
    }
};
template <std::uint32_t Polynomial>
alignas(64) inline constexpr Crc32Table<Polynomial> crc32_table{};

// Slicing-by-8: eight derived tables let crc32_update consume eight bytes per
// iteration instead of one. Table k is the base table advanced by k zero bytes.
template <std::uint32_t Polynomial>
struct Crc32Tables {
    std::uint32_t values[8][256]{};
    constexpr Crc32Tables() {
        const Crc32Table<Polynomial> base{};
        for (unsigned i = 0; i < 256; ++i) values[0][i] = base.values[i];
        for (unsigned k = 1; k < 8; ++k)
            for (unsigned i = 0; i < 256; ++i) {
                const std::uint32_t prev = values[k - 1][i];
                values[k][i] = (prev >> 8) ^ base.values[prev & 255u];
            }
    }
};
template <std::uint32_t Polynomial>
alignas(64) inline constexpr Crc32Tables<Polynomial> crc32_tables{};

template <std::uint32_t Polynomial>
inline std::uint32_t crc32_byte(std::uint32_t state, std::uint8_t byte) {
    // One immutable 1 KiB column per polynomial. No initialization, allocation,
    // alignment requirement on input, or architecture-specific CRC extension.
    return (state >> 8) ^ crc32_table<Polynomial>.values[(state ^ byte) & 255u];
}
template <std::uint32_t Polynomial>
inline std::uint32_t crc32_update(std::uint32_t state, const_bytes data) {
    if (!data.data) return state;
    const auto* p = data.data;
    std::size_t n = data.size;
    // Unaligned head: the wide path reads eight bytes at a time.
    while (n && (reinterpret_cast<std::uintptr_t>(p) & 7u)) {
        state = crc32_byte<Polynomial>(state, *p++);
        --n;
    }
    const auto& t = crc32_tables<Polynomial>.values;
    while (n >= 8) {
        const std::uint32_t lo = state ^
            (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
             (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
        const std::uint32_t hi =
            (static_cast<std::uint32_t>(p[4]) | (static_cast<std::uint32_t>(p[5]) << 8) |
             (static_cast<std::uint32_t>(p[6]) << 16) | (static_cast<std::uint32_t>(p[7]) << 24));
        state = t[7][lo & 255u] ^ t[6][(lo >> 8) & 255u] ^ t[5][(lo >> 16) & 255u] ^
                t[4][(lo >> 24) & 255u] ^ t[3][hi & 255u] ^ t[2][(hi >> 8) & 255u] ^
                t[1][(hi >> 16) & 255u] ^ t[0][(hi >> 24) & 255u];
        p += 8;
        n -= 8;
    }
    while (n--) state = crc32_byte<Polynomial>(state, *p++);
    return state;
}
} // namespace detail

inline std::uint32_t crc32_ieee_update(std::uint32_t state, const_bytes data) {
    return detail::crc32_update<0xEDB88320u>(state, data);
}

inline std::uint32_t crc32_ieee(const_bytes data) {
    return ~crc32_ieee_update(kCrc32IeeeInit, data);
}

// Castagnoli polynomial: distinct from IEEE (disk formats must choose explicitly).
inline std::uint32_t crc32c_update(std::uint32_t state, const_bytes data) {
    return detail::crc32_update<0x82F63B78u>(state, data);
}
inline std::uint32_t crc32c_byte(std::uint32_t state, std::uint8_t byte) {
    return detail::crc32_byte<0x82F63B78u>(state, byte);
}

inline std::uint32_t fnv1a32(const_bytes data, std::uint32_t state = kFnv1a32Basis) {
    if (!data.data) return state;
    for (std::size_t i = 0; i < data.size; ++i) {
        state = (state ^ data.data[i]) * 16777619u;
    }
    return state;
}

} // namespace libk
