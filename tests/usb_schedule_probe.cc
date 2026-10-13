// Production service scheduling and stream retention, with the existing host
// syscall seam. Test wake races and ownership, rather than matching code text.
#include "mochi/usb/function_stream.h"
#include "../userland/usb/functions/acm/control.h"
#include "../userland/drivers/usb/dwc2/host.h"
#include <cassert>
#include <cstdio>
struct Storage {
    MxRingPage page{};
    MxRingSlot slots[8]{};
};
static bool waited;
static mx_u64_t expected_counter;
static mx_i64_t wait_once() {
    waited = true;
    return MX_OK;
}
static void wakes_and_budget() {
    Storage input;
    mochi::SpscEndpoint link{nullptr, &input.page, 3, 1, 1};
    MxFunctionChunk chunk{};
    assert(link.rx().try_push_msg(MX_FUNCTION_RX, chunk));
    host_on_wait = wait_once;
    // Retained output makes input temporarily unconsumable. A queued slot
    // must not prevent the bounded sleep or set an unusable wake request.
    waited = false;
    mochi::driver::idle(&link, 1, 1, 1000000, 0);
    assert(waited && !input.page.flags.receiver_about_to_sleep);
    // Consumable input closes the ordinary publish-sleep/recheck race.
    waited = false;
    mochi::driver::idle(&link, 1, 1, 1000000);
    assert(!waited && !input.page.flags.receiver_about_to_sleep);
    MxRingSlot slot{};
    assert(link.try_recv(&slot));
    // IRQ does not inspect a ring flag. Keep the pre-drain counter instead
    // of absorbing an IRQ that arrived after hardware draining finished.
    const auto before = mx_doorbell_read(1);
    mx_doorbell_notify(1);
    expected_counter = before;
    host_on_wait = []() -> mx_i64_t {
        assert(host_wait_expected == expected_counter && host_notifies != expected_counter);
        return MX_OK;
    };
    mochi::driver::idle_on_counter(&link, 1, 1, before, 0);
    assert(input.page.flags.doorbell_counter_snapshot == before);
    assert(!input.page.flags.receiver_about_to_sleep);
    host_on_wait = nullptr;
    unsigned turns = 0;
    assert(mochi::driver::run_batch([&] {
        ++turns;
        return true;
    }));
    assert(turns == mochi::driver::kBatchLimit);
    turns = 0;
    assert(!mochi::driver::run_batch([&] { return ++turns < 3; }));
    assert(turns == 3);
}
static void retained_stream_and_cdc() {
    Storage output;
    mochi::Ring ring{&output.page, 3, 1};
    MxFunctionChunk chunk{1, 7, 1, 0, 0, {'a'}};
    for (unsigned i = 0; i < 8; ++i) {
        assert(ring.try_push_msg(MX_FUNCTION_TX, chunk));
    }
    mochi::usb::PendingFrame pending{};
    MxRingSlot incoming{};
    mochi::message_put(&incoming, MX_FUNCTION_TX, chunk);
    incoming.seq = 1234;
    assert(pending.retain(incoming));
    assert(!pending.flush(ring) && pending.held && pending.slot.seq == 1234);
    MxRingSlot slot{};
    assert(ring.try_pop(&slot) && pending.flush(ring) && !pending.held);
    for (unsigned i = 0; i < 8; ++i) {
        assert(ring.try_pop(&slot));
    }
    assert(slot.seq == 1234 && slot.payload[0] == incoming.payload[0]);
    mochi::usb::acm::Control cdc{};
    cdc.init();
    MxFunctionSetup request{};
    request.setup = {0xa1, 0x21, 0, 0, 7};
    cdc.reply(request);
    assert(request.status == MX_OK && request.length == 7);
    assert(libk::load_le<mx_u32_t>(request.data) == 115200 && request.data[6] == 8);
    request.setup = {0x21, 0x20, 0, 0, 7};
    request.length = 7;
    request.data[6] = 9;
    cdc.reply(request);
    assert(request.status != MX_OK && cdc.coding[6] == 8);
}
static void host_deadlines() {
    using namespace mochi::usb::dwc2;
    Host host{};
    constexpr mx_u64_t now = 10000000;
    assert(!host.wake_deadline(now));
    host.addressing = true;
    host.address_deadline = now + 2000000;
    assert(host.wake_deadline(now) == host.address_deadline);
    host.tokens[0] = 1;
    auto& row = host.transfer_rows[0];
    row.phase = TransferPhase::Data;
    row.retry = now + 125000; // high-speed interrupt retry precedes cold reset work
    row.prepared.request.deadline_ns = now + 500000;
    assert(host.wake_deadline(now) == row.retry);
    row.packet = 1; // in-flight packet relies on IRQ and transfer expiry
    assert(host.wake_deadline(now) == row.prepared.request.deadline_ns);
    row.packet = 0;
    row.retry = now - 1; // a blocked due row retries without spinning
    assert(host.wake_deadline(now) == row.prepared.request.deadline_ns);
    row.prepared.request.deadline_ns = now - 1;
    assert(host.wake_deadline(now) == now + mochi::driver::kBackpressureRetryNs);
    row.prepared.request.deadline_ns = 0;
    assert(host.wake_deadline(now) == now + mochi::driver::kBackpressureRetryNs);
}
int main() {
    wakes_and_budget();
    retained_stream_and_cdc();
    host_deadlines();
    std::puts("usb scheduling probe ok");
}
