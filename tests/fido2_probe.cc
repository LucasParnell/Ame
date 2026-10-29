#include "../userland/usb/functions/fido2/runtime.h"
#include "usb_device_model.h"
#include "../userland/usb/gadget/control.h"
#include <cstdio>
#include <cstring>
using namespace mochi::fido2;

static mx_u8_t seed[32] = {1,2,3,4,5,6,7,8}; // host-test fixture only
static void provision(Authenticator& auth) {
    std::memcpy(auth.master, seed, 32);
    auth.provisioned = true;
    auth.prepare_attestation();
}
static mx_u32_t make_request(mx_u8_t* out, const char* rp = "example.com",
                             libk::const_bytes exclude = {}, int algorithm = -8) {
    Writer w{libk::as_bytes(out, kMessageSize)};
    w.byte(1); w.map(exclude.size ? 5 : 4);
    mx_u8_t hash[32]{1}, user[8]{4};
    w.uint(1); w.bytes(libk::as_bytes(hash));
    w.uint(2); w.map(1); w.text("id"); w.text(rp);
    w.uint(3); w.map(1); w.text("id"); w.bytes(libk::as_bytes(user));
    w.uint(4); w.array(1); w.map(2); w.text("alg"); w.item(1, -algorithm - 1);
    w.text("type"); w.text("public-key");
    if (exclude.size) {
        w.uint(5); w.array(1); w.map(2);
        w.text("id"); w.bytes(exclude); w.text("type"); w.text("public-key");
    }
    assert(w.good);
    return w.size;
}
static mx_u32_t assertion_request(mx_u8_t* out, libk::const_bytes id,
                                  const char* rp = "example.com", bool up = true) {
    Writer w{libk::as_bytes(out, kMessageSize)};
    mx_u8_t hash[32]{9};
    w.byte(2); w.map(up ? 3 : 4);
    w.uint(1); w.text(rp); w.uint(2); w.bytes(libk::as_bytes(hash));
    w.uint(3); w.array(1); w.map(2);
    w.text("id"); w.bytes(id); w.text("type"); w.text("public-key");
    if (!up) { w.uint(5); w.map(1); w.text("up"); w.boolean(false); }
    assert(w.good);
    return w.size;
}
static Value response(const Authenticator& auth) {
    assert(auth.response[0] == 0);
    Value result{};
    assert(parse(auth.response + 1, auth.response_size - 1, result));
    assert(result.size == auth.response_size - 1);
    return result;
}
static void print_hex(const char* name, const mx_u8_t* data, size_t size) {
    std::printf("%s=", name);
    for (size_t i = 0; i < size; ++i) std::printf("%02x", data[i]);
    std::puts("");
}
static mx_u32_t attestation_request(mx_u8_t* out, const mx_u8_t hash[32]) {
    Writer w{libk::as_bytes(out, kMessageSize)};
    mx_u8_t user[8]{4};
    w.byte(1); w.map(4);
    w.uint(1); w.bytes(libk::as_bytes(hash, 32));
    w.uint(2); w.map(1); w.text("id"); w.text("example.com");
    w.uint(3); w.map(1); w.text("id"); w.bytes(libk::as_bytes(user));
    w.uint(4); w.array(1); w.map(2); w.text("alg"); w.item(1, 6);
    w.text("type"); w.text("public-key");
    assert(w.good);
    return w.size;
}
static void registration_signing_and_binding() {
    Authenticator auth{};
    provision(auth);
    mx_u8_t request[kMessageSize]{};
    auto size = make_request(request);
    assert(auth.begin(libk::as_bytes(request, size)) && auth.needs_presence);
    // Calling finish without the physical proof must fail closed.
    auth.finish(false); assert(auth.response[0] == 0x28);
    assert(auth.begin(libk::as_bytes(request, size)));
    auth.finish(true);
    auto result = response(auth);
    if (kCertificateAttestation) assert(member(result, 1).text("packed"));
    else assert(member(result, 1).text("none"));
    auto data = member(result, 2).bytes();
    assert(data.size == 162 && data.data[32] == 0x41 && data.data[53] == 0 && data.data[54] == 65);
    mx_u8_t id[65], public_key[32];
    std::memcpy(id, data.data + 55, 65);
    Value key{};
    assert(parse(data.data + 120, data.size - 120, key));
    const auto x = member(key, 0); // negative key -2 accessed explicitly below
    (void)x;
    mx_u32_t at = key.head;
    for (unsigned i = 0; i < key.number; ++i) {
        Value k{}, v{};
        assert(parse(key.data + at, key.size - at, k)); at += k.size;
        assert(parse(key.data + at, key.size - at, v)); at += v.size;
        if (k.major == 1 && k.number == 1) { assert(v.number == 32); std::memcpy(public_key, v.bytes().data, 32); }
    }
    // A new service incarnation needs only the provisioned master and opaque ID.
    Authenticator restarted{}; provision(restarted);
    size = assertion_request(request, libk::as_bytes(id));
    assert(restarted.begin(libk::as_bytes(request, size)));
    restarted.finish(true);
    result = response(restarted);
    data = member(result, 2).bytes();
    const auto signature = member(result, 3).bytes();
    assert(data.size == 37 && data.data[32] == 1 && signature.size == 64);
    mx_u8_t signed_data[69]{};
    std::memcpy(signed_data, data.data, 37); signed_data[37] = 9;
    assert(crypto_ed25519_check(signature.data, public_key, signed_data, sizeof(signed_data)) == 0);
    signed_data[38] ^= 1;
    assert(crypto_ed25519_check(signature.data, public_key, signed_data, sizeof(signed_data)) != 0);
    size = assertion_request(request, libk::as_bytes(id), "evil.example");
    assert(!restarted.begin(libk::as_bytes(request, size)) && restarted.response[0] == 0x2e);
    id[12] ^= 1;
    size = assertion_request(request, libk::as_bytes(id));
    assert(!restarted.begin(libk::as_bytes(request, size)) && restarted.response[0] == 0x2e);
    id[12] ^= 1;
    size = make_request(request, "example.com", libk::as_bytes(id));
    assert(auth.begin(libk::as_bytes(request, size))); // exclusion waits for UP
    auth.finish(true); assert(auth.response[0] == 0x19);
    size = assertion_request(request, libk::as_bytes(id), "example.com", false);
    assert(!restarted.begin(libk::as_bytes(request, size)));
    assert(member(response(restarted), 2).bytes().data[32] == 0);
    size = make_request(request, "example.com", {}, -257);
    assert(!auth.begin(libk::as_bytes(request, size)) && auth.response[0] == 0x26);
    auth.provisioned = false;
    size = make_request(request);
    assert(!auth.begin(libk::as_bytes(request, size)) && auth.response[0] == 0x28);
    const mx_u8_t info[]{4};
    assert(!auth.begin(libk::as_bytes(info)));
    assert(member(response(auth), 1).major == 4);
}

static void receive_message(Hid& hid, mx_u32_t cid, mx_u8_t command,
                             libk::const_bytes bytes, mx_u64_t now) {
    mx_u8_t packet[64]{};
    Hid::write_cid(packet, cid); packet[4] = command;
    packet[5] = bytes.size >> 8; packet[6] = bytes.size;
    auto n = bytes.size < 57 ? bytes.size : 57;
    std::memcpy(packet + 7, bytes.data, n);
    hid.receive(libk::as_bytes(packet), now);
    mx_u32_t at = n, seq = 0;
    while (at < bytes.size) {
        std::memset(packet + 4, 0, 60);
        packet[4] = seq++;
        n = bytes.size - at < 59 ? bytes.size - at : 59;
        std::memcpy(packet + 5, bytes.data + at, n);
        hid.receive(libk::as_bytes(packet), now);
        at += n;
    }
}
static mx_u32_t init(Hid& hid) {
    mx_u8_t nonce[8]{1,2,3,4,5,6,7,8}, report[64];
    receive_message(hid, kBroadcast, 0x86, libk::as_bytes(nonce), 0);
    assert(hid.report(libk::as_bytes(report)));
    assert(report[4] == 0x86 && report[6] == 17 && report[23] == 0x0c);
    assert(!std::memcmp(report + 7, nonce, 8));
    return Hid::read_cid(report + 15);
}
static void framing_cancel_timeout_and_presence() {
    Hid hid{}; provision(hid.auth);
    auto cid = init(hid), other = init(hid);
    assert(cid != other);
    mx_u8_t bytes[kMessageSize]{}, packet[64]{};
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = i;
    receive_message(hid, cid, 0x81, libk::as_bytes(bytes), 0);
    mx_u32_t at = 0;
    while (hid.report(libk::as_bytes(packet))) {
        const auto header = at ? 5u : 7u;
        const auto n = sizeof(bytes) - at < 64 - header ? sizeof(bytes) - at : 64 - header;
        assert(!std::memcmp(packet + header, bytes + at, n)); at += n;
    }
    assert(at == kMessageSize);
    // Invalid sequence discards the partial command; another CID gets BUSY.
    std::memset(packet, 0, 64); Hid::write_cid(packet, cid); packet[4] = 0x81; packet[6] = 100;
    hid.receive(libk::as_bytes(packet), 0); assert(hid.assembling);
    receive_message(hid, other, 0x81, libk::as_bytes(bytes, 1), 0);
    assert(hid.report(libk::as_bytes(packet)) && packet[7] == 6 && hid.assembling);
    Hid::write_cid(packet, cid); packet[4] = 1;
    hid.receive(libk::as_bytes(packet), 0);
    assert(hid.report(libk::as_bytes(packet)) && packet[7] == 4 && !hid.assembling);
    packet[4] = 0x81; packet[5] = 0; packet[6] = 100;
    hid.receive(libk::as_bytes(packet), 0); hid.tick(kAssemblyTimeout, false);
    assert(hid.report(libk::as_bytes(packet)) && packet[7] == 5);
    auto n = make_request(bytes);
    receive_message(hid, cid, 0x90, libk::as_bytes(bytes, n), 0);
    assert(hid.waiting);
    hid.tick(1, false);
    assert(hid.report(libk::as_bytes(packet)) && packet[4] == 0xbb && packet[7] == 2);
    receive_message(hid, cid, 0x91, libk::as_bytes(bytes, 0), 2);
    assert(hid.report(libk::as_bytes(packet)) && packet[4] == 0x90 && packet[7] == 0x2d);
    assert(!hid.waiting);
    receive_message(hid, cid, 0x90, libk::as_bytes(bytes, n), 0);
    hid.tick(kPresenceTimeout, false);
    assert(hid.report(libk::as_bytes(packet)) && packet[7] == 0x2f);
    receive_message(hid, cid, 0x90, libk::as_bytes(bytes, n), 0);
    hid.tick(1, true);
    assert(hid.report(libk::as_bytes(packet)) && packet[7] == 0 && !hid.waiting);
    while(hid.report(libk::as_bytes(packet))) {}
    // A physical press during a queued keepalive must survive USB backpressure.
    receive_message(hid, cid, 0x90, libk::as_bytes(bytes, n), 0);
    hid.tick(1, false);assert(hid.output_pending && hid.waiting);
    hid.tick(2, true);assert(hid.waiting);
    assert(hid.report(libk::as_bytes(packet)) && packet[4]==0xbb);
    hid.tick(3, false);
    assert(hid.report(libk::as_bytes(packet)) && packet[4]==0x90 && packet[7]==0 && !hid.waiting);
    while(hid.report(libk::as_bytes(packet))) {}
    // Cancellation and a new transaction cannot reuse an earlier physical press.
    receive_message(hid,cid,0x90,libk::as_bytes(bytes,n),0);hid.tick(1,false);hid.tick(2,true);
    assert(hid.report(libk::as_bytes(packet)) && packet[4]==0xbb);
    receive_message(hid,cid,0x91,{},3);
    assert(!hid.approval_pending && !hid.waiting);
    while(hid.report(libk::as_bytes(packet))) {}
    receive_message(hid,cid,0x90,libk::as_bytes(bytes,n),4);hid.tick(5,false);
    assert(hid.waiting && !hid.approval_pending);
    hid.reset(); assert(!hid.output_pending && !hid.known(cid));
    Presence p{}; p.reset(0);
    assert(!p.sample(true, 0) && !p.sample(true, 1'000'000'000)); // pre-held key
    assert(!p.sample(false, 1'000'000'001));
    assert(!p.sample(false, 1'031'000'001));
    assert(!p.sample(true, 1'032'000'001));
    assert(!p.sample(false, 1'033'000'001)); // bounce
    assert(!p.sample(true, 1'034'000'001));
    assert(p.sample(true, 1'065'000'001));
    assert(!p.sample(true, 2'000'000'000)); // approval is single use
}

static void malformed_cbor() {
    Value v{};
    const mx_u8_t duplicate[]{0xa2,1,0,1,0}, noncanonical[]{0x18,1}, indefinite[]{0x9f,0xff};
    assert(!parse(duplicate, sizeof(duplicate), v));
    assert(!parse(noncanonical, sizeof(noncanonical), v));
    assert(!parse(indefinite, sizeof(indefinite), v));
    const mx_u8_t bad_utf8[]{0x63,0xed,0xa0,0x80};
    assert(!parse(bad_utf8, sizeof(bad_utf8), v));
    mx_u8_t request[kMessageSize]{};
    const auto size = make_request(request);
    Authenticator a{}; provision(a);
    for (mx_u32_t n = 0; n < size; ++n) assert(!a.begin(libk::as_bytes(request, n)));
    // Every one-byte mutation must stay within the bounded parser/crypto state.
    for (mx_u32_t n = 0; n < size; ++n) {
        request[n] ^= 255;
        (void)a.begin(libk::as_bytes(request, size));
        request[n] ^= 255;
    }
}

namespace gadget = mochi::usb::gadget;
static void lazy_activation_and_report_backpressure() {
    gadget::Runtime r{};
    Storage commands, events, to_fido, from_fido, demand;
    mx_u8_t payload[8192]{};
    r.payload = libk::as_bytes(payload);
    r.links[0] = {&commands.page, &events.page, 3, 1, 1};
    r.links[3] = {&to_fido.page, &from_fido.page, 3, 1, 1};
    r.fido2_demand = {&demand.page, 3, 1};
    r.function_generation = 10;
    r.configuration = 1;
    gadget::reset_functions(r);
    for (unsigned i = 0; i < 12; ++i) gadget::service_functions(r);
    assert(!r.fido2_requested && !r.fido2_demand.depth() && !r.links[3].tx().depth());
    MxDeviceEvent descriptor{};
    descriptor.generation = 4; descriptor.setup = {0x81,6,0x2200,3,255};
    gadget::setup(r, descriptor);
    assert(r.length == 34 && !r.fido2_requested && !r.fido2_demand.depth());
    // First complete host OUT report demands the process and retains its bytes.
    auto& f = r.functions[2];
    f.rx_active = true; f.rx_id = 123;
    gadget::completion(r, {123,4,MxUsbStatus::Success,64,6,0,0,{}});
    assert(r.fido2_requested && r.demand_pending && f.rx_pending);
    gadget::service_functions(r);
    MxRingSlot slot{}; MxFido2Demand request{};
    assert(r.fido2_demand.try_pop(&slot) && mochi::message_get(slot,&request,MX_FIDO2_DEMAND));
    assert(request.active == 1 && request.generation == r.function_generation);
    MxFunctionChunk chunk{};
    assert(r.links[3].tx().try_pop(&slot) && mochi::message_get(slot,&chunk,MX_FUNCTION_RESET));
    assert(chunk.flags == MX_FUNCTION_CONFIGURED);
    assert(r.links[3].tx().try_pop(&slot) && mochi::message_get(slot,&chunk,MX_FUNCTION_RX) && chunk.sequence == 0);
    assert(r.links[3].tx().try_pop(&slot) && mochi::message_get(slot,&chunk,MX_FUNCTION_RX) && chunk.sequence == 1);
    // Half a function reply cannot become a short HID report on the wire.
    chunk = {r.function_generation,0,32,0x86,0,{}};
    assert(r.links[3].rx().try_push_msg(MX_FUNCTION_TX,chunk));
    gadget::service_functions(r);
    assert(f.tx_bytes == 32 && !f.tx_active);
    chunk.sequence = 1;
    assert(r.links[3].rx().try_push_msg(MX_FUNCTION_TX,chunk));
    // Make command space available after OUT receive posts filled it.
    while (r.links[0].tx().try_pop(&slot)) {}
    gadget::service_functions(r);
    assert(f.tx_bytes == 64 && f.tx_active);
    r.configuration = 0;
    gadget::reset_functions(r); gadget::service_functions(r);
    assert(!r.fido2_requested);
    assert(r.fido2_demand.try_pop(&slot) && mochi::message_get(slot,&request,MX_FIDO2_DEMAND));
    assert(request.active == 0);
}

static void unhex(const char* text, mx_u8_t* bytes, mx_u32_t size) {
    for (mx_u32_t i = 0; i < size; ++i) {
        unsigned value = 0;
        assert(std::sscanf(text + i * 2, "%2x", &value) == 1);
        bytes[i] = value;
    }
}
static Value negative_member(Value map, mx_u32_t encoded_key) {
    mx_u32_t at = map.head;
    for (mx_u32_t i = 0; i < map.number; ++i) {
        Value k{}, v{};
        assert(parse(map.data + at, map.size - at, k)); at += k.size;
        assert(parse(map.data + at, map.size - at, v)); at += v.size;
        if (k.major == 1 && k.number == encoded_key) return v;
    }
    return {};
}
static void raw_signature(libk::const_bytes der, mx_u8_t out[64]) {
    assert(der.size >= 8 && der.data[0] == 0x30 && der.data[1] == der.size - 2);
    mx_u32_t at = 2;
    for (mx_u32_t i = 0; i < 2; ++i) {
        assert(der.data[at++] == 2);
        auto count = der.data[at++];
        assert(count && count <= 33 && at + count <= der.size);
        if (count == 33) { assert(der.data[at++] == 0); --count; }
        std::memset(out + i * 32, 0, 32);
        std::memcpy(out + i * 32 + 32 - count, der.data + at, count);
        at += count;
    }
    assert(at == der.size);
}
static void registration_attestation() {
    Authenticator auth{}; provision(auth);
    mx_u8_t request[kMessageSize]{}, challenge[32];
    for (mx_u32_t i = 0; i < 32; ++i) challenge[i] = static_cast<mx_u8_t>(0x40 + i);
    auto size = attestation_request(request, challenge);
    assert(auth.begin(libk::as_bytes(request, size)) && auth.needs_presence);
    auth.finish(true);
    auto result = response(auth);
    auto fmt = member(result, 1);
    assert(fmt.text("packed"));
    std::puts("attestation.fmt=packed");
    auto data = member(result, 2).bytes();
    auto stmt = member(result, 3);
    auto alg = member(stmt, 0, "alg");
    auto sig = member(stmt, 0, "sig").bytes();
    assert(alg && alg.major == 1 && alg.number == 6);
    assert(sig.size >= 70 && sig.size <= 72);
    auto aaguid = libk::slice(data, 37, 16);
    assert(aaguid.size == 16 && integrity::bytes_equal(aaguid.data, kAaguid, 16));
    print_hex("attestation.aaguid", aaguid.data, aaguid.size);
    print_hex("attestation.clienthash", challenge, sizeof(challenge));
    print_hex("attestation.authdata", data.data, data.size);
    print_hex("attestation.sig", sig.data, sig.size);
#if defined(AME_FIDO2_ATTESTATION) && AME_FIDO2_ATTESTATION
    auto x5c = member(stmt, 0, "x5c");
    assert(x5c.major == 4 && x5c.number == 1);
    auto cert = element(x5c, 0).bytes();
    assert(cert.size > 0);
    print_hex("attestation.cert", cert.data, cert.size);
    print_hex("attestation.public", kAttestationPublicKey, 64);
#else
    // Self-attestation is signed by the ES256 credential key; recover X || Y
    // from the COSE key in authData so the host test can verify it.
    Value cose{}; assert(parse(data.data + 120, data.size - 120, cose));
    const auto x = negative_member(cose, 1), y = negative_member(cose, 2);
    assert(x.number == 32 && y.number == 32);
    mx_u8_t public_key[64];
    std::memcpy(public_key, x.bytes().data, 32);
    std::memcpy(public_key + 32, y.bytes().data, 32);
    print_hex("attestation.public", public_key, 64);
#endif
    // GetInfo must report the same model AAGUID as authData. Do this last:
    // begin(4) overwrites response, which data points into.
    const mx_u8_t info_command[]{4};
    assert(!auth.begin(libk::as_bytes(info_command)));
    const auto info = response(auth);
    const auto info_aaguid = member(info, 3).bytes();
    assert(info_aaguid.size == 16 && integrity::bytes_equal(info_aaguid.data, kAaguid, 16));
}

static void es256_vectors_and_credentials() {
    // RFC6979 A.2.5, SHA256("sample"): independent expected public key/signature.
    mx_u8_t secret[32], expected_key[64], expected_signature[64], key[64], signature[72], raw[64];
    unhex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721", secret, 32);
    unhex("60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6"
          "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299", expected_key, 64);
    unhex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"
          "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8", expected_signature, 64);
    assert(p256::public_key(secret, key) && !std::memcmp(key, expected_key, 64));
    auto count = p256::sign(secret, libk::as_bytes("sample", 6), signature);
    raw_signature(libk::as_bytes(signature, count), raw);
    mx_u8_t digest_sample[32];
    integrity::sha256(reinterpret_cast<const mx_u8_t*>("sample"), 6, digest_sample);
    assert(uECC_verify(key, digest_sample, 32, expected_signature, uECC_secp256r1()) == 1);
    assert(uECC_verify(key, digest_sample, 32, raw, uECC_secp256r1()) == 1);
    std::printf("es256.der=");
    for (mx_u32_t i = 0; i < count; ++i) std::printf("%02x", signature[i]);
    std::puts("");
    mx_u8_t repeated[72];
    assert(p256::sign(secret, libk::as_bytes("sample", 6), repeated) == count);
    assert(!std::memcmp(signature, repeated, count));
    Authenticator auth{}; provision(auth);
    mx_u8_t request[kMessageSize]{};
    auto size = make_request(request, "example.com", {}, -7);
    assert(auth.begin(libk::as_bytes(request, size)));
    auth.finish(true);
    auto data = member(response(auth), 2).bytes();
    assert(data.size == 197 && data.data[32] == 0x41);
    mx_u8_t id[65]; std::memcpy(id, data.data + 55, 65);
    assert(id[0] == 2);
    Value cose{}; assert(parse(data.data + 120, data.size - 120, cose));
    assert(member(cose, 1).number == 2 && member(cose, 3).major == 1 && member(cose, 3).number == 6);
    const auto x = negative_member(cose, 1), y = negative_member(cose, 2);
    assert(x.number == 32 && y.number == 32);
    std::memcpy(key, x.bytes().data, 32); std::memcpy(key + 32, y.bytes().data, 32);
    Authenticator restarted{}; provision(restarted);
    size = assertion_request(request, libk::as_bytes(id));
    assert(restarted.begin(libk::as_bytes(request, size)));
    restarted.finish(true);
    auto result = response(restarted);
    data = member(result, 2).bytes();
    auto der = member(result, 3).bytes();
    raw_signature(der, raw);
    mx_u8_t message[69]{}, digest[32];
    std::memcpy(message, data.data, 37); message[37] = 9;
    integrity::sha256(message, sizeof(message), digest);
    assert(uECC_verify(key, digest, 32, raw, uECC_secp256r1()) == 1);
    digest[0] ^= 1;
    assert(uECC_verify(key, digest, 32, raw, uECC_secp256r1()) == 0);
    id[0] = 1; // algorithm substitution is MAC-bound
    size = assertion_request(request, libk::as_bytes(id));
    assert(!restarted.begin(libk::as_bytes(request, size)) && restarted.response[0] == 0x2e);
}
static void arena_startup_and_prompt_transport() {
    alignas(Runtime) mx_u8_t memory[sizeof(Runtime)]{};
    mochi::Arena arena(memory, sizeof(memory));
    auto* runtime = arena.alloc<Runtime>();
    assert(runtime);
    runtime->initialize();
    Storage outgoing, incoming, notices;
    runtime->link = {&outgoing.page, &incoming.page, 3, 1, 1};
    runtime->generation = 42;
    runtime->prompt.ring = {&notices.page, 3, 1};
    assert(!runtime->service() && !runtime->link.tx().depth());
    MxFunctionChunk chunk{42,0,0,0,MX_FUNCTION_CONFIGURED,{}};
    assert(runtime->link.rx().try_push_msg(MX_FUNCTION_RESET,chunk));
    assert(runtime->service() && !runtime->link.tx().depth());
    mx_u8_t packet[64]{};
    Hid::write_cid(packet, kBroadcast); packet[4] = 0x86; packet[6] = 8;
    for (mx_u32_t i = 0; i < 8; ++i) packet[7+i] = i + 1;
    chunk = {42,0,32,6,0,{}};
    std::memcpy(chunk.data, packet, 32);
    assert(runtime->link.rx().try_push_msg(MX_FUNCTION_RX,chunk));
    assert(runtime->service() && !runtime->link.tx().depth());
    chunk.sequence = 1; std::memcpy(chunk.data, packet + 32, 32);
    assert(runtime->link.rx().try_push_msg(MX_FUNCTION_RX,chunk));
    for (unsigned i = 0; i < 8; ++i) runtime->service();
    // The very first transmitted report must be INIT, never an all-zero report.
    assert(runtime->link.tx().depth() == 2);
    MxRingSlot slot{};
    assert(runtime->link.tx().try_pop(&slot) && mochi::message_get(slot,&chunk,MX_FUNCTION_TX));
    assert(chunk.data[4] == 0x86 && chunk.data[6] == 17);
    assert(!std::memcmp(chunk.data + 7, packet + 7, 8));
    PromptPublisher publisher{}; publisher.ring = {&notices.page, 3, 1};
    PromptView view{};
    mx_u8_t site[253]; std::memset(site, 'a', sizeof(site)); site[12] = 27;
    publisher.start(42,10,1,libk::as_bytes(site));
    while (publisher.pending) {
        assert(publisher.flush());
        assert(publisher.ring.try_pop(&slot));
        const bool done = view.accept(slot,0);
        assert(done == !publisher.pending);
    }
    assert(view.state == MX_FIDO2_WAITING && view.rp[12] == '?' && std::strlen(view.rp) == 253);
    publisher.finish(MX_FIDO2_CANCELLED);
    assert(publisher.flush() && publisher.ring.try_pop(&slot) && view.accept(slot,1));
    assert(view.state == MX_FIDO2_CANCELLED && !view.expires);
    publisher.start(42,11,2,libk::as_bytes("example.com",11));
    assert(publisher.flush() && publisher.ring.try_pop(&slot) && view.accept(slot,2));
    assert(!view.expire(3) && view.expire(MX_FIDO2_PRESENCE_NS + 2));
    assert(view.state == MX_FIDO2_EXPIRED);
    publisher.start(42,12,MX_FIDO2_SELECT,libk::as_bytes("Selection",9));
    assert(publisher.flush() && publisher.ring.try_pop(&slot) && view.accept(slot,3));
    publisher.finish(MX_FIDO2_SELECTED);
    assert(publisher.flush() && publisher.ring.try_pop(&slot) && view.accept(slot,4));
    assert(view.state==MX_FIDO2_SELECTED && view.operation==MX_FIDO2_SELECT);
}
#include "fido2_pin_probe.h"
int main() {
    pin_and_resident_credentials();
    registration_signing_and_binding();
    registration_attestation();
    es256_vectors_and_credentials();
    arena_startup_and_prompt_transport();
    framing_cancel_timeout_and_presence();
    malformed_cbor();
    lazy_activation_and_report_backpressure();
    std::puts("fido2 registration, signing and lazy USB activation probe ok");
}
