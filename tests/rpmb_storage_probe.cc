#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include "rpmb_diagnostics.h"

using namespace anne;
static std::array<u8, 0x8000> window{};
static bool available = true, pending, notify_during_done;
static u64 storage_result, done_result;
static unsigned registrations, services, completions;
static MxDiagEl3Rpmb run(u32 action);
static bool prepare(rpmb::ConfigInfo& config) {
    rpmb_diag::ResearchAccess research;
    assert(!research); // exclusive against all research paths
    assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
    config = rpmb::emmc_config({16, 0, 0, {0, 0, 0, 0}});
    return available;
}
static u64 service(volatile u8* request) {
    ++services;
    assert(request == window.data());
    rpmb_diag::ResearchAccess research;
    assert(!research);
    assert(run(MX_RPMB_STATUS).done_inflight); // no state lock held over PIO
    assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_ALREADY_CALLED);
    assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
    if (!storage_result) { request[510] = 2; request[511] = 0; }
    return storage_result;
}
static bool write_pending() { return pending; }
static rpmb_diag::Storage storage{prepare, service, nullptr, write_pending};
static u64 firmware(u64 fid, u64 x1, u64 x2, u64 x3) {
    assert(x2 == 0 && x3 == 0);
    if (fid == rpmb::kRequestAddressSmc) {
        ++registrations;
        assert(x1 == reinterpret_cast<u64>(window.data()));
        const auto config = rpmb::emmc_config({16, 0, 0, {0, 0, 0, 0}});
        assert(std::memcmp(window.data(), &config, sizeof(config)) == 0);
        return 0;
    }
    assert(fid == rpmb::kRequestDoneSmc);
    ++completions;
    assert(x1 == storage_result && services == completions);
    if (!storage_result) assert(window[510] == 2 && window[511] == 0);
    if (notify_during_done) {
        notify_during_done = false;
        window[510] = 0; window[511] = 2;
        rpmb_diag::notify_request(4);
    }
    return done_result;
}
static MxDiagEl3Rpmb run(u32 action) {
    MxDiagEl3Rpmb out{};
    rpmb_diag::execute(action, out, window.data(), firmware, &storage);
    return out;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario(argv[1]);
    if (scenario == "prepare-failed") available = false;
    auto out = run(MX_RPMB_ARM_EMMC);
    if (!available) {
        assert(out.result == MX_RPMB_STORAGE_UNAVAILABLE && !out.armed && !registrations);
        for (const auto byte : window) assert(byte == 0);
        rpmb_diag::ResearchAccess research;
        assert(research); return 0;
    }
    assert(out.result == MX_RPMB_OK && out.storage_enabled && out.registered && registrations == 1);
    assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_NO_REQUEST && !services);
    rpmb_diag::notify_request(1);
    if (scenario == "duplicate") {
        rpmb_diag::notify_request(2);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_AMBIGUOUS_REQUEST);
        assert(!services && !completions); return 0;
    }
    if (scenario == "service-error") storage_result = 0xff04;
    if (scenario == "completion-failed") done_result = ~u64{0};
    if (scenario == "write-lease") pending = true;
    if (scenario == "notify-during-ff06") notify_during_done = true;
    out = run(MX_RPMB_COMPLETE);
    assert(out.completion_error == storage_result && out.done_result == done_result && services == 1);
    if (done_result) {
        assert(out.result == MX_RPMB_COMPLETION_FAILED);
        assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_ALREADY_CALLED); return 0;
    }
    assert(out.completed_events == 1 && out.done_returned && !out.done_inflight);
    if (scenario == "write-lease") {
        assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
        pending = false;
    }
    if (scenario == "two-requests" || scenario == "notify-during-ff06") {
        if (scenario == "two-requests") {
            assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_NO_REQUEST);
            rpmb_diag::notify_request(2);
        } else assert(out.events == 2);
        out = run(MX_RPMB_COMPLETE);
        assert(out.result == MX_RPMB_OK && out.completed_events == 2 && services == 2);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_NO_REQUEST);
    }
    assert(run(MX_RPMB_RELEASE).result == MX_RPMB_OK);
    rpmb_diag::ResearchAccess research;
    assert(research);
}
