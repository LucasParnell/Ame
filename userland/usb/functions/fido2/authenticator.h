#pragma once
#include "cbor.h"
#include "midix/format/integrity.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "p256.h"
#include "pin.h"
#include "attestation.h"
namespace mochi::fido2 {
constexpr mx_u32_t kMessageSize = 2048, kCredentialSize = 65;
// ES256 and Ed25519 credentials. A keyed PRF replaces random key
// generation: the 256-bit master is provisioned from the host OS CSPRNG once.
// IDs authenticate their RP binding; domain-separated seeds never leave here.
// Counter zero means unsupported, as permitted by WebAuthn. Resident credentials
// and PIN retry state are sealed and synced before acknowledging changes.
struct Authenticator {
    mx_u8_t master[32]{};
    bool provisioned = false;
    bool attestation_ready = false;
    Store store;
    Pin pin;
    bool resident_requested = false, verified = false, selection_only = false;
    mx_u8_t selection_status = 0, assertion_candidates[kResidentCapacity]{}, assertion_count = 0, assertion_next = 0;
    mx_i32_t selected_resident = -1;
    mx_u8_t assertion_hash[32]{};
    mx_u64_t assertion_expires = 0;
    bool assertion_presence = false, assertion_verified = false;
    void enable_store() { pin.initialize(store,master); }
    static void text_copy(char* destination,mx_u32_t capacity,Value value) {
        if (value && value.major==3) {
            const auto n=value.number<capacity-1?value.number:capacity-1;
            libk::copy_n(libk::as_bytes(destination,n),value.bytes(),n);destination[n]=0;
        }
    }
    mx_u8_t response[kMessageSize]{};
    mx_u32_t response_size = 0;
    Value request{};
    mx_u8_t command = 0, rp_hash[32]{};
    libk::const_bytes client_hash{}, user_id{}, rp_id{};
    mx_u8_t credential[kCredentialSize]{};
    bool excluded = false, needs_presence = false;
    mx_u8_t credential_version = 1;

    void derive(mx_u8_t out[32], mx_u8_t domain, libk::const_bytes a,
                libk::const_bytes b = {}, libk::const_bytes c = {}) const {
        crypto_blake2b_ctx ctx;
        crypto_blake2b_keyed_init(&ctx, 32, master, sizeof(master));
        crypto_blake2b_update(&ctx, &domain, 1);
        const libk::const_bytes parts[] = {a, b, c};
        for (const auto part : parts) {
            mx_u8_t length[8]{};
            for (mx_u32_t i = 0; i < 8; ++i) length[i] = part.size >> (8 * i);
            crypto_blake2b_update(&ctx, length, sizeof(length));
            if (part.size) crypto_blake2b_update(&ctx, part.data, part.size);
        }
        crypto_blake2b_final(&ctx, out);
        crypto_wipe(&ctx, sizeof(ctx));
    }
    void make_id() {
        credential[0] = credential_version;
        derive(credential + 1, 1, libk::as_bytes(rp_hash), client_hash, user_id);
        derive(credential + 33, 2, libk::as_bytes(rp_hash), libk::as_bytes(credential, 33));
    }
    bool valid_id(libk::const_bytes id) const {
        if (id.size != kCredentialSize || (id.data[0] != 1 && id.data[0] != 2)) return false;
        mx_u8_t mac[32];
        derive(mac, 2, libk::as_bytes(rp_hash), libk::slice(id, 0, 33));
        const bool valid = crypto_verify32(mac, id.data + 33) == 0;
        crypto_wipe(mac, sizeof(mac));
        return valid;
    }

    // The attestation key is derived once from the master seed. The generated
    // certificate is public; the derived private key never leaves this object.
    // A mismatched certificate (for example a userland bundle built for a
    // different device seed) disables certificate attestation and falls back
    // to self-attestation rather than emitting an unverifiable statement.
    bool prepare_attestation() {
        attestation_ready = false;
#if defined(AME_FIDO2_ATTESTATION) && AME_FIDO2_ATTESTATION
        if (!provisioned) return false;
        mx_u8_t secret[32]{}, public_key[64]{};
        derive(secret, 5, {});
        if (p256::public_key(secret, public_key) &&
            integrity::bytes_equal(public_key, kAttestationPublicKey, 64)) {
            attestation_ready = true;
        }
        crypto_wipe(secret, sizeof(secret));
#endif
        return attestation_ready;
    }
    mx_u8_t status(mx_u8_t code) {
        response[0] = code;
        response_size = 1;
        needs_presence = false;
        return code;
    }
    void info() {
        Writer w{libk::as_bytes(response)};
        w.byte(0); w.map(store.available ? 8 : 7);
        w.uint(1); w.array(1); w.text("FIDO_2_0");
        w.uint(3); w.bytes(libk::as_bytes(kAaguid));
        w.uint(4); w.map(store.available ? 4 : 3);
        w.text("rk"); w.boolean(store.available);
        w.text("up"); w.boolean(true);
        w.text("plat"); w.boolean(false);
        if (store.available) {w.text("clientPin");w.boolean(pin.configured());}
        w.uint(5); w.uint(kMessageSize);
        if (store.available) {w.uint(6);w.array(1);w.uint(1);}
        w.uint(7); w.uint(16); // maxCredentialCountInList
        w.uint(8); w.uint(kCredentialSize);
        w.uint(10); w.array(2); w.map(2);
        w.text("alg"); w.item(1, 6); // COSE ES256 -7
        w.text("type"); w.text("public-key"); w.map(2);
        w.text("alg"); w.item(1, 7); // COSE EdDSA -8
        w.text("type"); w.text("public-key");
        response_size = w.good ? w.size : 1;
        if (!w.good) status(0x7f);
    }
    // Returns true only for a validated operation awaiting a fresh physical
    // press. Input remains owned by the HID transport until complete/cancel.
    bool begin(libk::const_bytes input, mx_u64_t now = 0) {
        status(0x01);
        excluded = false; resident_requested = verified = selection_only = false;
        request = {};
        if (!input.size) { status(0x03); return false; }
        command = input.data[0];
        if (command == 4) {
            if (input.size != 1) status(0x03); else info();
            return false;
        }
        if (command == 8) {
            if (input.size!=1 || !assertion_count || assertion_next>=assertion_count || now>assertion_expires) {status(0x30);return false;}
            selected_resident=assertion_candidates[assertion_next++];
            const auto& r=store.data.resident[selected_resident];
            libk::copy_n(libk::as_bytes(credential),libk::as_bytes(r.id),65);
            verified=assertion_verified;command=2;finish(assertion_presence);return false;
        }
        assertion_count=assertion_next=0;
        if (command != 1 && command != 2 && command != 6) return false;
        if (!parse(input.data + 1, input.size - 1, request) ||
            request.size != input.size - 1 || request.major != 5) { status(0x12); return false; }
        if (!provisioned) { status(0x28); return false; }
        if (pin.store && !store.available) {status(0x7f);return false;}
        if (command==6) {
            Writer w{libk::as_bytes(response)};w.byte(0);
            const auto code=pin.command(request,w);
            if (code || !w.good) status(code?code:0x7f);else response_size=w.size;
            return false;
        }
        const auto pin_auth=member(request,command==1?8:6),protocol=member(request,command==1?9:7);
        if (pin_auth && pin_auth.major==2 && !pin_auth.number && store.available) {
            static constexpr char selection[]="Security key selection";
            rp_id=libk::as_bytes(selection,sizeof(selection)-1);
            selection_only=true;selection_status=pin.configured()?0x31:0x35;
            needs_presence=true;return true;
        }
        auto rp = member(request, command == 1 ? 2 : 1);
        const auto hash = member(request, command == 1 ? 1 : 2);
        if (!rp || !hash) { status(0x14); return false; }
        if (command == 1) {
            if (rp.major != 5) { status(0x11); return false; }
            rp = member(rp, 0, "id");
            if (!rp) { status(0x14); return false; }
        }
        if (rp.major != 3 || hash.major != 2) { status(0x11); return false; }
        if (!rp.number || rp.number > 253 || hash.number != 32) { status(0x02); return false; }
        rp_id = rp.bytes();
        integrity::sha256(rp_id.data, rp_id.size, rp_hash);
        client_hash = hash.bytes();
        if (pin_auth) {
            if (!pin.configured()) {status(0x35);return false;}
            if (!protocol || protocol.major!=0 || protocol.number!=1) {status(0x02);return false;}
            if (pin_auth.major!=2 || !pin.authorize(pin_auth.bytes(),client_hash)) {status(0x33);return false;}
            verified=true;
        } else if (command==1 && pin.configured()) {status(0x36);return false;}
        libk::copy_n(libk::as_bytes(assertion_hash),client_hash,32);
        const auto options = member(request, command == 1 ? 7 : 5);
        bool up = true;
        if (options) {
            if (options.major != 5) { status(0x11); return false; }
            const char* option_names[] = {"rk", "uv", "up"};
            for (const char* option : option_names) {
                const auto value = member(options, 0, option);
                if (!value) continue;
                if (value.major != 7) { status(0x11); return false; }
                if (option[0] == 'u' && option[1] == 'p') {
                    if (command == 1) { status(0x2c); return false; }
                    up = value.number == 21;
                } else if (option[0]=='r') {
                    if (command==2) {status(0x2b);return false;}
                    resident_requested=value.number==21;
                    if (resident_requested && !store.available) {status(0x2b);return false;}
                } else if (value.number==21 && !verified) {status(0x2c);return false;}
            }
        }
        if (command == 1) {
            const auto user = member(request, 3), algorithms = member(request, 4);
            if (!user || !algorithms) { status(0x14); return false; }
            if (user.major != 5 || algorithms.major != 4) { status(0x11); return false; }
            const auto id = member(user, 0, "id");
            if (!id) { status(0x14); return false; }
            if (id.major != 2) { status(0x11); return false; }
            if (!id.number || id.number > 64) { status(0x02); return false; }
            user_id = id.bytes();
            bool supported = false;
            for (mx_u32_t i = 0; i < algorithms.number; ++i) {
                const auto algorithm = element(algorithms, i);
                const auto type = member(algorithm, 0, "type"), alg = member(algorithm, 0, "alg");
                if (algorithm.major != 5 || !type || !alg || type.major != 3 || alg.major > 1) {
                    status(0x11); return false;
                }
                if (!supported && type.text("public-key") && alg.major == 1 &&
                    (alg.number == 6 || alg.number == 7)) {
                    // Follow the relying party's preference order.
                    credential_version = alg.number == 6 ? 2 : 1;
                    supported = true;
                }
            }
            if (!supported) { status(0x26); return false; }
        }
        const auto list = member(request, command == 1 ? 5 : 3);
        bool found = false;
        if (list) {
            if (list.major != 4) { status(0x11); return false; }
            if (list.number > 16) { status(0x15); return false; }
            for (mx_u32_t i = 0; i < list.number; ++i) {
                const auto descriptor = element(list, i);
                const auto type = member(descriptor, 0, "type"), id = member(descriptor, 0, "id");
                if (descriptor.major != 5 || !type || !id || type.major != 3 || id.major != 2) {
                    status(0x11); return false;
                }
                if (!found && type.text("public-key") && valid_id(id.bytes())) {
                    libk::copy_exact(libk::as_bytes(credential), id.bytes());
                    found = true;
                }
            }
        }
        selected_resident=-1;
        if (command==2 && !list && store.available) {
            if (!up) {status(0x27);return false;}
            for (mx_u32_t i=0;i<kResidentCapacity;++i) {
                const auto& r=store.data.resident[i];
                if (r.used && integrity::bytes_equal(r.rp_hash,rp_hash,32)) assertion_candidates[assertion_count++]=i;
            }
            if (assertion_count) {
                selected_resident=assertion_candidates[0];assertion_next=1;
                libk::copy_n(libk::as_bytes(credential),libk::as_bytes(store.data.resident[selected_resident].id),65);
                found=true;assertion_expires=now+30'000'000'000ull;
                assertion_presence=up;assertion_verified=verified;
            }
        }
        if (command == 2 && !found) { status(0x2e); return false; }
        excluded = command == 1 && found;
        needs_presence = up;
        if (!up) finish(false);
        return up;
    }
    void finish(bool presence) {
        if (needs_presence && !presence) { status(0x28); return; }
        needs_presence = false;
        if (selection_only) {status(selection_status);return;}
        if (excluded) { status(0x19); return; }
        if (command == 1) {
            make_id();
            if (resident_requested) {
                mx_i32_t target=-1;
                for(mx_u32_t i=0;i<kResidentCapacity;++i) {
                    const auto& r=store.data.resident[i];
                    if (!r.used && target<0) target=i;
                    if (r.used && integrity::bytes_equal(r.rp_hash,rp_hash,32) && r.user_size==user_id.size &&
                        integrity::bytes_equal(r.user,user_id.data,user_id.size)) {target=i;break;}
                }
                if (target<0) {status(0x28);return;}
                auto& r=store.data.resident[target];libk::zero(r);
                r.used=1;r.user_size=user_id.size;
                libk::copy_n(libk::as_bytes(r.rp_hash),libk::as_bytes(rp_hash),32);
                libk::copy_n(libk::as_bytes(r.id),libk::as_bytes(credential),65);
                libk::copy_n(libk::as_bytes(r.user),user_id,user_id.size);
                libk::copy_n(libk::as_bytes(r.rp),rp_id,rp_id.size);
                const auto user=member(request,3);
                text_copy(r.name,sizeof(r.name),member(user,0,"name"));
                text_copy(r.display,sizeof(r.display),member(user,0,"displayName"));
                if (!store.save()) {status(0x7f);return;}
            }
        }
        const bool es256 = credential[0] == 2;
        mx_u8_t seed[32], secret[64]{}, public_key[64]{};
        derive(seed, 3, libk::as_bytes(rp_hash), libk::as_bytes(credential, 33));
        if (es256) {
            bool valid = false;
            for (mx_u8_t attempt = 0; attempt < 16; ++attempt) {
                if (attempt) derive(seed, 4, libk::as_bytes(rp_hash),
                                    libk::as_bytes(credential, 33), libk::as_bytes(&attempt, 1));
                if (p256::public_key(seed, public_key)) { valid = true; break; }
            }
            if (!valid) { crypto_wipe(seed, sizeof(seed)); status(0x7f); return; }
            libk::copy_n(libk::as_bytes(secret), libk::as_bytes(seed), 32);
            crypto_wipe(seed, sizeof(seed));
        } else crypto_ed25519_key_pair(secret, public_key, seed); // wipes seed
        mx_u8_t auth[256]{};
        Writer a{libk::as_bytes(auth)};
        a.raw(libk::as_bytes(rp_hash));
        a.byte((command == 1 ? 0x40 : 0) | (presence ? 1 : 0) | (verified ? 4 : 0));
        for (mx_u32_t i = 0; i < 4; ++i) a.byte(0); // unsupported counter
        if (command == 1) {
            a.raw(libk::as_bytes(kAaguid));
            a.byte(0); a.byte(kCredentialSize); a.raw(libk::as_bytes(credential));
            a.map(es256 ? 5 : 4);
            a.uint(1); a.uint(es256 ? 2 : 1); // kty EC2 / OKP
            a.uint(3); a.item(1, es256 ? 6 : 7); // ES256 / EdDSA
            a.item(1, 0); a.uint(es256 ? 1 : 6); // P-256 / Ed25519
            a.item(1, 1); a.bytes(libk::as_bytes(public_key, 32));
            if (es256) { a.item(1, 2); a.bytes(libk::as_bytes(public_key + 32, 32)); }
        }
        // Attestation statements are built before the response map so a
        // signing failure can return a clean CTAP error. authData is at most
        // 197 bytes for ES256, leaving room for clientDataHash in auth[256].
        const bool certificate = command == 1 && attestation_ready;
        const bool self_attest = command == 1 && !certificate && es256;
        mx_u8_t attestation_signature[72]{};
        mx_u32_t attestation_signature_size = 0;
        if (certificate || self_attest) {
            const auto message_size = a.size + 32;
            if (!a.good || message_size > sizeof(auth) || client_hash.size != 32) {
                crypto_wipe(seed, sizeof(seed));
                crypto_wipe(secret, sizeof(secret));
                status(0x7f);
                return;
            }
            libk::copy_n(libk::as_bytes(auth + a.size, 32), client_hash, 32);
            if (certificate) {
                derive(seed, 5, {});
                attestation_signature_size = p256::sign(
                    seed, libk::as_bytes(auth, message_size), attestation_signature);
                crypto_wipe(seed, sizeof(seed));
            } else {
                attestation_signature_size = p256::sign(
                    secret, libk::as_bytes(auth, message_size), attestation_signature);
            }
            if (!attestation_signature_size) {
                crypto_wipe(seed, sizeof(seed));
                crypto_wipe(secret, sizeof(secret));
                status(0x7f);
                return;
            }
        }
        Writer w{libk::as_bytes(response)};
        w.byte(0); w.map(command==2 && selected_resident>=0 ? (assertion_count>1 && assertion_next==1?5:4) : 3);
        if (command == 1) {
            if (certificate || self_attest) {
                w.uint(1); w.text("packed");
                w.uint(2); w.bytes(libk::as_bytes(auth, a.size));
                w.uint(3); w.map(certificate ? 3 : 2);
                w.text("alg"); w.item(1, 6); // ES256
                w.text("sig"); w.bytes(libk::as_bytes(attestation_signature, attestation_signature_size));
                if (certificate) {
#if defined(AME_FIDO2_ATTESTATION) && AME_FIDO2_ATTESTATION
                    w.text("x5c"); w.array(1);
                    w.bytes(libk::as_bytes(kAttestationCertificate, kAttestationCertificateSize));
#endif
                }
            } else {
                w.uint(1); w.text("none");
                w.uint(2); w.bytes(libk::as_bytes(auth, a.size));
                w.uint(3); w.map(0);
            }
        } else {
            mx_u8_t signed_data[69], signature[72];
            libk::copy_n(libk::as_bytes(signed_data), libk::as_bytes(auth, 37), 37);
            libk::copy_n(libk::as_bytes(signed_data + 37, 32), libk::as_bytes(assertion_hash), 32);
            mx_u32_t signature_size = 64;
            if (es256) signature_size = p256::sign(secret, libk::as_bytes(signed_data), signature);
            else crypto_ed25519_sign(signature, secret, signed_data, sizeof(signed_data));
            w.uint(1); w.map(2);
            w.text("id"); w.bytes(libk::as_bytes(credential));
            w.text("type"); w.text("public-key");
            w.uint(2); w.bytes(libk::as_bytes(auth, 37));
            w.uint(3); w.bytes(libk::as_bytes(signature, signature_size));
            if (!signature_size) w.good = false;
            if (selected_resident>=0) {
                const auto& r=store.data.resident[selected_resident];
                const bool names=verified && (r.name[0] || r.display[0]);
                w.uint(4);w.map(1+(names && r.name[0]?1:0)+(names && r.display[0]?1:0));
                w.text("id");w.bytes(libk::as_bytes(r.user,r.user_size));
                if (names && r.name[0]) {w.text("name");w.text(r.name);}
                if (names && r.display[0]) {w.text("displayName");w.text(r.display);}
                if (assertion_count>1 && assertion_next==1) {w.uint(5);w.uint(assertion_count);}
            }
            crypto_wipe(signed_data, sizeof(signed_data));
        }
        crypto_wipe(secret, sizeof(secret));
        response_size = w.size;
        if (!w.good || !a.good) status(0x7f);
    }
};
} // namespace mochi::fido2
