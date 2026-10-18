#pragma once
#include "crypto.h"
namespace mochi::fido2 {
constexpr mx_u32_t kResidentCapacity = 16;
struct Resident {
    mx_u8_t used, user_size, reserved[2];
    mx_u8_t rp_hash[32], id[65], user[64];
    char rp[254], name[64], display[64];
};
struct Stored {
    mx_u32_t version, pin_set, retries, reserved;
    mx_u64_t epoch;
    mx_u8_t pin_hash[16];
    Resident resident[kResidentCapacity];
};
struct Store {
    Stored data{};
    bool available = false;
    bool (*persist)(void*,const Stored&) = nullptr;
    void* context = nullptr;
    bool save() {
        if (!available) return false;
        if (!persist || persist(context,data)) return true;
        available = false;
        return false;
    }
    void fresh() {
        libk::zero(data); data.version=1; data.retries=8;
    }
};
struct SealedStore {
    mx_u32_t version, size;
    mx_u8_t nonce[24], mac[16];
    mx_u8_t ciphertext[sizeof(Stored)];
};
inline void seal_store(SealedStore& out,const Stored& state,const mx_u8_t master[32]) {
    out.version=1;out.size=sizeof(state);
    mx_u8_t key[32],nonce[32];
    prf(key,master,8);prf(nonce,master,9,libk::as_bytes(&state,sizeof(state)));
    libk::copy_n(libk::as_bytes(out.nonce),libk::as_bytes(nonce),24);
    crypto_aead_lock(out.ciphertext,out.mac,key,out.nonce,
                     reinterpret_cast<const mx_u8_t*>(&out),8,
                     reinterpret_cast<const mx_u8_t*>(&state),sizeof(state));
    crypto_wipe(key,sizeof(key));crypto_wipe(nonce,sizeof(nonce));
}
inline bool open_store(Stored& out,const SealedStore& sealed,const mx_u8_t master[32]) {
    if (sealed.version!=1 || sealed.size!=sizeof(out)) return false;
    mx_u8_t key[32];prf(key,master,8);
    const bool valid=crypto_aead_unlock(reinterpret_cast<mx_u8_t*>(&out),sealed.mac,key,sealed.nonce,
        reinterpret_cast<const mx_u8_t*>(&sealed),8,sealed.ciphertext,sizeof(out))==0;
    crypto_wipe(key,sizeof(key));
    if (!valid || out.version!=1 || out.pin_set>1 || out.retries>8) {crypto_wipe(&out,sizeof(out));return false;}
    for (const auto& r:out.resident) {
        if (r.used>1 || r.user_size>64 || (r.used &&
            (r.rp[253] || r.name[63] || r.display[63] || (r.id[0]!=1 && r.id[0]!=2)))) return false;
    }
    return true;
}
} // namespace mochi::fido2
