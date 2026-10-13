// Side-effectful register model: W1C interrupts, popped RX status/FIFO data,
// queue credits and explicit channel halt. No physical hardware is accessed.
#include "mochi/usb/dwc2_transfer.h"
#include <cassert>
#include <cstdio>

using namespace mochi::usb::dwc2;
struct Model {
    mx_u32_t words[1024]{};
    mx_u32_t rx_status = 0;
    mx_u8_t rx_bytes[64]{}, tx_bytes[64]{};
    mx_u32_t rx_at = 0, rx_size = 0, tx_size = 0;
    bool rx_pending = false, fail_writes = false;
    Model() {
        words[kCoreId / 4] = 0x4f54310a;
        words[kHardware2 / 4] = (2u << 3) | (1u << 6) | (3u << 10) | (1u << 14) | (1u << 19);
        words[kHardware3 / 4] = (2048u << 16) | (6u << 4) | 8;
        words[kHardware4 / 4] = (3u << 26) | (1u << 25);
        words[kInterruptStatus / 4] = kHostMode;
        words[kReset / 4] = kAhbIdle;
        words[kNonperiodicTxStatus / 4] = words[kPeriodicTxStatus / 4] = (8u << 16) | 512;
    }
    Registers registers() {
        return {this,
            [](void* p, mx_u32_t reg, mx_u32_t* out) {
                auto& m = *static_cast<Model*>(p);
                if (!out || reg >= sizeof(m.words) || reg % 4) return false;
                if (reg == kInterruptStatus) *out = m.words[reg / 4] | (m.rx_pending ? kRxAvailable : 0);
                else if (reg == kRxStatusPop) { *out = m.rx_status; m.rx_pending = false; }
                else *out = m.words[reg / 4];
                return true;
            },
            [](void* p, mx_u32_t reg, mx_u32_t value) {
                auto& m = *static_cast<Model*>(p);
                if (m.fail_writes || reg >= sizeof(m.words) || reg % 4) return false;
                if (reg >= kChannelBase && (reg - kChannelBase) % kChannelStride == kChannelStatus)
                    m.words[reg / 4] &= ~value;
                else m.words[reg / 4] = value;
                return true;
            },
            [](void* p, mx_u32_t reg, libk::bytes destination) {
                auto& m = *static_cast<Model*>(p);
                if (reg != fifo_register(0) || m.rx_at > m.rx_size ||
                    destination.size > m.rx_size - m.rx_at) return false;
                if (!libk::copy_n(destination, libk::slice(libk::as_bytes(m.rx_bytes), m.rx_at, destination.size),
                                  destination.size)) return false;
                m.rx_at += static_cast<mx_u32_t>((destination.size + 3) & ~mx_u64_t{3});
                return true;
            },
            [](void* p, mx_u32_t reg, libk::const_bytes source) {
                auto& m = *static_cast<Model*>(p);
                if (reg < kFifoBase || reg % kFifoStride || source.size > sizeof(m.tx_bytes)) return false;
                m.tx_size = static_cast<mx_u32_t>(source.size);
                return libk::copy_n(libk::as_bytes(m.tx_bytes), source, source.size);
            }, 0x20000};
    }
    void receive(mx_u32_t channel, mx_u32_t length, Pid pid) {
        rx_at = 0;
        rx_size = (length + 3) & ~3u;
        rx_status = channel | (length << 4) | (static_cast<mx_u32_t>(pid) << 15) | (2u << 17);
        rx_pending = true;
        for (mx_u32_t i = 0; i < rx_size; ++i) rx_bytes[i] = static_cast<mx_u8_t>(i + 1);
    }
    void interrupt(mx_u32_t channel, mx_u32_t bits) {
        words[channel_register(channel, kChannelStatus) / 4] |= bits;
    }
};

static void geometry_and_reset() {
    Model m;
    auto io = m.registers();
    Geometry geometry{};
    assert(identify(io, &geometry));
    assert(geometry.core_id == 0x4f54310a && geometry.host_channels == 2 && geometry.device_endpoints == 4);
    assert(geometry.host && geometry.device && geometry.fifo_words == 2048);
    // Revisions and fixed/either-width geometry select the vendor's timing;
    // a reset-default turnaround is replaced without losing unrelated options.
    m.words[kUsbConfig/4]=(1u<<22)|(1u<<29)|(5u<<10);
    assert(configure_utmi(io,geometry,false));
    assert(m.words[kUsbConfig/4]==((1u<<22)|(1u<<30)|(9u<<10)));
    auto wide=geometry; wide.utmi_width=1;
    assert(configure_utmi(io,wide,true));
    assert(m.words[kUsbConfig/4]==((1u<<22)|(1u<<29)|(1u<<3)|(6u<<10)));
    assert(device_turnaround(io,wide.core_id,MxUsbSpeed::Full));
    assert((m.words[kUsbConfig/4]&(15u<<10))==(9u<<10));
    auto unsupported=geometry; unsupported.high_speed_phy=2;
    const auto unchanged=m.words[kUsbConfig/4];
    assert(!configure_utmi(io,unsupported,false)&&m.words[kUsbConfig/4]==unchanged);
    assert(utmi_turnaround(0x4f54250a,true,MxUsbSpeed::High)==4);
    assert(utmi_turnaround(0x4f54250a,false,MxUsbSpeed::High)==8);
    assert(!decode_geometry(0x55330000, 0, 0, 0, 0, &geometry) && geometry.core_id == 0x4f54310a);
    assert(!decode_geometry(0x4f54310a, 0, 7, 2048u << 16, 0, &geometry));
    const HostFifos layout{{0, 512}, {512, 512}, {1024, 512}};
    assert(valid_host_fifos(layout, 2048));
    assert(!valid_host_fifos({{0, 512}, {500, 512}, {1024, 512}}, 2048));
    assert(!valid_host_fifos(layout, 1200));
    mx_u32_t value = 99;
    assert(port_write(kPortPower | kPortEnabled | kPortChanges, kPortReset, 0, 1u << 1, &value));
    assert(value == (kPortPower | kPortReset | (1u << 1)));
    assert(!port_write(0, kPortEnabled, 0, 0, &value));
    Reset reset;
    assert(reset.begin(io, 10000));
    assert(m.words[kAhbConfig / 4] == 0 && m.words[kInterruptMask / 4] == 0);
    assert(reset.advance(io, 1) == ResetState::WaitingReset);
    m.words[kReset / 4] = kAhbIdle;
    assert(reset.advance(io, 2) == ResetState::Settling);
    assert(reset.advance(io, 1001) == ResetState::Settling);
    assert(reset.advance(io, 1002) == ResetState::WaitingFlush);
    m.words[kReset / 4] = kAhbIdle;
    assert(reset.advance(io, 1003) == ResetState::Ready);
    assert(prepare_host_pio(io, geometry, layout));
    assert(!(m.words[kAhbConfig / 4] & kDmaEnable));
    assert(reset.begin(io, 100) && reset.advance(io, 100) == ResetState::Fault);
}

static void packet_lifetime() {
    Model m;
    auto io = m.registers();
    Geometry geometry{};
    assert(identify(io, &geometry));
    libk::SlotMetadata slots[2]{};
    PacketState states[2]{};
    PacketCold rows[2]{};
    PacketExecutor executor;
    assert(executor.init(io, geometry, {slots, states, rows, 2}));
    mx_u8_t buffer[8];
    libk::fill(libk::as_bytes(buffer), 0xaa);
    const Packet input{libk::as_bytes(buffer), 8, 8, 1, 0x81, MxUsbSpeed::Full,
        MxUsbTransferType::Interrupt, Pid::Data0, false};
    MxUsbToken token = 0;
    assert(executor.submit(input, &token) == MX_OK && token);
    const auto channel = static_cast<mx_u32_t>(token) - 1;
    assert(!executor.release(token) && !executor.result(token));
    m.receive(channel, 3, Pid::Data0);
    m.interrupt(channel, kChannelComplete | kChannelHalted);
    assert(executor.poll());
    assert(executor.result(token)->status == PacketStatus::Success && executor.result(token)->actual == 3);
    assert(buffer[0] == 1 && buffer[2] == 3 && buffer[3] == 0xaa);
    assert(executor.result(token)); // terminal result survives sink backpressure
    assert(executor.release(token) && !executor.result(token) && !executor.release(token));

    MxUsbToken next = 0;
    assert(executor.submit(input, &next) == MX_OK && next != token);
    assert(!executor.cancel(token)); // old generation cannot stop a new packet
    m.words[kPeriodicTxStatus / 4] = 0;
    assert(executor.cancel(next) && executor.needs_queue_wake());
    assert(!executor.release(next) && !executor.result(next));
    m.words[kPeriodicTxStatus / 4] = (8u << 16) | 512;
    (void)executor.poll();
    assert(!executor.needs_queue_wake());
    m.interrupt(static_cast<mx_u32_t>(next) - 1, kChannelHalted);
    assert(executor.poll() && executor.result(next)->status == PacketStatus::Cancelled);
    assert(executor.release(next));

    assert(executor.submit(input, &next) == MX_OK);
    m.receive(static_cast<mx_u32_t>(next) - 1, 12, Pid::Data0);
    m.interrupt(static_cast<mx_u32_t>(next) - 1, kChannelHalted);
    assert(executor.poll() && executor.result(next)->status == PacketStatus::IoError);
    assert(m.rx_at == m.rx_size && executor.release(next));
    m.words[kPeriodicTxStatus / 4] = 0;
    assert(executor.submit(input, &next) == MX_ERR_WOULD_BLOCK);
    m.words[kPeriodicTxStatus / 4] = (8u << 16) | 512;
    m.fail_writes = true;
    next = 0;
    assert(executor.submit(input, &next) == MX_ERR_FAULT && !next && executor.faulted());
}

static void outgoing_and_fifo_views() {
    alignas(8) mx_u32_t registers[2]{0x04030201, 0xaabbccdd};
    libk::Mmio mmio{reinterpret_cast<volatile mx_u8_t*>(registers), sizeof(registers)};
    mx_u8_t destination[6]{0, 0, 0, 0, 0, 0xaa};
    assert(mmio.read_fifo32(0, libk::as_bytes(destination, 5)));
    assert(destination[0] == 1 && destination[3] == 4 && destination[4] == 1 && destination[5] == 0xaa);
    const mx_u8_t source[]{1, 2, 3, 4, 5, 6};
    assert(mmio.write_fifo32(0, libk::as_bytes(source)) && registers[0] == 0x605);
    assert(registers[1] == 0xaabbccdd);
    assert(!mmio.write_fifo32(1, libk::as_bytes(source)) && registers[0] == 0x605);
    assert(!mmio.read_fifo32(0, {nullptr, 4}));

    Model m;
    auto io = m.registers();
    Geometry geometry{};
    assert(identify(io, &geometry));
    libk::SlotMetadata slots[2]{};
    PacketState states[2]{};
    PacketCold rows[2]{};
    PacketExecutor executor;
    assert(executor.init(io, geometry, {slots, states, rows, 2}));
    mx_u8_t setup[]{0x80, 6, 0, 1, 0, 0, 8, 0};
    const Packet packet{libk::as_bytes(setup), 8, 8, 0, 0, MxUsbSpeed::Full,
        MxUsbTransferType::Control, Pid::Setup, false};
    MxUsbToken token = 0;
    assert(executor.submit(packet, &token) == MX_OK && m.tx_size == 8);
    assert(m.tx_bytes[0] == 0x80 && m.tx_bytes[1] == 6);
    const auto channel = static_cast<mx_u32_t>(token) - 1;
    m.interrupt(channel, kChannelStall); // no CHHLTD: buffer must remain owned
    assert(executor.poll() && !executor.result(token) && !executor.release(token));
    m.interrupt(channel, kChannelHalted);
    assert(executor.poll() && executor.result(token)->status == PacketStatus::Stall);
    assert(executor.release(token));
}

static void control_phases_and_retry() {
    Model model;
    auto registers=model.registers(); Geometry geometry{};
    assert(identify(registers,&geometry));
    libk::SlotMetadata slots[2]{}; PacketState states[2]{}; PacketCold records[2]{};
    PacketExecutor packets;
    assert(packets.init(registers,geometry,{slots,states,records,2}));
    mx_u8_t bytes[16]{};
    Endpoint ep{1,1,0,0,8,0,1,0,MxUsbSpeed::Full,MxUsbTransferType::Control,Pid::Data0};
    mochi::usb::PreparedTransfer request{};
    request.payload=libk::as_bytes(bytes); request.request.length=16;
    request.setup={0x80,6,0x100,0,16}; request.direction=MxUsbDirection::In;
    Transfer row{}; start(row,request,ep);
    assert(!advance(row,ep,packets,0,0)&&row.packet);
    assert(model.tx_bytes[0]==0x80&&model.tx_bytes[1]==6&&model.tx_bytes[6]==16);
    auto finish=[&](mx_u32_t actual,Pid pid,mx_u32_t interrupt=kChannelComplete) {
        const auto channel=static_cast<mx_u32_t>(row.packet)-1;
        if(actual) model.receive(channel,actual,pid);
        model.interrupt(channel,interrupt|kChannelHalted); assert(packets.poll());
    };
    finish(0,Pid::Setup);
    assert(!advance(row,ep,packets,1,0)&&row.phase==TransferPhase::Data);
    finish(0,Pid::Data1,kChannelNak);
    assert(!advance(row,ep,packets,2,0)&&!row.packet&&row.toggle==Pid::Data1);
    assert(!advance(row,ep,packets,1000001,0)&&!row.packet);
    assert(!advance(row,ep,packets,1000002,0)&&row.packet);
    finish(8,Pid::Data1);
    assert(!advance(row,ep,packets,1000003,0)&&row.actual==8&&row.toggle==Pid::Data0);
    finish(3,Pid::Data0);
    assert(!advance(row,ep,packets,1000004,0)&&row.actual==11&&row.phase==TransferPhase::Status);
    const auto channel=static_cast<mx_u32_t>(row.packet)-1;
    assert(!(model.words[channel_register(channel,kCharacteristics)/4]&(1u<<15)));
    assert(((model.words[channel_register(channel,kTransferSize)/4]>>29)&3)==static_cast<mx_u32_t>(Pid::Data1));
    finish(0,Pid::Data1);
    assert(advance(row,ep,packets,1000005,0)&&row.phase==TransferPhase::Done&&row.actual==11);
    assert(ep.toggle==Pid::Data0); // control does not disturb non-control toggle
}

int main() {
    control_phases_and_retry();
    geometry_and_reset();
    packet_lifetime();
    outgoing_and_fifo_views();
    std::puts("dwc2 packet probe ok");
}
