#include "libk/arm64_crypto.h"
#include "libk/hash.h"

extern "C" int crypto_state_probe(std::uint32_t*, const std::uint8_t*, std::size_t,
                                  const std::uint32_t*, void*);

static alignas(64) std::uint8_t data[8256];
static alignas(16) std::uint8_t vectors[512];

extern "C" int crypto_probe() {
    using namespace libk;
    static_assert(arm64::CryptoFeatures{0x11120}.sha256());
    static_assert(arm64::CryptoFeatures{0x11120}.crc32());
    static_assert(!arm64::CryptoFeatures{0}.sha256());
    static_assert(!arm64::CryptoFeatures{0xf000}.sha256());

    for (std::size_t i = 0; i < sizeof(data); ++i)
        data[i] = static_cast<std::uint8_t>(i * 137 + i / 3);

    constexpr std::size_t lengths[] = {0,  1,   7,   8,   9,   55,   56,   63,  64,
                                       65, 127, 128, 129, 255, 4095, 4096, 8199};
    for (unsigned offset = 0; offset < 8; ++offset) {
        for (auto length : lengths) {
            const auto input = as_bytes(data + offset, length);
            if (arm64::crc32c_update(~0u, input) != libk::crc32c_update(~0u, input))
                return 1;

            Sha256 portable, hardware, streaming;
            std::uint8_t a[32], b[32], c[32];
            if (!portable.update(input) || !portable.finish(as_bytes(a)))
                return 2;
            if (!hardware.update(input, arm64::sha256_blocks) ||
                !hardware.finish(as_bytes(b), arm64::sha256_blocks))
                return 3;

            for (std::size_t at = 0; at < length;) {
                const auto count = length - at < 13 ? length - at : 13;
                if (!streaming.update(slice(input, at, count), arm64::sha256_blocks))
                    return 4;
                at += count;
            }
            if (!streaming.finish(as_bytes(c), arm64::sha256_blocks))
                return 5;
            if (!equal(as_bytes(a), as_bytes(b)) || !equal(as_bytes(a), as_bytes(c)))
                return 6;
        }
    }

    // Independent NIST SHA-256("abc") vector, including final padding.
    constexpr std::uint8_t expected[] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    Sha256 known;
    std::uint8_t digest[32];
    if (!known.update(as_bytes("abc", 3), arm64::sha256_blocks) ||
        !known.finish(as_bytes(digest), arm64::sha256_blocks) ||
        !equal(as_bytes(digest), as_bytes(expected)))
        return 7;

    Sha256 state, reference;
    sha256_compress(reference.words, data, 1);
    if (crypto_state_probe(state.words, data, 1, sha256_detail::kSha256K, vectors))
        return 8;
    if (!equal(as_bytes(state.words), as_bytes(reference.words)))
        return 9;
    for (auto byte : vectors)
        if (byte != 0x5a)
            return 10;
    return 0;
}
