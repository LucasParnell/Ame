#pragma once
#include "store.h"
#include "cbor.h"
namespace mochi::fido2 {
struct Pin {
    Store* store = nullptr;
    const mx_u8_t* master = nullptr;
    mx_u8_t private_key[32]{}, public_key[64]{}, token[32]{};
    mx_u8_t failures = 0;
    bool agreement_ready = false, token_issued = false;
    bool configured() const {return store && store->available && store->data.pin_set;}
    bool initialize(Store& s,const mx_u8_t key[32]) {store=&s;master=key;return s.available;}
    bool agreement() {
        if (!store || !store->available) return false;
        // Discovery returns the current key. Replacing it here invalidates a
        // platform that is still asking for the PIN using the previous key.
        if (agreement_ready) return true;
        if (store->data.epoch==~mx_u64_t{0}) return false;
        ++store->data.epoch;
        if (!store->save()) return false;
        for (mx_u8_t attempt=0;attempt<16;++attempt) {
            prf(private_key,master,10,libk::as_bytes(&store->data.epoch,8),libk::as_bytes(&attempt,1));
            if (p256::public_key(private_key,public_key)) {agreement_ready=true;return true;}
        }
        return false;
    }
    bool shared(Value peer,mx_u8_t out[32]) {
        if (!agreement_ready || peer.major!=5) return false;
        const auto kty=member(peer,1),alg=member(peer,3);
        if (!kty || kty.major!=0 || kty.number!=2 || !alg || alg.major!=1 || alg.number!=24) return false;
        mx_u8_t key[64]{};mx_u32_t at=peer.head;
        bool x=false,y=false,curve=false;
        for (mx_u32_t i=0;i<peer.number;++i) {
            Value k{},v{};
            if (!parse(peer.data+at,peer.size-at,k)) return false;at+=k.size;
            if (!parse(peer.data+at,peer.size-at,v)) return false;at+=v.size;
            if (k.major!=1) continue;
            if (k.number==0) curve=v.major==0 && v.number==1;
            if ((k.number==1 || k.number==2) && v.major==2 && v.number==32) {
                libk::copy_n(libk::as_bytes(key+(k.number==1?0:32),32),v.bytes(),32);
                if (k.number==1) x=true;else y=true;
            }
        }
        if (!x || !y || !curve || !uECC_valid_public_key(key,uECC_secp256r1())) return false;
        mx_u8_t secret[32];
        const bool ok=uECC_shared_secret(key,private_key,secret,uECC_secp256r1())==1;
        if (ok) integrity::sha256(secret,32,out);
        crypto_wipe(secret,sizeof(secret));return ok;
    }
    bool authorize(libk::const_bytes parameter,libk::const_bytes hash) const {
        if (!configured() || !token_issued || parameter.size!=16) return false;
        mx_u8_t mac[32];hmac256(mac,token,hash);
        const bool ok=crypto_verify16(mac,parameter.data)==0;
        crypto_wipe(mac,sizeof(mac));return ok;
    }
    mx_u8_t check_pin(const mx_u8_t key[32],Value encrypted) {
        if (!configured()) return 0x35;
        if (!store->data.retries) return 0x32;
        if (failures>=3) return 0x34;
        if (!encrypted || encrypted.major!=2 || encrypted.number!=16) return 0x02;
        // Persist every attempt before checking it; unplugging cannot replenish retries.
        --store->data.retries;
        if (!store->save()) return 0x7f;
        mx_u8_t hash[16];libk::copy_n(libk::as_bytes(hash),encrypted.bytes(),16);
        pin_aes(key,hash,16,true);
        const bool valid=crypto_verify16(hash,store->data.pin_hash)==0;
        crypto_wipe(hash,sizeof(hash));
        if (!valid) {
            ++failures;agreement_ready=false;token_issued=false;
            crypto_wipe(private_key,sizeof(private_key));
            return !store->data.retries?0x32:failures>=3?0x34:0x31;
        }
        store->data.retries=8;failures=0;
        return store->save()?0:0x7f;
    }
    static bool new_pin(const mx_u8_t key[32],Value encrypted,mx_u8_t hash[16]) {
        if (encrypted.major!=2 || encrypted.number!=64) return false;
        mx_u8_t plaintext[64],digest[32];
        libk::copy_n(libk::as_bytes(plaintext),encrypted.bytes(),64);pin_aes(key,plaintext,64,true);
        mx_u32_t n=0;while (n<64 && plaintext[n]) ++n;
        bool valid=n>=4 && n<=63;
        for (mx_u32_t i=n;i<64;++i) valid &= !plaintext[i];
        // Reuse the bounded CBOR UTF-8 validator and count Unicode code points.
        mx_u8_t text[66]{0x78,static_cast<mx_u8_t>(n)};
        libk::copy_n(libk::as_bytes(text+2,n),libk::as_bytes(plaintext,n),n);
        Value checked{};
        if (n<24) {
            text[0]=0x60|n;libk::move_n(libk::as_bytes(text+1,65),libk::as_bytes(text+2,n),n);
        }
        valid &= parse(text,n+(n<24?1:2),checked);
        mx_u32_t characters=0;for(mx_u32_t i=0;i<n;++i) characters+=(plaintext[i]&0xc0)!=0x80;
        valid &= characters>=4;
        if (valid) {integrity::sha256(plaintext,n,digest);libk::copy_n(libk::as_bytes(hash,16),libk::as_bytes(digest),16);}
        crypto_wipe(plaintext,sizeof(plaintext));crypto_wipe(digest,sizeof(digest));crypto_wipe(text,sizeof(text));
        return valid;
    }
    mx_u8_t command(Value request,Writer& w) {
        if (!store || !store->available) return 0x7f;
        const auto protocol=member(request,1),sub=member(request,2);
        if (!protocol || !sub) return 0x14;
        if (protocol.major!=0 || protocol.number!=1 || sub.major!=0) return 0x02;
        if (sub.number==1) {w.map(1);w.uint(3);w.uint(store->data.retries);return 0;}
        if (sub.number==2) {
            if (!agreement()) return 0x7f;
            w.map(1);w.uint(1);w.map(5);
            w.uint(1);w.uint(2);w.uint(3);w.item(1,24);
            w.item(1,0);w.uint(1);w.item(1,1);w.bytes(libk::as_bytes(public_key,32));
            w.item(1,2);w.bytes(libk::as_bytes(public_key+32,32));return 0;
        }
        if (sub.number!=3 && sub.number!=4 && sub.number!=5) return 0x02;
        const auto peer=member(request,3),auth=member(request,4),new_enc=member(request,5),old_enc=member(request,6);
        if (!peer) return 0x14;
        if (sub.number==3 && configured()) return 0x30;
        mx_u8_t key[32];if (!shared(peer,key)) return 0x02;
        mx_u8_t status=0;
        if (sub.number==3 || sub.number==4) {
            if (!auth || !new_enc || (sub.number==4 && !old_enc)) status=0x14;
            else if (auth.major!=2 || auth.number!=16 || new_enc.major!=2 || new_enc.number!=64 ||
                     (sub.number==4 && (old_enc.major!=2 || old_enc.number!=16))) status=0x02;
            else {
                mx_u8_t mac[32];hmac256(mac,key,new_enc.bytes(),sub.number==4?old_enc.bytes():libk::const_bytes{});
                if (crypto_verify16(mac,auth.bytes().data)) status=0x33;
                crypto_wipe(mac,sizeof(mac));
                if (!status && sub.number==4) status=check_pin(key,old_enc);
                mx_u8_t hash[16];
                if (!status && !new_pin(key,new_enc,hash)) status=0x37;
                if (!status) {
                    libk::copy_n(libk::as_bytes(store->data.pin_hash),libk::as_bytes(hash),16);
                    store->data.pin_set=1;store->data.retries=8;token_issued=false;
                    if (!store->save()) status=0x7f;
                }
                crypto_wipe(hash,sizeof(hash));
            }
        } else {
            if (member(request,9) || member(request,10)) status=0x02;
            else status=check_pin(key,old_enc);
            if (!status) {
                if (store->data.epoch==~mx_u64_t{0}) status=0x7f;
                else {
                    ++store->data.epoch;
                    if (!store->save()) status=0x7f;
                    else {
                        prf(token,master,11,libk::as_bytes(&store->data.epoch,8),libk::as_bytes(private_key));
                        token_issued=true;
                        mx_u8_t encrypted[32];libk::copy_n(libk::as_bytes(encrypted),libk::as_bytes(token),32);
                        pin_aes(key,encrypted,32,false);w.map(1);w.uint(2);w.bytes(libk::as_bytes(encrypted));
                        crypto_wipe(encrypted,sizeof(encrypted));
                    }
                }
            }
        }
        crypto_wipe(key,sizeof(key));return status;
    }
};
} // namespace mochi::fido2
