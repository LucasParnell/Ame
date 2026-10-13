#pragma once
// PIO host-packet executor. USB transfer phases, retries, addressing and class
// policy sit above this layer. A logical endpoint is not a hardware channel.
// A row and its CPU buffer remain owned until CHHLTD AND RX drain, and then
// until the caller publishes/consumes the terminal result and calls release().
#include "mochi/usb/dwc2_core.h"
#include "midix/abi/status.h"
#include "libk/slot_pool.h"

namespace mochi::usb::dwc2 {

enum class PacketState : mx_u8_t { Free, Active, Halting, Terminal };
enum class PacketStatus : mx_u8_t {
    Success, Nak, Nyet, Stall, TransactionError, Babble, ToggleError,
    FrameOverrun, Cancelled, IoError,
};
struct Packet {
    libk::bytes buffer;       // caller-owned CPU storage, never DMA memory
    mx_u16_t length;          // requested bytes, at most one maximum packet
    mx_u16_t max_packet;
    mx_u8_t device_address;
    mx_u8_t endpoint_address;
    MxUsbSpeed speed;
    MxUsbTransferType type;
    Pid pid;
    bool odd_frame;
};
struct PacketResult { PacketStatus status; mx_u16_t actual; mx_u32_t raw_status; };
struct PacketCold {
    Packet packet;
    PacketResult result;
    mx_u32_t characteristics;
    bool halt_requested;
};
struct PacketStorage {
    libk::SlotMetadata* slots;
    PacketState* states;
    PacketCold* records;
    mx_u32_t capacity;
};

class PacketExecutor {
public:
    PacketExecutor() = default;
    PacketExecutor(const PacketExecutor&) = delete;
    PacketExecutor& operator=(const PacketExecutor&) = delete;

    bool init(const Registers& registers, const Geometry& geometry, PacketStorage storage) {
        if (ready_ || !registers.valid() || !geometry.host || !storage.slots ||
            !storage.states || !storage.records || storage.capacity < geometry.host_channels ||
            !geometry.host_channels || geometry.host_channels > 16) return false;
        mx_u32_t mode = 0, ahb = 0;
        if (!registers.read(registers.context, kInterruptStatus, &mode) || !(mode & kHostMode) ||
            !registers.read(registers.context, kAhbConfig, &ahb) || (ahb & kDmaEnable) ||
            !pool_.init(storage.slots, geometry.host_channels)) return false;
        registers_ = registers;
        storage_ = storage;
        channels_ = geometry.host_channels;
        for (mx_u32_t i = 0; i < channels_; ++i) {
            storage_.states[i] = PacketState::Free;
            libk::zero(storage_.records[i]);
        }
        ready_ = true;
        return true;
    }

    // Admission checks FIFO and request-queue space before any packet becomes
    // visible to hardware. Queue pressure is explicit; no busy wait or allocation.
    mx_status_t submit(const Packet& packet, MxUsbToken* out) {
        if (!ready_ || fault_ || !out || !valid(packet)) return MX_ERR_INVALID;
        const auto periodic = packet.type == MxUsbTransferType::Interrupt;
        mx_u32_t queue = 0;
        if (!read(periodic ? kPeriodicTxStatus : kNonperiodicTxStatus, &queue)) return MX_ERR_FAULT;
        const mx_u32_t words = (packet.length + 3u) / 4;
        const bool input = (packet.endpoint_address & 0x80u) != 0;
        if (!((queue >> 16) & 0xffu) || (!input && (queue & 0xffffu) < words))
            return MX_ERR_WOULD_BLOCK;
        const auto token = pool_.acquire();
        if (!token) return MX_ERR_WOULD_BLOCK;
        const auto channel = pool_.index_of(token);
        auto& record = storage_.records[channel];
        libk::zero(record);
        record.packet = packet;
        record.result.status = PacketStatus::Success;
        record.characteristics = packet.max_packet |
            (static_cast<mx_u32_t>(packet.endpoint_address & 15u) << 11) |
            (input ? (1u << 15) : 0) | (packet.speed == MxUsbSpeed::Low ? (1u << 17) : 0) |
            (static_cast<mx_u32_t>(packet.type) << 18) | (1u << 20) |
            (static_cast<mx_u32_t>(packet.device_address) << 22) |
            (periodic && packet.odd_frame ? (1u << 29) : 0);
        // IN transfers request one whole USB packet even for a shorter tail.
        // The receive path rejects bytes beyond the caller's requested length.
        const mx_u32_t size = input ? packet.max_packet : packet.length;
        const mx_u32_t transfer = size | (1u << 19) | (static_cast<mx_u32_t>(packet.pid) << 29);
        storage_.states[channel] = PacketState::Active;
        if (!write(channel_register(channel, kChannelMask), 0) ||
            !write(channel_register(channel, kChannelStatus), kChannelOwnedInterrupts) ||
            !write(channel_register(channel, kSplit), 0) ||
            !write(channel_register(channel, kTransferSize), transfer) ||
            !write(channel_register(channel, kChannelMask), kChannelOwnedInterrupts)) return failed_submit(token);
        mx_u32_t mask = 0;
        if (!read(kHostInterruptMask, &mask) || !write(kHostInterruptMask, mask | (1u << channel)))
            return failed_submit(token);
        libk::dma::order_mmio();
        if (!write(channel_register(channel, kCharacteristics), record.characteristics | kChannelEnable))
            return failed_submit(token);
        if (!input && packet.length && !registers_.transmit(registers_.context, fifo_register(channel),
            libk::slice(packet.buffer, 0, packet.length))) return failed_submit(token);
        libk::dma::order_mmio();
        *out = token;
        return MX_OK;
    }

    bool cancel(MxUsbToken token) {
        const auto index = pool_.index_of(token);
        if (index == libk::kNoSlot || fault_) return false;
        if (storage_.states[index] == PacketState::Terminal) return true;
        auto& record = storage_.records[index];
        record.result.status = PacketStatus::Cancelled;
        storage_.states[index] = PacketState::Halting;
        return request_halt(index);
    }

    // Drain RX before channel completion. A packet can be in the shared RX FIFO
    // after its channel reports CHHLTD; publishing first would let the buffer
    // or channel be reused while old packet data still names that channel.
    bool poll(mx_u32_t receive_budget = 64) {
        if (!ready_ || fault_ || !receive_budget) return false;
        bool progress = false;
        mx_u32_t global = 0;
        for (mx_u32_t n = 0; n < receive_budget; ++n) {
            if (!read(kInterruptStatus, &global)) return false;
            if (!(global & kRxAvailable)) break;
            if (!receive_packet()) return false;
            progress = true;
        }
        if (!read(kInterruptStatus, &global) || (global & kRxAvailable)) return progress;
        for (mx_u32_t channel = 0; channel < channels_; ++channel) {
            const auto state = storage_.states[channel];
            if (state != PacketState::Active && state != PacketState::Halting) continue;
            mx_u32_t interrupt = 0;
            if (!read(channel_register(channel, kChannelStatus), &interrupt)) return false;
            interrupt &= kChannelOwnedInterrupts;
            if (interrupt) {
                progress = true;
                auto& record = storage_.records[channel];
                record.result.raw_status |= interrupt;
                if (state != PacketState::Halting) record.result.status = status(interrupt);
                if (!write(channel_register(channel, kChannelStatus), interrupt)) return false;
                if (interrupt & kChannelHalted) {
                    // CHHLTD without a cause is not successful completion.
                    if (!(record.result.raw_status & kChannelComplete) &&
                        record.result.status == PacketStatus::Success) record.result.status = PacketStatus::IoError;
                    if ((record.result.raw_status & kChannelComplete) &&
                        record.result.status == PacketStatus::Success && !(record.packet.endpoint_address & 128))
                        record.result.actual = record.packet.length;
                    if (!write(channel_register(channel, kChannelMask), 0)) return false;
                    storage_.states[channel] = PacketState::Terminal;
                    continue;
                }
                if (interrupt & ~kChannelAck) storage_.states[channel] = PacketState::Halting;
            }
            if (storage_.states[channel] == PacketState::Halting) (void)request_halt(channel);
        }
        return progress;
    }

    const PacketResult* result(MxUsbToken token) const {
        const auto index = pool_.index_of(token);
        return index != libk::kNoSlot && storage_.states[index] == PacketState::Terminal ?
               &storage_.records[index].result : nullptr;
    }
    bool release(MxUsbToken token) {
        const auto index = pool_.index_of(token);
        if (index == libk::kNoSlot || storage_.states[index] != PacketState::Terminal) return false;
        // The caller has published the result, or fenced a dead client.
        storage_.states[index] = PacketState::Free;
        libk::zero(storage_.records[index]);
        return pool_.release(token);
    }
    // Caller must have stopped/reset the controller before discarding rows.
    // Used only for role teardown; no caller buffer may still be borrowed.
    void fenced() { ready_=false; fault_=false; channels_=0; }
    bool faulted() const { return fault_; }
    bool needs_queue_wake() const {
        for (mx_u32_t i = 0; i < channels_; ++i)
            if (storage_.states[i] == PacketState::Halting && !storage_.records[i].halt_requested) return true;
        return false;
    }

private:
    static bool valid(const Packet& p) {
        if (p.device_address > 127 || (p.endpoint_address & 0x70u) || !p.max_packet ||
            p.length > p.max_packet || p.max_packet > 1024 || p.pid > Pid::Setup || p.pid == Pid::Data2 ||
            p.type == MxUsbTransferType::Isochronous || p.type > MxUsbTransferType::Interrupt ||
            p.speed < MxUsbSpeed::Low || p.speed > MxUsbSpeed::High ||
            (p.length && (!p.buffer.data || p.buffer.size < p.length))) return false;
        if (p.type == MxUsbTransferType::Control) {
            if ((p.endpoint_address & 15u) || (p.max_packet != 8 && p.max_packet != 16 &&
                p.max_packet != 32 && p.max_packet != 64)) return false;
        } else {
            if (!(p.endpoint_address & 15u) || p.pid == Pid::Setup || p.pid == Pid::Data2) return false;
        }
        if (p.speed == MxUsbSpeed::Low && (p.max_packet > 8 || p.type == MxUsbTransferType::Bulk)) return false;
        if (p.speed == MxUsbSpeed::Full && p.max_packet > 64) return false;
        if (p.speed == MxUsbSpeed::High && p.type == MxUsbTransferType::Control && p.max_packet != 64) return false;
        if (p.type == MxUsbTransferType::Bulk && (p.speed == MxUsbSpeed::High ? p.max_packet != 512 :
            p.max_packet != 8 && p.max_packet != 16 && p.max_packet != 32 && p.max_packet != 64)) return false;
        return p.pid != Pid::Setup || (p.type == MxUsbTransferType::Control &&
               !(p.endpoint_address & 128) && p.length == 8);
    }
    static PacketStatus status(mx_u32_t interrupt) {
        if (interrupt & kChannelAhbError) return PacketStatus::IoError;
        if (interrupt & kChannelStall) return PacketStatus::Stall;
        if (interrupt & kChannelBabble) return PacketStatus::Babble;
        if (interrupt & kChannelToggleError) return PacketStatus::ToggleError;
        if (interrupt & kChannelTransactionError) return PacketStatus::TransactionError;
        if (interrupt & kChannelFrameOverrun) return PacketStatus::FrameOverrun;
        if (interrupt & kChannelComplete) return PacketStatus::Success;
        if (interrupt & kChannelNyet) return PacketStatus::Nyet;
        if (interrupt & kChannelNak) return PacketStatus::Nak;
        return PacketStatus::Success;
    }
    bool read(mx_u32_t reg, mx_u32_t* value) {
        if (!registers_.read(registers_.context, reg, value)) fault_ = true;
        return !fault_;
    }
    bool write(mx_u32_t reg, mx_u32_t value) {
        if (!registers_.write(registers_.context, reg, value)) fault_ = true;
        return !fault_;
    }
    mx_status_t failed_submit(MxUsbToken) {
        // A register failure after activation is uncertain execution. Keep the
        // row and buffer quarantined; the platform must fence/reset the core.
        fault_ = true;
        return MX_ERR_FAULT;
    }
    bool request_halt(mx_u32_t channel) {
        auto& record = storage_.records[channel];
        if (record.halt_requested) return true;
        mx_u32_t queue = 0;
        if (!read(record.packet.type == MxUsbTransferType::Interrupt ? kPeriodicTxStatus : kNonperiodicTxStatus,
                  &queue)) return false;
        if (!((queue >> 16) & 0xffu)) return true; // retain row; request a queue-space wake
        if (!write(channel_register(channel, kCharacteristics), record.characteristics |
                   kChannelDisable | kChannelEnable)) return false;
        libk::dma::order_mmio();
        record.halt_requested = true;
        return true;
    }
    bool discard(mx_u32_t bytes) {
        while (bytes) {
            mx_u8_t word[4]{};
            const auto n = bytes < 4 ? bytes : 4;
            if (!registers_.receive(registers_.context, fifo_register(0), libk::as_bytes(word, n))) {
                fault_ = true;
                return false;
            }
            bytes -= n;
        }
        return true;
    }
    bool receive_packet() {
        mx_u32_t receive = 0;
        if (!read(kRxStatusPop, &receive)) return false;
        const auto channel = receive & 15u, bytes = (receive >> 4) & 0x7ffu;
        const auto packet_status = (receive >> 17) & 15u;
        if (packet_status != 2) return bytes ? discard(bytes) : true;
        if (channel >= channels_ || storage_.states[channel] != PacketState::Active ||
            !(storage_.records[channel].packet.endpoint_address & 128)) return discard(bytes);
        auto& record = storage_.records[channel];
        const auto actual = record.result.actual;
        const auto& packet = record.packet;
        if (bytes > static_cast<mx_u32_t>(packet.length) - actual || bytes > packet.buffer.size - actual ||
            ((receive >> 15) & 3u) != static_cast<mx_u32_t>(packet.pid)) {
            if (!discard(bytes)) return false;
            record.result.status = PacketStatus::IoError;
            storage_.states[channel] = PacketState::Halting;
            return request_halt(channel);
        }
        if (bytes && !registers_.receive(registers_.context, fifo_register(0),
            libk::slice(packet.buffer, actual, bytes))) { fault_ = true; return false; }
        record.result.actual = static_cast<mx_u16_t>(actual + bytes);
        return true;
    }
    Registers registers_{};
    PacketStorage storage_{};
    libk::SlotPool pool_;
    mx_u32_t channels_ = 0;
    bool ready_ = false, fault_ = false;
};
} // namespace mochi::usb::dwc2
