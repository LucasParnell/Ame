#pragma once
#include "p256.h"
#include "aes.h"
namespace mochi::fido2 {
inline void prf(mx_u8_t out[32], const mx_u8_t master[32], mx_u8_t domain,
                libk::const_bytes a = {}, libk::const_bytes b = {}, libk::const_bytes c = {}) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, master, 32);
    crypto_blake2b_update(&ctx, &domain, 1);
    const libk::const_bytes parts[] = {a,b,c};
    for (const auto part : parts) {
        mx_u8_t length[8]{};
        for (mx_u32_t i = 0; i < 8; ++i) length[i] = part.size >> (8*i);
        crypto_blake2b_update(&ctx,length,8);
        if (part.size) crypto_blake2b_update(&ctx,part.data,part.size);
    }
    crypto_blake2b_final(&ctx,out); crypto_wipe(&ctx,sizeof(ctx));
}
inline void hmac256(mx_u8_t out[32], const mx_u8_t key[32], libk::const_bytes a,
                    libk::const_bytes b = {}) {
    // All PIN protocol messages fit in 96 bytes, plus a 64-byte HMAC pad.
    mx_u8_t buffer[160]{}, inner[32];
    if (a.size + b.size > 96) { libk::zero(libk::as_bytes(out,32)); return; }
    for (mx_u32_t i=0;i<64;++i) buffer[i]=(i<32?key[i]:0)^0x36;
    libk::copy_n(libk::as_bytes(buffer+64,a.size),a,a.size);
    libk::copy_n(libk::as_bytes(buffer+64+a.size,b.size),b,b.size);
    integrity::sha256(buffer,64+a.size+b.size,inner);
    for (mx_u32_t i=0;i<64;++i) buffer[i]=(i<32?key[i]:0)^0x5c;
    libk::copy_n(libk::as_bytes(buffer+64,32),libk::as_bytes(inner),32);
    integrity::sha256(buffer,96,out);
    crypto_wipe(buffer,sizeof(buffer)); crypto_wipe(inner,sizeof(inner));
}
inline void pin_aes(const mx_u8_t key[32], mx_u8_t* bytes, mx_u32_t size, bool decrypt) {
    AES_ctx ctx; const mx_u8_t iv[16]{};
    AES_init_ctx_iv(&ctx,key,iv);
    if (decrypt) AES_CBC_decrypt_buffer(&ctx,bytes,size);
    else AES_CBC_encrypt_buffer(&ctx,bytes,size);
    crypto_wipe(&ctx,sizeof(ctx));
}
} // namespace mochi::fido2
