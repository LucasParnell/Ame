// Production gadget policy and DWC2 executor on separate real SPSC rings.
// The host model sends wire-level stages, including status packets that arrive
// before a process can post RECEIVE. No phone or replacement policy is used.
#include "usb_device_model.h"
#include "../userland/usb/gadget/control.h"
#include "../userland/usb/functions/acm/bridge.h"
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
    void settle_abort() {
        if (device.setup_flushing) {
            device.poll(); // start the acknowledged EP0 flush
            device.poll(); // observe hardware completion
            host_now += 1000;
            device.poll(); // PHY clocks have elapsed
            assert(!device.setup_flushing && !hardware.accesses_during_flush);
        }
    }
    void setup(const MxUsbSetup& request) {
        hardware.setup(request);
        device.poll();
        settle_abort();
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

static void posted_control_out_keeps_live_window() {
    Fixture f;
    f.setup({0x21, 0x20, 0, 0, 7});
    assert(f.hardware.words[Device::reg(false, 0) / 4] & (1u << 31));
    const auto size = f.hardware.words[Device::reg(false, 0, 0x10) / 4];
    f.device.poll(); // RECEIVE adopts the window armed at SETUP completion
    assert(f.device.out[0].active && f.device.out[0].length == 7);
    assert(f.hardware.live_ep0_size_writes == 0);
    assert(f.hardware.words[Device::reg(false, 0, 0x10) / 4] == size);
    const mx_u8_t coding[]{0, 0xc2, 1, 0, 0, 0, 8};
    f.hardware.out_packet(0, libk::as_bytes(coding));
    f.device.poll();
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::Function);
    assert(f.policy.function_request.length == 7);
    for (unsigned i = 0; i < 7; ++i) {
        assert(f.policy.function_request.data[i] == coding[i]);
    }
}

static void delayed_out_irq_cannot_complete_next_receive() {
    Fixture f;
    f.setup({0x21, 0x20, 0, 0, 7});
    f.device.poll();
    assert(f.device.out[0].active);
    // A delayed endpoint XFRC (including SETUP's XFRC) is not PIO data.
    // Only RX FIFO OUTDONE can finish this receive.
    f.hardware.words[Device::reg(false, 0, 8) / 4] |= 1;
    f.device.poll();
    gadget::service(f.policy);
    assert(f.device.out[0].active && !f.device.out[0].terminal);
    assert(f.device.activity.out_complete == 0);
    assert(f.policy.phase == gadget::ControlPhase::OutClass);
    const mx_u8_t coding[]{0, 0xc2, 1, 0, 0, 0, 8};
    f.hardware.out_packet(0, libk::as_bytes(coding), true);
    f.device.poll();
    gadget::service(f.policy);
    assert(f.device.activity.out_complete == 1);
    assert(f.policy.phase == gadget::ControlPhase::Function);
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
    f.settle_abort();
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

static void control_reply_does_not_wait_for_bulk_in() {
    Fixture f;
    Storage to_function, from_function;
    f.policy.acm_control = {&to_function.page, &from_function.page, 3, 1, 1};
    f.policy.configuration = 1;
    f.policy.function_generation = 7;
    f.policy.functions[0].tx_active = true; // host has paused CDC IN reads
    f.policy.functions[0].tx_id = 91;
    f.policy.functions[0].tx_bytes = 512;
    f.setup({0x21,0x22,3,0,0});
    assert(f.policy.phase == gadget::ControlPhase::Function);
    MxFunctionSetup reply=f.policy.function_request;
    reply.status=MX_OK;
    assert(f.policy.acm_control.rx().try_push_msg(MX_FUNCTION_SETUP_REPLY,reply));
    gadget::service(f.policy);
    assert(f.policy.phase == gadget::ControlPhase::StatusIn);
    assert(f.policy.functions[0].tx_active); // response cannot retire owned bytes
    assert(f.policy.functions[0].tx_bytes == 512);
}

static void endpoint_observation_preserves_outstanding_transfer() {
    Fixture f;
    auto& row=f.device.in[2];
    row.configured=row.active=row.queued=true;
    row.length=row.tx_bytes=256;
    row.id=99;
    const auto control=(1u<<31)|(1u<<15)|512u;
    f.hardware.words[Device::reg(true,2)/4]=control;
    f.hardware.words[Device::reg(true,2,0x10)/4]=123;
    f.setup({0xc0,0x5b,0,0x82,sizeof(MxDeviceSnapshot)});
    assert(f.policy.phase==gadget::ControlPhase::Observe);
    f.device.poll(); // accepts the bounded observation command
    f.device.poll(); // publishes it without retiring the outstanding IN row
    gadget::service(f.policy);
    assert(f.policy.length==sizeof(MxDeviceSnapshot));
    MxDeviceSnapshot snapshot{};
    assert(libk::copy_n(libk::as_bytes(&snapshot,sizeof(snapshot)),
        libk::slice(f.policy.payload,512,sizeof(snapshot)),sizeof(snapshot)));
    assert(snapshot.id==f.policy.observe_id && snapshot.generation==f.device.generation);
    assert(snapshot.control==control && snapshot.transfer_size==123);
    assert((snapshot.flags&31)==23 && snapshot.length==256 && snapshot.tx_bytes==256);
    assert(row.id==99 && row.active && row.queued && !row.terminal);
    f.setup({0xc0,0x5b,0,0x89,sizeof(MxDeviceSnapshot)});
    f.device.poll();
    assert(f.hardware.words[Device::reg(true,0)/4] & (1u<<21));
}

static void control_history_survives_diagnostic_setup() {
    Fixture f;
    f.setup({0x21,0x20,0,0,7});
    f.device.poll();
    const mx_u8_t coding[]{0,0xc2,1,0,0,0,8};
    f.hardware.out_packet(0,libk::as_bytes(coding));
    f.device.poll();
    const auto retained=f.device.control_trace;
    assert(retained.actual==7 && (retained.length&15u)==15u);
    assert(retained.tx_bytes==(0x21u|(0x20u<<8)|(7u<<16)));
    f.setup({0xc0,0x5c,0,0,sizeof(MxDeviceSnapshot)});
    assert(f.policy.observe_history);
    f.device.poll(); // accept the observation
    f.device.poll(); // publish its retained response
    gadget::service(f.policy);
    MxDeviceSnapshot response{};
    libk::copy_n(libk::as_bytes(&response,sizeof(response)),
                libk::slice(f.policy.payload,512,sizeof(response)),sizeof(response));
    assert(response.actual==7 && response.tx_bytes==retained.tx_bytes);
    assert(response.control==retained.control && response.interrupt==retained.interrupt);
    assert(response.transfer_size==retained.transfer_size && response.length==retained.length);
    assert(response.global_interrupt==retained.global_interrupt && response.flags==retained.flags);
    assert(response.id==f.device.sequence && response.generation==f.device.generation);
    assert(f.device.control_trace.actual==7 && !f.device.trace_active);
}

static void setup_only_window_is_preserved_for_control_in() {
    Fixture f;
    f.hardware.keep_setup_window_enabled=true;
    f.setup({0x80,6,0x100,0,18});
    assert((f.hardware.words[Device::reg(false,0,0x10)/4]&127u)==16);
    assert(f.hardware.live_ep0_size_writes==0);
    assert(f.policy.phase==gadget::ControlPhase::In);
}

static void validated_receive_transitions_setup_window_to_out_transfer() {
    Fixture f;
    f.hardware.keep_setup_window_enabled=true;
    f.device.ep0_out_trial=2;
    f.setup({0x21,0x20,0,0,7});
    // SETUP credits are not a DATA receive. Preserve them while policy has
    // not yet validated the request, then program the complete OUT transfer.
    assert((f.hardware.words[Device::reg(false,0,0x10)/4]&127u)==16);
    assert(f.hardware.words[Device::reg(false,0)/4]&(1u<<17));
    assert(f.hardware.live_ep0_size_writes==0);
    f.device.poll();
    assert(f.hardware.words[Device::reg(false,0,0x10)/4]==((3u<<29)|(1u<<19)|64u));
    assert(!(f.hardware.words[Device::reg(false,0)/4]&(1u<<17)));
    assert(f.hardware.live_ep0_size_writes==1);
    assert(f.hardware.live_ep0_size_before_setup_ack==0);
    assert(f.device.ep0_out_trial==0);
    assert((f.device.control_trace.length & (7u<<9))==((1u<<9)|(2u<<10)));
    assert(f.device.control_trace.flags==((3u<<29)|(1u<<19)|64u));
    const mx_u8_t coding[]{0,0xc2,1,0,0,0,8};
    f.hardware.out_packet(0,libk::as_bytes(coding));
    f.device.poll();gadget::service(f.policy);
    assert(f.policy.phase==gadget::ControlPhase::Function);
    assert(f.policy.function_request.length==7);
    assert(f.device.control_trace.actual==7);
}

static void status_out_keeps_baseline_and_does_not_consume_trial() {
    Fixture f;
    f.hardware.keep_setup_window_enabled=true;
    f.device.ep0_out_trial=2;
    f.setup({0x80,6,0x100,0,18});
    f.device.poll();
    assert(f.hardware.live_ep0_size_writes==0);
    assert((f.hardware.words[Device::reg(false,0,0x10)/4]&127u)==16);
    f.hardware.ack_in();
    f.device.poll();gadget::service(f.policy);
    assert(f.policy.phase==gadget::ControlPhase::StatusOut);
    f.device.poll();
    assert(f.device.out[0].active && f.device.out[0].length==0);
    assert((f.hardware.words[Device::reg(false,0,0x10)/4]&127u)==16);
    assert(f.hardware.words[Device::reg(false,0)/4]&(1u<<17));
    assert(f.hardware.live_ep0_size_writes==0);
    assert(f.hardware.live_ep0_size_before_setup_ack==0);
    assert(f.device.ep0_out_trial==2);
    f.hardware.out_packet(0);
    f.device.poll();gadget::service(f.policy);
    assert(f.policy.phase==gadget::ControlPhase::Idle);

}

static void trial_is_explicit_bounded_and_reset_scoped() {
    Fixture f;
    f.hardware.keep_setup_window_enabled=true;
    f.setup({0x21,0x20,0,0,7});f.device.poll();
    assert(f.hardware.live_ep0_size_writes==0); // unarmed default keeps v32 behavior
    assert(f.hardware.words[Device::reg(false,0)/4]&(1u<<17));
    const auto retained=f.device.control_trace;
    f.policy.configuration=1;
    f.setup({0xc0,0x5d,2,0,sizeof(MxDeviceSnapshot)});
    assert(f.policy.observe_trial);
    f.device.poll();f.device.poll();gadget::service(f.policy);
    assert(f.device.ep0_out_trial==2);
    MxDeviceSnapshot answer{};
    libk::copy_n(libk::as_bytes(&answer,sizeof(answer)),
                libk::slice(f.policy.payload,512,sizeof(answer)),sizeof(answer));
    assert(answer.flags==2 && answer.id==f.device.sequence);
    assert(f.device.control_trace.tx_bytes==retained.tx_bytes);
    assert(f.device.control_trace.flags==retained.flags); // arming preserves history
    f.setup({0xc0,0x5d,3,0,sizeof(MxDeviceSnapshot)});
    f.device.poll();
    assert((f.hardware.words[Device::reg(true,0)/4]&(1u<<21)) && f.device.ep0_out_trial==2);
    f.hardware.words[kInterruptStatus/4]|=1u<<12;
    f.device.poll();
    assert(f.device.ep0_out_trial==0);
}

static void complete_line_coding_round_trip_uses_real_acm_policy() {
    Fixture f;
    Storage requests, replies, stream_out[2], stream_in[2];
    f.policy.acm_control={&requests.page,&replies.page,3,1,1};
    f.policy.function_generation=7;
    mochi::usb::acm::Runtime acm{};
    acm.control.init();
    acm.links[2]={&replies.page,&requests.page,3,1,1};
    for(unsigned i=0;i<2;++i)acm.links[i]={&stream_out[i].page,&stream_in[i].page,3,1,1};
    f.hardware.keep_setup_window_enabled=true;
    f.device.ep0_out_trial=1;
    f.setup({0x21,0x20,0,0,7});f.device.poll();
    const mx_u8_t coding[]{0,0xe1,0,0,0,0,8}; // 57600 baud
    f.hardware.out_packet(0,libk::as_bytes(coding));
    f.device.poll();gadget::service(f.policy);
    for(unsigned i=0;i<7;++i)assert(f.policy.function_request.data[i]==coding[i]);
    assert(mochi::usb::acm::service(acm));
    assert(mochi::usb::acm::service(acm));
    gadget::service(f.policy);f.device.poll();
    for(unsigned i=0;i<7;++i)assert(acm.control.coding[i]==coding[i]);
    assert(f.policy.phase==gadget::ControlPhase::StatusIn);
    assert(f.device.in[0].active && f.device.in[0].length==0);
    assert(!(f.hardware.words[Device::reg(true,0)/4]&(1u<<21)));
    f.hardware.ack_in();f.device.poll();gadget::service(f.policy);
    assert(f.policy.phase==gadget::ControlPhase::Idle);
    assert(f.policy.last_out_reply_status==MX_OK && f.policy.last_out_reply_length==0);
    f.setup({0xc0,0x5e,0,0,48});
    assert(f.policy.length==48);
    const auto* snapshot=f.payload+512;
    assert(libk::load_le<mx_u32_t>(snapshot)==0x31534341u);
    assert(libk::load_le<mx_u32_t>(snapshot+12)==7);
    assert(libk::load_le<mx_u32_t>(snapshot+16)==MX_OK);
    for(unsigned i=0;i<7;++i)assert(snapshot[32+i]==coding[i]);
}

static void log_tail_pages_are_bounded_to_frozen_window() {
    host_on_debug_read=[](mx_u64_t request,mx_u64_t offset,void* out,mx_u64_t size)->mx_i64_t {
        if(request==MX_DIAG_SYSTEM && size==sizeof(MxDiagSystem)) {
            auto& info=*static_cast<MxDiagSystem*>(out);info={};
            info.boot_log_size=9000;info.boot_log_capacity=16384;return sizeof(info);
        }
        if(request==MX_DIAG_BOOT_LOG) {
            assert(offset>=808 && offset+size<=9000 && size<=64);
            for(mx_u64_t i=0;i<size;++i)static_cast<mx_u8_t*>(out)[i]=(offset+i)&255;
            return size;
        }
        return MX_ERR_INVALID;
    };
    Fixture f;
    f.setup({0xc0,0x5f,0,0,16});
    assert(f.policy.length==16 && f.policy.log_tail_start==808 && f.policy.log_tail_length==8192);
    assert(libk::load_le<mx_u32_t>(f.payload+512)==0x31474c42u);
    f.setup({0xc0,0x5f,1,127,64});
    assert(f.policy.length==64);
    for(unsigned i=0;i<64;++i)assert(f.payload[512+i]==((8936+i)&255));
    f.setup({0xc0,0x5f,1,128,64});f.device.poll();
    assert(f.hardware.words[Device::reg(true,0)/4]&(1u<<21));
    host_on_debug_read=nullptr;
}

int main() {
    log_tail_pages_are_bounded_to_frozen_window();
    complete_line_coding_round_trip_uses_real_acm_policy();
    trial_is_explicit_bounded_and_reset_scoped();
    validated_receive_transitions_setup_window_to_out_transfer();
    status_out_keeps_baseline_and_does_not_consume_trial();
    control_history_survives_diagnostic_setup();
    setup_only_window_is_preserved_for_control_in();
    endpoint_observation_preserves_outstanding_transfer();
    control_reply_does_not_wait_for_bulk_in();
    address_then_descriptor();
    staged_address_backpressure_and_abort();
    early_control_out_and_reset();
    delayed_out_irq_cannot_complete_next_receive();
    posted_control_out_keeps_live_window();
    configure_and_stall_backpressure();
    function_tx_batch_and_epoch();
    reset_notice_backpressure_drains_obsolete_traffic();
    std::puts("usb gadget control probe ok");
}
