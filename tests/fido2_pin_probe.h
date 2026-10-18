#include "mochi/fs_private.h"

static void peer_key(Writer& w, const mx_u8_t key[64]) {
    w.map(5);w.uint(1);w.uint(2);w.uint(3);w.item(1,24);
    w.item(1,0);w.uint(1);w.item(1,1);w.bytes(libk::as_bytes(key,32));
    w.item(1,2);w.bytes(libk::as_bytes(key+32,32));
}
static void pin_and_resident_credentials() {
    Authenticator auth{};provision(auth);auth.store.fresh();auth.store.available=true;auth.enable_store();
    mx_u8_t request[kMessageSize]{}, host_secret[32]{},host_public[64],shared[32],point[32];
    host_secret[31]=7;assert(p256::public_key(host_secret,host_public));
    auto key_agreement=[&] {
        Writer w{libk::as_bytes(request)};w.byte(6);w.map(2);w.uint(1);w.uint(1);w.uint(2);w.uint(2);
        assert(!auth.begin(libk::as_bytes(request,w.size)));assert(auth.response[0]==0);
        assert(uECC_shared_secret(auth.pin.public_key,host_secret,point,uECC_secp256r1()));
        integrity::sha256(point,32,shared);
    };
    // Safari sends empty pinAuth to select a key before asking for its first PIN.
    Writer select{libk::as_bytes(request)};select.byte(1);select.map(1);select.uint(8);select.bytes({});
    assert(auth.begin(libk::as_bytes(request,select.size)) && auth.needs_presence);
    auth.finish(true);assert(auth.response[0]==0x35 && !auth.store.data.pin_set);
    key_agreement();
    // Repeated discovery (including a second browser client) must not replace
    // the agreement key while the first client is entering its PIN.
    const auto epoch=auth.store.data.epoch;
    mx_u8_t first_agreement[64];std::memcpy(first_agreement,auth.pin.public_key,64);
    key_agreement();
    assert(auth.store.data.epoch==epoch && !std::memcmp(first_agreement,auth.pin.public_key,64));
    mx_u8_t encrypted[64]{'1','2','3','4'},mac[32];
    pin_aes(shared,encrypted,64,false);hmac256(mac,shared,libk::as_bytes(encrypted));
    Writer set{libk::as_bytes(request)};set.byte(6);set.map(5);
    set.uint(1);set.uint(1);set.uint(2);set.uint(3);set.uint(3);peer_key(set,host_public);
    set.uint(4);set.bytes(libk::as_bytes(mac,16));set.uint(5);set.bytes(libk::as_bytes(encrypted));
    assert(!auth.begin(libk::as_bytes(request,set.size)));assert(auth.response[0]==0 && auth.pin.configured());
    // Token uses the same agreement as setPIN, matching Safari's setup flow.
    auto token_request=[&](bool correct) {
        mx_u8_t digest[32],enc[16];integrity::sha256(reinterpret_cast<const mx_u8_t*>(correct?"1234":"9999"),4,digest);
        std::memcpy(enc,digest,16);pin_aes(shared,enc,16,false);
        Writer w{libk::as_bytes(request)};w.byte(6);w.map(4);
        w.uint(1);w.uint(1);w.uint(2);w.uint(5);w.uint(3);peer_key(w,host_public);w.uint(6);w.bytes(libk::as_bytes(enc));
        assert(!auth.begin(libk::as_bytes(request,w.size)));
    };
    token_request(true);auto result=response(auth);mx_u8_t token[32];
    assert(member(result,2).number==32);std::memcpy(token,member(result,2).bytes().data,32);pin_aes(shared,token,32,true);
    mx_u8_t hash[32]{1};hmac256(mac,token,libk::as_bytes(hash));
    assert(auth.pin.authorize(libk::as_bytes(mac,16),libk::as_bytes(hash)));
    mac[0]^=1;assert(!auth.pin.authorize(libk::as_bytes(mac,16),libk::as_bytes(hash)));mac[0]^=1;
    for (mx_u8_t user=1;user<=2;++user) {
        Writer w{libk::as_bytes(request)};w.byte(1);w.map(7);
        w.uint(1);w.bytes(libk::as_bytes(hash));w.uint(2);w.map(1);w.text("id");w.text("example.com");
        w.uint(3);w.map(1);w.text("id");w.bytes(libk::as_bytes(&user,1));
        w.uint(4);w.array(1);w.map(2);w.text("alg");w.item(1,6);w.text("type");w.text("public-key");
        w.uint(7);w.map(1);w.text("rk");w.boolean(true);w.uint(8);w.bytes(libk::as_bytes(mac,16));w.uint(9);w.uint(1);
        assert(auth.begin(libk::as_bytes(request,w.size)));auth.finish(true);
        assert(member(response(auth),2).bytes().data[32]==0x45);
    }
    // Persisted state survives a new service, while volatile PIN tokens do not.
    SealedStore sealed{};seal_store(sealed,auth.store.data,seed);
    Authenticator restarted{};provision(restarted);assert(open_store(restarted.store.data,sealed,seed));
    restarted.store.available=true;restarted.enable_store();assert(restarted.pin.configured());assert(!restarted.pin.token_issued);
    sealed.ciphertext[100]^=1;Stored broken{};assert(!open_store(broken,sealed,seed));
    sealed.ciphertext[100]^=1;sealed.nonce[0]^=1;assert(!open_store(broken,sealed,seed));
    Writer get{libk::as_bytes(request)};get.byte(2);get.map(2);get.uint(1);get.text("example.com");get.uint(2);get.bytes(libk::as_bytes(hash));
    assert(restarted.begin(libk::as_bytes(request,get.size),1));restarted.finish(true);
    result=response(restarted);assert(member(result,5).number==2);
    assert(member(member(result,4),0,"id").bytes().data[0]==1);
    const mx_u8_t next[]{8};assert(!restarted.begin(libk::as_bytes(next),2));
    result=response(restarted);assert(member(member(result,4),0,"id").bytes().data[0]==2);
    assert(!restarted.begin(libk::as_bytes(next),3) && restarted.response[0]==0x30);
    for (unsigned attempt=0;attempt<3;++attempt) {
        key_agreement();token_request(false);
        assert(auth.response[0]==(attempt==2?0x34:0x31));assert(auth.store.data.retries==7-attempt);
    }
    // Replug clears the temporary block, preserving the durable retry budget.
    auth.pin.failures=0;auth.store.data.retries=1;key_agreement();token_request(false);
    assert(auth.response[0]==0x32 && !auth.store.data.retries);
    key_agreement();token_request(true);assert(auth.response[0]==0x32);
    auth.store.persist=[](void*,const Stored&) {return false;};
    auth.pin.agreement_ready=false;
    Writer fail{libk::as_bytes(request)};fail.byte(6);fail.map(2);fail.uint(1);fail.uint(1);fail.uint(2);fail.uint(2);
    assert(!auth.begin(libk::as_bytes(request,fail.size)) && auth.response[0]==0x7f && !auth.store.available);
    FsOpenRequest open{};open.name_len=6;std::memcpy(open.name,".fido2",6);MxRingSlot slot{};
    mochi::message_put(&slot,FS_OPEN,open);assert(mochi::fs::private_state_request(slot));
    open.name[1]='x';mochi::message_put(&slot,FS_OPEN,open);assert(!mochi::fs::private_state_request(slot));
}
