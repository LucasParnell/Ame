#include <array>
#include <cassert>
#include <string>
#include "rpmb_diagnostics.h"

using namespace anne;
static std::array<u8, 0x8000> window{};
static std::string scenario;
static unsigned registrations, smcs, programs, polls, queries;
static unsigned ready_after = 3;
static bool programmed;
static MxDiagEl3Rpmb run(u32 action);
static void make_key() {
    window[511] = scenario == "wrong-frame" ? 2 : 1;
    for (unsigned i = 196; i < 228; ++i) window[i] = u8(i ^ 0xa5);
    window[rpmb::kKeyStatusOffset] = 1;
}
static bool prepare(rpmb::ConfigInfo& out) {
    out = rpmb::kAnneConfig;
    assert(run(MX_RPMB_STATUS).registering);
    return scenario != "prepare-failed";
}
static rpmb::KeyInfo key_info() {
    ++queries;
    assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
    if (scenario == "query-error") return {rpmb::kErrorMmc};
    if (scenario == "wrong-counter-type") return {0, 0, 7, 0x0400};
    if (scenario == "already") return {0, 22, 0, 0x0200};
    if (programmed && scenario != "verify-failed") return {0, 0, 0, 0x0200};
    return {0, 0, 7, 0x0200};
}
static u64 program(volatile u8* request) {
    ++programs;
    assert(request == window.data() && window[511] == 1 && window[196] == (196 ^ 0xa5));
    rpmb_diag::ResearchAccess research;
    assert(!research && run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
    if (scenario == "transport-error") return rpmb::kErrorMmc;
    for (unsigned i = 0; i < 512; ++i) window[i] = 0;
    window[510] = 1;
    if (scenario == "card-error") window[509] = 1;
    else programmed = true;
    return 0;
}
static void delay(u32 ms) {
    assert(ms == 5);
    ++polls;
    assert(run(MX_RPMB_STATUS).registering);
    assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
    if (scenario != "timeout" && polls == ready_after) make_key();
}
static bool pending() { return false; }
static u64 service(volatile u8*) { assert(false); return 0; }
static const rpmb_diag::Storage storage{prepare, service, program, pending, key_info, delay};
static u64 firmware(u64 fid, u64 x1, u64 x2, u64 x3) {
    assert(!x2 && !x3);
    if (fid == rpmb::kRequestAddressSmc) {
        ++registrations;
        assert(x1 == reinterpret_cast<u64>(window.data()));
        return scenario == "registration-failed" ? ~u64{0} : 0;
    }
    assert(fid == rpmb::kSetKeySmc && x1 == 0); // never FF06 on this path
    ++smcs;
    for (unsigned i = 0; i < 512; ++i) assert(window[i] == 0);
    assert(window[rpmb::kKeyStatusOffset] == 0);
    if (scenario == "smc-failed") return ~u64{0};
    if (scenario == "immediate") make_key();
    return 0;
}
static MxDiagEl3Rpmb run(u32 action) {
    MxDiagEl3Rpmb out{};
    rpmb_diag::execute(action, out, window.data(), firmware, &storage);
    return out;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    scenario = argv[1];
    window[511] = 1; window[196] = 0x7c; // stale key frame must never be sent
    window[rpmb::kKeyStatusOffset] = 1;
    if (scenario == "stale-request") window[rpmb::kRequestStatusOffset] = 1;
    if (scenario == "key-status") {
        const auto original = window;
        const auto out = run(MX_RPMB_KEY_STATUS);
        assert(out.result == MX_RPMB_OK && out.key_state == 1 && !out.armed);
        assert(!registrations && !smcs && !programs && queries == 1 && window == original);
        return 0;
    }
    const auto out = run(MX_RPMB_PROVISION_KEY);
    if (scenario == "prepare-failed" || scenario == "stale-request") {
        assert(!registrations && !smcs && !programs && !out.armed); return 0;
    }
    if (scenario == "registration-failed") {
        assert(out.result == MX_RPMB_REGISTRATION_FAILED && !queries && !smcs); return 0;
    }
    for (unsigned i = 196; i < 228; ++i) assert(window[i] == 0);
    if (scenario == "already") {
        assert(out.result == MX_RPMB_ALREADY_PROVISIONED && !smcs && !programs && out.key_state == 2);
    } else if (scenario == "query-error" || scenario == "wrong-counter-type") {
        assert(out.result == MX_RPMB_PROVISION_FAILED && !smcs && !programs);
    } else if (scenario == "timeout") {
        assert(out.result == MX_RPMB_PROVISION_UNCERTAIN && out.provision_pending && out.armed);
        assert(smcs == 1 && polls == 300 && programs == 0);
        assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
        assert(run(MX_RPMB_PROVISION_KEY).result == MX_RPMB_BUSY);
        rpmb_diag::ResearchAccess research; assert(!research); return 0;
    } else if (scenario == "transport-error" || scenario == "verify-failed") {
        assert(out.result == MX_RPMB_PROVISION_UNCERTAIN && programs == 1 && smcs == 1);
    } else if (scenario == "smc-failed" || scenario == "wrong-frame" || scenario == "card-error") {
        assert(out.result == MX_RPMB_PROVISION_FAILED && smcs == 1);
        if (scenario != "card-error") assert(!programs);
    } else {
        assert(out.result == MX_RPMB_OK && out.provision_stage == MX_RPMB_PROVISION_DONE);
        assert(out.key_state == 2 && smcs == 1 && programs == 1 && queries == 2);
        assert(polls == (scenario == "immediate" ? 0 : ready_after));
    }
    assert(!out.armed && !out.registering && !out.provision_pending);
    rpmb_diag::ResearchAccess research; assert(research);
}
