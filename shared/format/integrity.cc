#include "midix/format/integrity.h"

namespace integrity {

namespace {

constexpr u32 kSha256K[64] = {
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

u32 rotr(u32 v, u32 n) {
    return (v >> n) | (v << (32 - n));
}

u32 load_be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

void store_be32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

void compress(u32 h[8], const u8 block[64]) {
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

} // namespace

u32 crc32c(const u8* data, usize size, Progress progress) {
    static constexpr u32 table[16] = {
        0x00000000u, 0x105ec76fu, 0x20bd8edeu, 0x30e349b1u, 0x417b1dbcu, 0x5125dad3u,
        0x61c69362u, 0x7198540du, 0x82f63b78u, 0x92a8fc17u, 0xa24bb5a6u, 0xb21572c9u,
        0xc38d26c4u, 0xd3d3e1abu, 0xe330a81au, 0xf36e6f75u,
    };
    u32 crc = 0xffffffffu;
    for (usize i = 0; i < size; ++i) {
        if (!(i & 4095)) progress.poll();
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 0xfu];
        crc = (crc >> 4) ^ table[crc & 0xfu];
    }
    return ~crc;
}

u32 crc32c_zero_field(const u8* data, usize size, usize zero_offset, usize zero_size,
                     Progress progress) {
    static constexpr u32 table[16] = {
        0x00000000u, 0x105ec76fu, 0x20bd8edeu, 0x30e349b1u, 0x417b1dbcu, 0x5125dad3u,
        0x61c69362u, 0x7198540du, 0x82f63b78u, 0x92a8fc17u, 0xa24bb5a6u, 0xb21572c9u,
        0xc38d26c4u, 0xd3d3e1abu, 0xe330a81au, 0xf36e6f75u,
    };
    u32 crc = 0xffffffffu;
    for (usize i = 0; i < size; ++i) {
        if (!(i & 4095)) progress.poll();
        const bool zero = i >= zero_offset && i < zero_offset + zero_size;
        crc ^= zero ? 0 : data[i];
        crc = (crc >> 4) ^ table[crc & 0xfu];
        crc = (crc >> 4) ^ table[crc & 0xfu];
    }
    return ~crc;
}

void sha256(const u8* data, usize size, u8 out[kSha256Size], Progress progress) {
    u32 h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    usize offset = 0;
    while (offset + 64 <= size) {
        if (!(offset & 4095)) progress.poll();
        compress(h, data + offset);
        offset += 64;
    }

    progress.poll();
    u8 tail[128]{};
    const usize remaining = size - offset;
    for (usize i = 0; i < remaining; ++i) {
        tail[i] = data[offset + i];
    }
    tail[remaining] = 0x80u;
    const u64 bit_len = static_cast<u64>(size) * 8ull;

    usize final_blocks = 1;
    if (remaining + 1 + 8 > 64) {
        final_blocks = 2;
    }
    const usize len_pos = final_blocks * 64 - 8;
    for (u32 i = 0; i < 8; ++i) {
        tail[len_pos + i] = static_cast<u8>(bit_len >> (56 - i * 8));
    }

    compress(h, tail);
    if (final_blocks == 2) {
        compress(h, tail + 64);
    }

    for (u32 i = 0; i < 8; ++i) {
        store_be32(out + i * 4, h[i]);
    }
}

bool bytes_equal(const u8* a, const u8* b, usize size) {
    u8 diff = 0;
    for (usize i = 0; i < size; ++i) {
        diff |= static_cast<u8>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace integrity
