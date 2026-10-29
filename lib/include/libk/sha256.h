#pragma once
// Streaming SHA-256 over bounded views. Compression backends operate on whole
// blocks and preserve the same state layout; callers own CPU/ISA eligibility.
#include "libk/bytes.h"

namespace libk {
namespace sha256_detail {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
inline constexpr u32 kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

inline u32 rotr(u32 v, u32 n) {
    return (v >> n) | (v << (32 - n));
}

inline u32 load_be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

inline void store_be32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

inline void compress(u32 h[8], const u8 block[64]) {
    u32 w[64];
    for (u32 i = 0; i < 16; ++i) {
        w[i] = load_be32(block + i * 4);
    }
    for (u32 i = 16; i < 64; ++i) {
        const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = h[0];
    u32 b = h[1];
    u32 c = h[2];
    u32 d = h[3];
    u32 e = h[4];
    u32 f = h[5];
    u32 g = h[6];
    u32 hh = h[7];

    for (u32 i = 0; i < 64; ++i) {
        const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const u32 ch = (e & f) ^ ((~e) & g);
        const u32 t1 = hh + s1 + ch + kSha256K[i] + w[i];
        const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

} // namespace sha256_detail

using Sha256Compress = void (*)(std::uint32_t state[8], const std::uint8_t* data,
                                std::size_t blocks);

inline void sha256_compress(std::uint32_t state[8], const std::uint8_t* data, std::size_t blocks) {
    for (std::size_t i = 0; i < blocks; ++i)
        sha256_detail::compress(state, data + i * 64);
}

struct Sha256 {
    std::uint32_t words[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::uint64_t byte_count{};
    std::size_t used{};
    std::uint8_t tail[64]{};

    bool update(const_bytes data, Sha256Compress compress = sha256_compress) {
        if (!compress || (data.size && !data.data) || used >= sizeof(tail) ||
            byte_count > (~std::uint64_t{0} >> 3) ||
            data.size > (~std::uint64_t{0} >> 3) - byte_count)
            return false;

        byte_count += data.size;
        std::size_t offset = 0;
        if (used) {
            const auto room = sizeof(tail) - used;
            const auto count = data.size < room ? data.size : room;
            copy_n(as_bytes(tail + used, room), data, count);
            used += count;
            offset += count;
            if (used == sizeof(tail)) {
                compress(words, tail, 1);
                used = 0;
            }
        }

        const auto blocks = (data.size - offset) / 64;
        if (blocks) {
            compress(words, data.data + offset, blocks);
            offset += blocks * 64;
        }
        const auto remaining = data.size - offset;
        if (remaining) {
            copy_n(as_bytes(tail + used, sizeof(tail) - used), slice(data, offset, remaining),
                   remaining);
            used += remaining;
        }
        return true;
    }

    bool finish(bytes output, Sha256Compress compress = sha256_compress) const {
        if (!compress || output.size != 32 || !output.data || used >= sizeof(tail) ||
            byte_count > (~std::uint64_t{0} >> 3))
            return false;

        std::uint32_t state[8];
        copy_exact(as_bytes(state), as_bytes(words));
        alignas(8) std::uint8_t padding[128]{};
        copy_n(as_bytes(padding), as_bytes(tail), used);
        padding[used] = 0x80;

        const auto blocks = used < 56 ? 1u : 2u;
        const auto bit_count = byte_count * 8;
        for (unsigned i = 0; i < 8; ++i)
            padding[blocks * 64 - 1 - i] = static_cast<std::uint8_t>(bit_count >> (i * 8));
        compress(state, padding, blocks);

        for (unsigned i = 0; i < 8; ++i)
            sha256_detail::store_be32(output.data + i * 4, state[i]);
        return true;
    }
};
} // namespace libk
