// Production device executor driven by W1C / FIFO register effects. The model
// publishes the real shared-ring messages; no hardware or phone is involved.
#include "usb_device_model.h"
#include <cstdio>
static void a_packet_waits_for_its_full_fifo_capacity() {
    constexpr unsigned lengths[]{7, 512};
    for (const unsigned length : lengths) {
        mx_u8_t payload[8192]{};
        Device device{};
        Model model{};
        device.payload = libk::as_bytes(payload);
        Geometry geometry{};
        assert(decode_geometry(0x4f54310a, 0, 0x22affc50, 0x0f80d4e8, 0xfff08060, &geometry));
        host_now = 0;
        assert(device.activate(model.registers(), geometry));
        device.poll(); host_now = 150000; device.poll();
        auto& row = device.in[2];
        row.configured = true; row.max_packet = 512; row.type = MxUsbTransferType::Bulk;
        for (unsigned i = 0; i < length; ++i) payload[1536 + i] = i;
        model.tx_space[2] = (length + 3) / 4 - 1;
        MxDevicePacket send{1, device.generation, 1536, length, 0, 0x82,
                            MxUsbTransferType::Bulk, 0, 0};
        MxRingSlot slot{};
        mochi::message_put(&slot, MX_DEVICE_SEND, send);
        device.command(slot);
        assert(model.tx_calls == 0 && row.tx_bytes == 0 && !row.queued);
        model.tx_space[2] = (length + 3) / 4;
        device.fill_in_fifo(2);
        assert(model.tx_calls == 1 && model.tx_bytes == length && row.tx_bytes == length && row.queued);
        for (unsigned i = 0; i < length; ++i) assert(model.tx[i] == static_cast<mx_u8_t>(i));
    }
}
static void detach_waits_for_connect_flush_without_exposing_the_pullup() {
    mx_u8_t payload[8192]{};
    Device device{};
    Model model{};
    device.payload = libk::as_bytes(payload);
    Geometry geometry{};
    assert(decode_geometry(0x4f54310a, 0, 0x22affc50, 0x0f80d4e8, 0xfff08060, &geometry));
    host_now = 0;
    model.hold_flush = true;
    assert(device.activate(model.registers(), geometry));
    assert(!device.quiesce() && device.active);
    assert(model.accesses_during_flush == 0);
    assert(model.words[kDeviceControl / 4] & (1u << 1));
    model.hold_flush = false;
    model.words[kReset / 4] = kAhbIdle;
    assert(!device.quiesce());
    host_now = 149999;
    assert(!device.quiesce());
    host_now = 150000;
    assert(device.quiesce() && !device.active);
    assert(model.words[kDeviceControl / 4] & (1u << 1));
    assert(model.accesses_during_flush == 0);
}
static void setup_abort_waits_for_flush_before_other_controller_io() {
    Storage requests, replies;
    mx_u8_t payload[8192]{};
    Device device{};
    Model model{};
    device.payload = libk::as_bytes(payload);
    device.link = {&replies.page, &requests.page, 3, 1, 1};
    Geometry geometry{};
    assert(decode_geometry(0x4f54310a, 0, 0x22affc50, 0x0f80d4e8, 0xfff08060, &geometry));
    host_now = 0;
    assert(device.activate(model.registers(), geometry));
    device.poll();
    host_now = 150000;
    device.poll();
    model.setup(18);
    device.poll();
    MxRingSlot slot{};
    MxDeviceEvent first{};
    assert(device.link.tx().try_pop(&slot) && mochi::message_get(slot, &first, MX_DEVICE_SETUP));
    MxDevicePacket send{first.id, device.generation, 512, 18, 0, 128,
                        MxUsbTransferType::Control, 0, 0};
    mochi::message_put(&slot, MX_DEVICE_SEND, send);
    device.command(slot);
    assert(device.in[0].active);
    const auto old_tx_bytes = model.tx_bytes;
    // Another function has a retained packet to refill while EP0 is aborted.
    auto& bulk = device.in[2];
    bulk.configured = bulk.active = true;
    bulk.max_packet = 512;
    bulk.offset = 1536;
    bulk.length = 4;
    model.hold_flush = true;
    model.setup(8);
    device.poll();
    model.words[Device::reg(true, 0) / 4] |= 1u << 31;
    model.words[Device::reg(true, 0, 8) / 4] &= ~(1u << 6);
    device.poll();
    assert(!(model.words[kReset / 4] & kTxFlush)); // MAC has not acknowledged NAK
    model.words[Device::reg(true, 0, 8) / 4] |= 1u << 6;
    model.words[kReset / 4] = 0;
    device.poll();
    assert(!(model.words[kReset / 4] & kTxFlush)); // AHB is still busy
    model.words[kReset / 4] = kAhbIdle;
    device.poll();
    device.poll();
    assert(model.accesses_during_flush == 0);
    assert(model.pending() && device.setup_flushing);
    assert(!bulk.queued && model.tx_bytes == old_tx_bytes);
    assert(!device.quiesce() && device.active);
    assert(model.accesses_during_flush == 0);
    // Hardware completes the flush. Its required PHY settle interval must
    // finish before the pending SETUP completion or bulk refill is consumed.
    model.words[kReset / 4] = kAhbIdle;
    model.hold_flush = false;
    ++host_now;
    device.poll();
    assert(model.pending() && !bulk.queued);
    host_now += 999;
    device.poll();
    assert(model.pending() && !bulk.queued);
    ++host_now;
    device.poll();
    assert(!model.pending() && !device.setup_flushing && bulk.queued);
    assert(model.accesses_during_flush == 0);
    MxDeviceEvent next{};
    assert(device.link.tx().try_pop(&slot) && mochi::message_get(slot, &next, MX_DEVICE_SETUP));
    assert(next.id > first.id && next.setup.length == 8);
}
static void all_implemented_fifos_have_disjoint_packet_ram() {
    mx_u8_t payload[8192]{};
    Device device{};
    Model model{};
    device.payload = libk::as_bytes(payload);
    Geometry geometry{};
    assert(decode_geometry(0x4f54310a, 0, 0x22affc50, 0x0f80d4e8, 0xfff08060, &geometry));
    // Real v40 register dump: untouched FIFOs 7..14 point beyond the 3968
    // packet words; FIFO15 covers all 4096 words, including RX. Activation
    // must replace these defaults even when no transfer currently uses them.
    for (unsigned i = 7; i <= 15; ++i)
        model.words[(0x104 + (i - 1) * 4) / 4] =
            0x10000000u | (((i + 1) * 0x1000u) & 0xffffu);
    assert(device.activate(model.registers(), geometry));
    const auto rx = model.words[kRxFifoSize / 4];
    assert(rx >= 512 && rx <= geometry.fifo_words);
    FifoRegion regions[16]{};
    for (unsigned i = 0; i <= geometry.in_endpoints; ++i) {
        const auto value = model.words[(i ? 0x104 + (i - 1) * 4 : kNonperiodicTxSize) / 4];
        regions[i] = {static_cast<mx_u16_t>(value), static_cast<mx_u16_t>(value >> 16)};
        assert(regions[i].words >= 128); // one admitted maximum-size packet
        assert(valid_region(regions[i], geometry.fifo_words));
        assert(regions[i].start >= rx);
        for (unsigned j = 0; j < i; ++j) assert(disjoint(regions[i], regions[j]));
    }
    assert(model.rx_payload_reads == 0 && model.raw_fifo_reads == 0);
    // Insufficient RAM must fail before exposing or programming the gadget.
    Geometry small = geometry;
    small.fifo_words = 1024;
    Device rejected{};
    rejected.payload = libk::as_bytes(payload);
    Model untouched{};
    assert(!rejected.activate(untouched.registers(), small));
    assert(!rejected.active && !untouched.words[kRxFifoSize / 4]);
    // Endpoint numbers cannot select a TX FIFO absent from this core.
    Geometry fewer = geometry;
    fewer.in_endpoints = 4;
    Device limited{};
    Model reduced{};
    limited.payload = libk::as_bytes(payload);
    assert(limited.activate(reduced.registers(), fewer));
    MxDevicePacket configure{1, limited.generation, 0, 0, 512, 0x87,
                             MxUsbTransferType::Bulk, 0, 0};
    MxRingSlot slot{};
    mochi::message_put(&slot, MX_DEVICE_CONFIGURE, configure);
    limited.command(slot);
    assert(!limited.in[7].configured && !reduced.words[Device::reg(true, 7) / 4]);
    configure.endpoint = 0x84;
    mochi::message_put(&slot, MX_DEVICE_CONFIGURE, configure);
    limited.command(slot);
    assert(limited.in[4].configured && limited.in[4].max_packet == 512);
}
static void status_only_entries_do_not_consume_payload() {
    Storage requests, replies;
    mx_u8_t payload[8192]{};
    Device device{};
    Model model{};
    device.payload = libk::as_bytes(payload);
    device.link = {&replies.page, &requests.page, 3, 1, 1};
    Geometry geometry{};
    assert(decode_geometry(0x4f54310a, 0, 0x22affc50, 0x0f80d4e8, 0xfff08060, &geometry));
    host_now = 0;
    assert(device.activate(model.registers(), geometry));
    model.receive(6, 0, libk::as_bytes("\x21\x20\0\0\0\0\x07\0", 8));
    // Only DATA and SETUP updates own FIFO bytes. A status-only entry's
    // count must never consume the following packet or trigger an overflow.
    model.status(4, 0, 0x7ff); // SETUP completion
    model.status(1, 0, 7);     // global OUT NAK
    model.status(15, 0, 0x7ff); // unrecognised status
    device.receive_fifo();
    assert(model.rx_payload_reads == 1 && model.raw_fifo_reads == 0);
    assert(device.rx_audit_reported); // malformed header observed without draining data
    assert(device.setup_pending && device.setup.setup.request == 0x20);
    assert(device.setup.setup.length == 7);

    MxDevicePacket receive{device.setup.id, device.generation, 0, 7, 0, 0,
                           MxUsbTransferType::Control, 0, 0};
    MxRingSlot slot{};
    mochi::message_put(&slot, MX_DEVICE_RECEIVE, receive);
    device.command(slot);
    const mx_u8_t coding[] = {0, 0xc2, 1, 0, 0, 0, 8};
    model.receive(2, 0, libk::as_bytes(coding));
    model.status(3, 0, 0x7ff); // OUT completion has no payload
    device.receive_fifo();
    assert(model.rx_payload_reads == 2 && model.raw_fifo_reads == 0);
    assert(device.out[0].terminal && device.out[0].actual == sizeof(coding));
    for (unsigned i = 0; i < sizeof(coding); ++i) assert(payload[i] == coding[i]);
}
int main() {
    detach_waits_for_connect_flush_without_exposing_the_pullup();
    a_packet_waits_for_its_full_fifo_capacity();
    setup_abort_waits_for_flush_before_other_controller_io();
    all_implemented_fifos_have_disjoint_packet_ram();
    status_only_entries_do_not_consume_payload();
    host_now = 0;
    Storage requests,replies;
    mx_u8_t payload[8192]{};
    Device device{}; Model model{};
    device.payload=libk::as_bytes(payload);
    device.link={&replies.page,&requests.page,3,1,1};
    Geometry geometry{};
    assert(decode_geometry(0x4f54310a,0,0x22affc50,0x0f80d4e8,0xfff08060,&geometry));
    assert(geometry.host_channels==16&&geometry.device_endpoints==16&&geometry.fifo_words==3968);
    assert(device.activate(model.registers(),geometry));
    assert(!(model.words[kAhbConfig/4]&kDmaEnable));
    assert(model.words[kDeviceControl/4]&(1u<<1));
    // Host traffic cannot pass while new FIFO geometry is being flushed or
    // before the disconnect interval ends. The model independently owns time.
    model.setup(18); device.poll();
    assert(model.pending()&&device.connect_phase==DeviceConnectPhase::Settling);
    host_now=149999; device.poll();
    assert(model.pending()&&(model.words[kDeviceControl/4]&(1u<<1)));
    host_now=150000; device.poll();
    assert(!model.pending()&&device.connect_phase==DeviceConnectPhase::Connected);
    assert(!device.rx_audit_reported); // normal peek/pop must agree
    assert(!(model.words[kDeviceControl/4]&(1u<<1))&&device.activity.setups==1);
    MxRingSlot slot{}; MxDeviceEvent event{};
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    model.words[kUsbConfig/4]=0x40001400; // reset/default timing, as seen on phone
    model.words[kInterruptStatus/4]|=1u<<13; device.poll();
    assert(device.activity.enumerations==1&&(model.words[kUsbConfig/4]&(15u<<10))==(9u<<10));
    device.poll(); assert(device.activity.enumerations==1); // W1C is one observation
    model.setup(18); device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    assert(event.setup.length==18);
    const auto setup_id=event.id;
    payload[512]=18; payload[513]=1;
    MxDevicePacket send{setup_id,device.generation,512,18,0,128,MxUsbTransferType::Control,0,0};
    assert(device.link.rx().try_push_msg(MX_DEVICE_SEND,send));
    device.poll(); assert(device.in[0].active);
    assert(device.activity.commands==1);
    device.poll(); assert(model.tx_bytes==18&&model.tx[0]==18);
    model.ack_in();
    device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_COMPLETE));
    assert(event.id==setup_id&&event.endpoint==128&&event.length==18);
    assert(device.activity.in_complete==1);
    // A newer SETUP invalidates late replies and never lets their bytes start.
    model.setup(8); device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    assert(event.id>setup_id);
    assert(device.link.rx().try_push_msg(MX_DEVICE_SEND,send)); device.poll();
    assert(!device.in[0].active);
    // An aborting SETUP fences old IN bytes before a new setup can be offered.
    send.id=event.id; send.generation=device.generation;
    assert(device.link.rx().try_push_msg(MX_DEVICE_SEND,send)); device.poll();
    assert(device.in[0].active);
    model.setup(8); device.poll();
    assert(!device.in[0].active&&device.setup_flushing);
    device.poll(); // quiescent reader/AHB -> start flush
    device.poll(); // observe completion -> PHY settle interval
    host_now += 1000;
    device.poll();
    assert(!device.setup_flushing && model.accesses_during_flush == 0);
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    // Bus reset invalidates endpoint work even if an old completion is pending.
    model.words[kInterruptStatus/4]|=1u<<12; device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_RESET));
    assert(event.generation>send.generation&&!device.in[0].active);
    assert(device.activity.resets==1&&device.activity.setups==4);
    // A stuck hardware flush never exposes the pull-up, even at timeout.
    Device blocked{}; Model stuck{};
    blocked.payload=libk::as_bytes(payload); stuck.hold_flush=true;
    assert(blocked.activate(stuck.registers(),geometry));
    assert(!blocked.advance_connect(host_now+100000000ull));
    assert(blocked.connect_phase==DeviceConnectPhase::Fault&&!blocked.active);
    assert(stuck.words[kDeviceControl/4]&(1u<<1));
    std::puts("usb device probe ok");
}
