#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include "rpmb.h"
#include "rpmb_diagnostics.h"

using namespace anne;
static std::array<u8, 0x8000> window{}, registered_window{};
static unsigned registrations, triggers, completions;
static u64 registration_result, completion_result;
static bool concurrent;
static std::atomic<bool> notified{}, completed{};

static u64 firmware(u64 fid, u64 x1, u64 x2, u64 x3) {
    assert(x2 == 0 && x3 == 0);
    // Firmware must always run without the state lock, but with exclusive
    // ownership against all legacy raw/research operations.
    rpmb_diag::ResearchAccess research;
    assert(!research);
    if (fid == rpmb::kRequestAddressSmc) {
        ++registrations;
        assert(x1 == reinterpret_cast<u64>(window.data()));
        assert(std::memcmp(window.data(), &rpmb::kAnneConfig, sizeof(rpmb::kAnneConfig)) == 0);
        registered_window = window;
        return registration_result;
    }
    if (fid == rpmb::kCounterSmc) {
        ++triggers;
        assert(x1 == 0);
        if (concurrent) {
            rpmb_diag::notify_request(0);
            notified.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!completed.load(std::memory_order_acquire)) {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
            return 0;
        }
        return ~u64{0}; // Retail debug service unavailable.
    }
    assert(fid == rpmb::kRequestDoneSmc);
    ++completions;
    assert(x1 == rpmb::kErrorBlockDevice && x1 != 0);
    // No forged responses, status markers or HMAC breadcrumbs were written.
    assert(window == registered_window);
    completed.store(true, std::memory_order_release);
    return completion_result;
}

static MxDiagEl3Rpmb run(u32 action) {
    MxDiagEl3Rpmb out;
    std::memset(&out, 0xa5, sizeof(out)); // Caller result fields are never trusted.
    rpmb_diag::execute(action, out, window.data(), firmware);
    assert(out.version == MX_DIAG_VERSION && out.action == action);
    return out;
}
static void arm() {
    auto info = run(MX_RPMB_ARM);
    assert(info.result == MX_RPMB_OK && info.armed && info.registered);
    assert(registrations == 1 && !triggers && !completions);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    // Plausible/stale frame bytes alone must never authorize completion.
    window[510] = 0; window[511] = 2;
    window[rpmb::kRequestInfoOffset + 20] = 3;
    window[rpmb::kRequestInfoOffset] = 1;
    window[0x6900] = 0x7f;
    if (scenario == "busy-research") {
        rpmb_diag::ResearchAccess research;
        assert(research);
        assert(run(MX_RPMB_ARM).result == MX_RPMB_BUSY);
        assert(!registrations);
        return 0;
    }
    if (scenario == "stale") {
        window[rpmb::kRequestStatusOffset] = 1;
        assert(run(MX_RPMB_ARM).result == MX_RPMB_STALE_REQUEST);
        assert(!registrations);
        rpmb_diag::ResearchAccess research;
        assert(research);
        return 0;
    }
    if (scenario == "registration-failed") {
        registration_result = 0xff01;
        assert(run(MX_RPMB_ARM).result == MX_RPMB_REGISTRATION_FAILED);
        assert(run(MX_RPMB_COUNTER).result == MX_RPMB_BUSY);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_BUSY);
        assert(!triggers && !completions);
        assert(run(MX_RPMB_RELEASE).result == MX_RPMB_OK);
        return 0;
    }
    if (scenario == "old-event") rpmb_diag::notify_request(7);
    arm();
    if (scenario == "no-event" || scenario == "old-event") {
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_NO_REQUEST);
        assert(!completions);
    } else if (scenario == "disabled-trigger") {
        auto info = run(MX_RPMB_COUNTER);
        assert(info.trigger_returned && !info.trigger_inflight && info.trigger_result == ~u64{0});
        assert(run(MX_RPMB_COUNTER).result == MX_RPMB_ALREADY_CALLED);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_NO_REQUEST);
        assert(triggers == 1 && !completions);
    } else if (scenario == "concurrent") {
        concurrent = true;
        std::thread trigger([] { assert(run(MX_RPMB_COUNTER).result == MX_RPMB_OK); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!notified.load(std::memory_order_acquire)) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        auto info = run(MX_RPMB_STATUS);
        assert(info.events == 1 && info.trigger_inflight && !info.done_started);
        assert(info.frame_type == 2 && info.request_state == 3 && info.func_id == 1);
        assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
        info = run(MX_RPMB_COMPLETE);
        assert(info.result == MX_RPMB_OK && info.done_returned && !info.done_inflight);
        assert(info.completion_error == 0xff05);
        trigger.join();
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_ALREADY_CALLED);
        assert(completions == 1 && triggers == 1);
    } else if (scenario == "notify-before-counter") {
        rpmb_diag::notify_request(0);
        assert(run(MX_RPMB_COUNTER).result == MX_RPMB_BUSY);
        assert(!triggers);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_OK);
        assert(completions == 1);
    } else if (scenario == "duplicate") {
        rpmb_diag::notify_request(0);
        rpmb_diag::notify_request(1);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_AMBIGUOUS_REQUEST);
        assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
        assert(!completions);
        return 0;
    } else if (scenario == "completion-failed") {
        completion_result = ~u64{0};
        rpmb_diag::notify_request(0);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_COMPLETION_FAILED);
        assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_ALREADY_CALLED);
        assert(run(MX_RPMB_RELEASE).result == MX_RPMB_BUSY);
        assert(completions == 1);
        return 0;
    } else assert(false);
    assert(run(MX_RPMB_RELEASE).result == MX_RPMB_OK);
    assert(run(MX_RPMB_COUNTER).result == MX_RPMB_BUSY);
    assert(run(MX_RPMB_COMPLETE).result == MX_RPMB_BUSY);
    assert(run(99).result == MX_RPMB_BAD_ACTION);
    rpmb_diag::ResearchAccess research;
    assert(research);
}
