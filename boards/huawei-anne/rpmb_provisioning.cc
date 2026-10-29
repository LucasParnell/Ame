#include "rpmb_provisioning.h"

namespace anne::rpmb_diag {
namespace {
void barrier() {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    asm volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
u16 be16(const volatile u8* p) { return (u16(p[0]) << 8) | p[1]; }
u16 le16(const volatile u8* p) { return p[0] | (u16(p[1]) << 8); }
struct Wipe {
    volatile u8* request;
    ~Wipe() {
        // PROGRAM_KEY's secret occupies mac[32]. Volatile stores survive DSE.
        for (unsigned i = 196; i < 228; ++i) request[i] = 0;
        barrier();
    }
};
}
ProvisionResult provision(volatile u8* request, const Storage& storage, Smc call) {
    ProvisionResult out;
    if (!request || !call || !storage.key_info || !storage.provision_key || !storage.delay_ms) {
        out.transport = rpmb::kErrorSetKey; return out;
    }
    Wipe wipe{request};
    out.key = storage.key_info();
    if (out.key.present()) { out.result = MX_RPMB_ALREADY_PROVISIONED; return out; }
    if (!out.key.missing()) { out.transport = out.key.error ? out.key.error : rpmb::kErrorCounter; return out; }

    // mmc_rpmb_key_store explicitly clears BOTH before FF05, so stale readiness
    // or a frame left by a previous attempt cannot authorize PROGRAM_KEY.
    for (unsigned i = 0; i < 512; ++i) request[i] = 0;
    request[rpmb::kKeyStatusOffset] = 0;
    request[rpmb::kKeyStatusOffset + 1] = 0;
    barrier();
    out.stage = MX_RPMB_PROVISION_GET_FRAME;
    out.smc_result = call(rpmb::kSetKeySmc, 0, 0, 0);
    if (out.smc_result) { out.transport = out.smc_result; return out; }
    out.stage = MX_RPMB_PROVISION_WAIT_FRAME;
    // WAIT_KEY_FRAME_TIMES=300, mdelay(5), from the pinned Huawei driver.
    bool ready = false;
    for (unsigned i = 0; i < 300; ++i) {
        barrier();
        if (le16(request + rpmb::kKeyStatusOffset) == 1) { ready = true; break; }
        storage.delay_ms(5);
    }
    if (!ready) {
        out.result = MX_RPMB_PROVISION_UNCERTAIN;
        out.transport = rpmb::kErrorTimeout;
        // FF05 may finish late. Keep research access excluded until reset.
        out.pending = true; return out;
    }
    if (be16(request + 510) != 1) { out.transport = rpmb::kErrorSetKey; return out; }
    out.stage = MX_RPMB_PROVISION_PROGRAM;
    out.key = {}; // the programming attempt can change the card's key state
    out.transport = storage.provision_key(request);
    barrier();
    out.response_result = (u64(be16(request + 510)) << 16) | be16(request + 508);
    if (out.transport) {
        // A timed-out programming command may have reached the card. Never
        // replay it; a later explicit attempt must query key state first.
        out.result = MX_RPMB_PROVISION_UNCERTAIN;
        return out;
    }
    if (out.response_result != 0x01000000) return out;
    out.stage = MX_RPMB_PROVISION_VERIFY;
    out.key = storage.key_info();
    if (!out.key.present()) {
        out.transport = out.key.error ? out.key.error : rpmb::kErrorCounter;
        out.result = MX_RPMB_PROVISION_UNCERTAIN;
        return out;
    }
    out.stage = MX_RPMB_PROVISION_DONE;
    out.result = MX_RPMB_OK;
    return out;
}
}
