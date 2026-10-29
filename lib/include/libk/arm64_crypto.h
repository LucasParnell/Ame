#pragma once
#include "libk/sha256.h"

namespace libk::arm64 {
struct CryptoFeatures {
    std::uint64_t isar0;
    constexpr bool sha256() const {
        const auto field = (isar0 >> 12) & 15u;
        return field == 1 || field == 2;
    }
    constexpr bool crc32() const {
        return ((isar0 >> 16) & 15u) == 1;
    }
};

// The caller establishes ISA availability and SIMD ownership. These routines
// follow AAPCS64; SHA uses caller-saved vector registers, CRC uses only GPRs.
extern "C" void libk_arm64_sha256_blocks(std::uint32_t state[8], const std::uint8_t* data,
                                         std::size_t blocks, const std::uint32_t rounds[64]);
extern "C" std::uint32_t libk_arm64_crc32c(std::uint32_t state, const std::uint8_t* data,
                                           std::size_t size);

inline void sha256_blocks(std::uint32_t state[8], const std::uint8_t* data, std::size_t blocks) {
    libk_arm64_sha256_blocks(state, data, blocks, sha256_detail::kSha256K);
}

inline std::uint32_t crc32c_update(std::uint32_t state, const_bytes data) {
    return data.data ? libk_arm64_crc32c(state, data.data, data.size) : state;
}
} // namespace libk::arm64
