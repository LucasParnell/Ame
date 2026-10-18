#pragma once
#include "midix/format/integrity.h"
#include "libk/bytes.h"
#include "monocypher.h"
#include "uECC.h"
namespace mochi::fido2::p256 {
// uECC's deterministic HMAC implementation hashes a SHA256 HMAC pad (64 bytes), its
// previous 32-byte result, a separator and at most two 32-byte inputs: 161
// bytes maximum per invocation. Adapt Ame's one-shot SHA256 without a heap.
struct Hash {
    uECC_HashContext context;
    mx_u8_t scratch[128], input[192];
    mx_u32_t size;
    bool valid;
    Hash() : context{init, update, finish, 64, 32, scratch}, size(0), valid(true) {}
    static void init(const uECC_HashContext* ctx) {
        auto& h = *reinterpret_cast<Hash*>(const_cast<uECC_HashContext*>(ctx));
        h.size = 0;
    }
    static void update(const uECC_HashContext* ctx, const mx_u8_t* data, unsigned size) {
        auto& h = *reinterpret_cast<Hash*>(const_cast<uECC_HashContext*>(ctx));
        if (size > sizeof(h.input) - h.size) { h.valid = false; return; }
        libk::copy_n(libk::as_bytes(h.input + h.size, size), libk::as_bytes(data, size), size);
        h.size += size;
    }
    static void finish(const uECC_HashContext* ctx, mx_u8_t* result) {
        auto& h = *reinterpret_cast<Hash*>(const_cast<uECC_HashContext*>(ctx));
        integrity::sha256(h.input, h.size, result);
    }
};
inline bool public_key(const mx_u8_t private_key[32], mx_u8_t public_key[64]) {
    // The provisioned PRF supplies private keys; Deterministic HMAC supplies signature
    // nonces. No ambient host RNG or unseeded fallback exists on the device.
    uECC_set_rng(nullptr);
    return uECC_compute_public_key(private_key, public_key, uECC_secp256r1()) == 1;
}
inline mx_u32_t der_integer(mx_u8_t* out, const mx_u8_t* input) {
    mx_u32_t at = 0;
    while (at < 31 && !input[at]) ++at;
    const bool pad = input[at] & 128;
    out[0] = 2; out[1] = 32 - at + pad;
    if (pad) out[2] = 0;
    libk::copy_n(libk::as_bytes(out + 2 + pad, 32 - at), libk::as_bytes(input + at, 32 - at), 32 - at);
    return 2 + 32 - at + pad;
}
inline mx_u32_t sign(const mx_u8_t private_key[32], libk::const_bytes message, mx_u8_t der[72]) {
    mx_u8_t digest[32], raw[64];
    integrity::sha256(message.data, message.size, digest);
    Hash hash;
    const bool ok = uECC_sign_deterministic(private_key, digest, sizeof(digest),
                                            &hash.context, raw, uECC_secp256r1()) == 1 && hash.valid;
    crypto_wipe(&hash, sizeof(hash));
    crypto_wipe(digest, sizeof(digest));
    mx_u32_t size = 0;
    if (ok) {
        der[0] = 0x30;
        size = 2 + der_integer(der + 2, raw);
        size += der_integer(der + size, raw + 32);
        der[1] = size - 2;
    }
    crypto_wipe(raw, sizeof(raw));
    return size;
}
} // namespace mochi::fido2::p256
