// Production gadget policy and DWC2 executor on separate real SPSC rings.
// The host model sends wire-level stages, including status packets that arrive
// before a process can post RECEIVE. No phone or replacement policy is used.
#include "usb_device_model.h"
#include "../userland/usb/gadget/control.h"
#include <cstdio>
namespace gadget = mochi::usb::gadget;
struct Fixture {
    Storage commands, events;
    mx_u8_t payload[8192]{};
    Model hardware;
    Device device{};
    gadget::Runtime policy{};
    Fixture() {
        device.payload = libk::as_bytes(payload);
        device.link = {&events.page, &commands.page, 3, 1, 1};
        policy.links[0] = {&commands.page, &events.page, 3, 1, 1};
        policy.payload = libk::as_bytes(payload);
        Geometry geometry{};
        assert(decode_geometry(0x4f54310a, 0, 0x22affc50, 0x0f80d4e8, 0xfff08060, &geometry));
        host_now = 0;
        assert(device.activate(hardware.registers(), geometry));
        device.poll();
        host_now = 150000;
        device.poll();
        assert(device.connect_phase == DeviceConnectPhase::Connected);
    }
    void setup(const MxUsbSetup& request) {
        hardware.setup(request);
        device.poll();
        gadget::service(policy);
    }
};

static void address_then_descriptor() {
    Fixture f;
    f.setup({0, 5, 1, 0, 0});
    assert(f.policy.links[0].tx().depth() == 2); // ADDRESS precedes status SEND
    assert((f.hardware.words[kDeviceConfig / 4] & (0x7fu << 4)) == 0);
    f.device.poll(); // drains ADDRESS and status SEND in the same turn
    assert(f.device.activity.commands == 2);
    assert(f.device.in[0].active && f.device.address_status_active);
    assert((f.hardware.words[kDeviceConfig / 4] & (0x7fu << 4)) == (1u << 4));
    assert(f.device.address == 0); // software commits only after status ACK
    f.hardware.ack_in();
    // The next SETUP and previous ACK can be observed in one controller poll.
    f.hardware.setup({0x80, 6, 0x100, 0, 18});
    f.device.poll();
    assert(f.device.address == 1 && !f.device.address_pending);
    gadget::service(f.policy);
    f.device.poll();
    f.device.poll();
    assert(f.hardware.tx_bytes == 18 && f.hardware.tx[0] == 18);
    assert(f.hardware.tx[7] == 64 && f.hardware.tx[8] == 0x09 && f.hardware.tx[9] == 0x12);
    // The host's OUT status arrives immediately after the IN ACK, before the
    // gadget process gets CPU time to ask for it. PIO has no OUT XFRC here.
    f.hardware.ack_in();
    f.hardware.out_packet(0);
    f.device.poll();
    assert(f.device.early_out.received && f.device.early_out.complete);
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::StatusOut);
    f.device.poll();
    f.device.poll();
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::Idle);
    assert(f.device.activity.out_complete == 1 && !f.device.early_out.received);
    // A duplicated endpoint IRQ cannot publish that PIO completion twice.
    f.hardware.words[Device::reg(false, 0, 8) / 4] |= 1;
    f.device.poll();
    assert(f.device.activity.out_complete == 1 && f.policy.links[0].rx().depth() == 0);
}

static void staged_address_backpressure_and_abort() {
    Fixture f;
    // Fill the command ring so only ADDRESS fits. The policy must retry SEND
    // without either preparing the hardware early or repeating ADDRESS.
    const MxDevicePacket filler{};
    f.hardware.setup({0, 5, 7, 0, 0});
    f.device.poll();
    for (unsigned i = 0; i < 7; ++i) {
        assert(f.policy.links[0].tx().try_push_msg(MX_DEVICE_STALL, filler));
    }
    gadget::service(f.policy);
    assert(!f.policy.address_pending && !f.policy.ep0_active);
    for (unsigned i = 0; i < 8; ++i) {
        f.device.poll();
    }
    assert(f.device.address_pending && f.device.next_address == 7);
    assert((f.hardware.words[kDeviceConfig / 4] & (0x7fu << 4)) == 0);
    gadget::service(f.policy);
    f.device.poll();
    assert(f.device.address_status_active && f.device.in[0].active);
    // A new SETUP before status ACK cancels the staged address and its bytes.
    f.hardware.setup({0x80, 6, 0x100, 0, 8});
    f.device.poll();
    assert(f.device.address == 0 && !f.device.address_pending);
    assert((f.hardware.words[kDeviceConfig / 4] & (0x7fu << 4)) == 0);
    assert(!f.device.in[0].active);
}

static void early_control_out_and_reset() {
    Fixture f;
    // Setup data is not published before its distinct phase-complete record.
    const mx_u8_t request[]{0x21, 0x20, 0, 0, 0, 0, 7, 0};
    f.hardware.receive(6, 0, libk::as_bytes(request));
    f.device.poll();
    assert(f.device.setup_received && f.policy.links[0].rx().depth() == 0);
    f.hardware.receive(4, 0);
    f.device.poll();
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::OutClass);
    // Let bytes arrive ahead of the posted receive command. The executor
    // already admitted a receive window and owns the fixed private row.
    const mx_u8_t coding[]{0, 0xc2, 1, 0, 0, 0, 8};
    f.hardware.out_packet(0, libk::as_bytes(coding));
    f.device.poll(); // drains bytes, then consumes the previously queued RECEIVE
    assert(f.device.out[0].terminal && f.device.out[0].actual == 7);
    assert(!f.device.early_out.received);
    for (unsigned i = 0; i < 7; ++i) {
        assert(f.payload[i] == coding[i]);
    }
    f.device.poll();
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::Function);
    // Another early packet belongs only to its current generation/setup.
    f.hardware.out_packet(0);
    f.device.poll();
    assert(f.device.early_out.complete);
    f.hardware.words[kInterruptStatus / 4] |= 1u << 12;
    f.device.poll();
    assert(!f.device.early_out.received && !f.device.address_pending);
}

static void configure_and_stall_backpressure() {
    Fixture f;
    const MxDevicePacket filler{};
    f.hardware.setup({0, 9, 1, 0, 0});
    f.device.poll();
    for (unsigned i = 0; i < 7; ++i) {
        assert(f.policy.links[0].tx().try_push_msg(MX_DEVICE_STALL, filler));
    }
    gadget::service(f.policy);
    assert(f.policy.configure_next == 1 && !f.policy.ep0_active);
    assert(gadget::retry_pending(f.policy));
    f.device.poll();
    gadget::service(f.policy);
    assert(f.policy.configure_next == gadget::kFunctionEndpointCount && f.policy.ep0_active);
    f.device.poll();
    assert(f.device.in[0].active && f.device.out[1].configured && f.device.in[5].configured);
    // An unsupported request retains its stall while the command ring is full.
    f.hardware.setup({0x80, 0xff, 0, 0, 0});
    f.device.poll();
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.policy.links[0].tx().try_push_msg(MX_DEVICE_STALL, filler));
    }
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::Stall);
    f.device.poll();
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::Idle);
    f.device.poll();
    assert(f.hardware.words[Device::reg(true, 0) / 4] & (1u << 21));
}

static void function_tx_batch_and_epoch() {
    Fixture f;
    Storage to_function[2], from_function[2];
    for (unsigned i = 0; i < 2; ++i) {
        f.policy.links[i + 1] = {&to_function[i].page, &from_function[i].page, 3, 1, 1};
    }
    f.device.speed = MxUsbSpeed::High;
    f.setup({0, 9, 1, 0, 0});
    f.device.poll();
    const auto generation = f.policy.function_generation;
    assert(generation);
    MxFunctionChunk chunk{generation, 1, 32, 0, 0, {}};
    for (unsigned i = 0; i < 8; ++i) {
        chunk.data[0] = i;
        assert(f.policy.links[1].rx().try_push_msg(MX_FUNCTION_TX, chunk));
    }
    gadget::service(f.policy);
    assert(f.policy.functions[0].tx_bytes == 256 && f.policy.functions[0].tx_active);
    f.device.poll();
    assert(f.device.in[2].active && f.device.in[2].length == 256);
    assert(f.hardware.tx_bytes == 256);
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.hardware.tx[i * 32] == i);
    }
    const auto old_id = f.policy.functions[0].tx_id;
    gadget::completion(
        f.policy, {old_id + 1, f.policy.generation, MxUsbStatus::Success, 256, 0x82, 0, 0, {}});
    assert(f.policy.functions[0].tx_active); // unrelated completion cannot retire its slice
    f.hardware.ack_in(2);
    f.device.poll();
    gadget::service(f.policy);
    assert(!f.policy.functions[0].tx_active && !f.policy.functions[0].tx_bytes);
    gadget::reset_functions(f.policy);
    assert(f.policy.function_generation != generation);
    assert(f.policy.links[1].rx().try_push_msg(MX_FUNCTION_TX, chunk));
    gadget::service(f.policy);
    assert(!f.policy.functions[0].tx_bytes); // queued old-function bytes are discarded
}

static void reset_notice_backpressure_drains_obsolete_traffic() {
    Fixture f;
    Storage outgoing, incoming;
    f.policy.links[1] = {&outgoing.page, &incoming.page, 3, 1, 1};
    f.policy.configuration = 1;
    gadget::reset_functions(f.policy);
    const MxFunctionChunk old{f.policy.function_generation - 1, 1, 1, 0, 0, {'x'}};
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.policy.links[1].tx().try_push_msg(MX_FUNCTION_RX, old));
        assert(f.policy.links[1].rx().try_push_msg(MX_FUNCTION_TX, old));
    }
    assert(gadget::service_functions(f.policy));
    assert(f.policy.functions[0].reset && !f.policy.links[1].rx().depth());
    assert(!f.policy.functions[0].tx_bytes);
    MxRingSlot slot{};
    while (f.policy.links[1].tx().try_pop(&slot)) {
    }
    assert(gadget::service_functions(f.policy));
    assert(!f.policy.functions[0].reset);
    MxFunctionChunk reset{};
    assert(f.policy.links[1].tx().try_pop(&slot));
    assert(mochi::message_get(slot, &reset, MX_FUNCTION_RESET));
    assert(reset.flags == MX_FUNCTION_CONFIGURED);
}

int main() {
    address_then_descriptor();
    staged_address_backpressure_and_abort();
    early_control_out_and_reset();
    configure_and_stall_backpressure();
    function_tx_batch_and_epoch();
    reset_notice_backpressure_drains_obsolete_traffic();
    std::puts("usb gadget control probe ok");
}
