#pragma once
// Transfer phases above the one-packet executor. One endpoint's DATA toggle
// advances only on ACKed packets. NAK/NYET preserve it; control data/status
// always start at DATA1. No enumeration, HID, board or input policy lives here.
#include "mochi/usb/dwc2_packet.h"
#include "mochi/usb/transfer_engine.h"
namespace mochi::usb::dwc2 {
enum class TransferPhase : mx_u8_t { Setup, Data, Status, Zlp, Done };
struct Endpoint {
    MxUsbToken token, device, active;
    mx_u64_t next;
    mx_u16_t max_packet;
    mx_u8_t address, device_address, interval;
    MxUsbSpeed speed;
    MxUsbTransferType type;
    Pid toggle;
};
struct Transfer {
    PreparedTransfer prepared;
    MxUsbToken packet;
    mx_u64_t retry;
    mx_u32_t actual, raw, errors;
    mx_u16_t packet_length;
    mx_u8_t setup[8];
    TransferPhase phase;
    Pid toggle;
    MxUsbStatus result;
    bool stopping, short_packet;
};
inline mx_u64_t interval_ns(const Endpoint& ep) {
    if (ep.type != MxUsbTransferType::Interrupt) {
        return 1000000;
    }
    return ep.speed == MxUsbSpeed::High ? (125000ull << (ep.interval - 1))
                                        : mx_u64_t{ep.interval} * 1000000;
}
inline void start(Transfer& row, const PreparedTransfer& prepared, Endpoint& ep) {
    libk::zero(row);
    row.prepared = prepared;
    row.result = MxUsbStatus::Success;
    row.phase = ep.type == MxUsbTransferType::Control ? TransferPhase::Setup : TransferPhase::Data;
    row.toggle = ep.type == MxUsbTransferType::Control ? Pid::Data1 : ep.toggle;
    libk::write_le<mx_u8_t>(libk::slice(libk::as_bytes(row.setup), 0, 1),
                            prepared.setup.request_type);
    row.setup[1] = prepared.setup.request;
    libk::write_le<mx_u16_t>(libk::slice(libk::as_bytes(row.setup), 2, 2), prepared.setup.value);
    libk::write_le<mx_u16_t>(libk::slice(libk::as_bytes(row.setup), 4, 2), prepared.setup.index);
    libk::write_le<mx_u16_t>(libk::slice(libk::as_bytes(row.setup), 6, 2), prepared.setup.length);
    row.retry = ep.next;
}
inline void stop(Transfer& row, PacketExecutor& packets, MxUsbStatus why) {
    row.stopping = true;
    row.result = why;
    if (row.packet) {
        (void)packets.cancel(row.packet);
    } else {
        row.phase = TransferPhase::Done;
    }
}
inline bool advance(Transfer& row, Endpoint& ep, PacketExecutor& packets, mx_u64_t now,
                    mx_u32_t frame) {
    if (row.phase == TransferPhase::Done) {
        return true;
    }
    if (row.packet) {
        const auto* completion = packets.result(row.packet);
        if (!completion) {
            return false;
        }
        const auto result = *completion;
        (void)packets.release(row.packet);
        row.packet = 0;
        row.raw |= result.raw_status;
        if (row.stopping) {
            row.phase = TransferPhase::Done;
            return true;
        }
        if (result.status == PacketStatus::Nak || result.status == PacketStatus::Nyet ||
            result.status == PacketStatus::FrameOverrun ||
            result.status == PacketStatus::TransactionError) {
            if (result.status == PacketStatus::TransactionError && ++row.errors >= 3) {
                row.result = MxUsbStatus::IoError;
                row.phase = TransferPhase::Done;
                return true;
            }
            row.retry = now + interval_ns(ep);
            return false;
        }
        if (result.status != PacketStatus::Success) {
            row.result =
                result.status == PacketStatus::Stall ? MxUsbStatus::Stall : MxUsbStatus::IoError;
            row.phase = TransferPhase::Done;
            return true;
        }
        row.errors = 0;
        if (row.phase == TransferPhase::Setup) {
            row.phase = row.prepared.request.length ? TransferPhase::Data : TransferPhase::Status;
        } else if (row.phase == TransferPhase::Status || row.phase == TransferPhase::Zlp) {
            row.phase = TransferPhase::Done;
        } else {
            row.actual += result.actual;
            row.short_packet = result.actual < row.packet_length;
            row.toggle = row.toggle == Pid::Data0 ? Pid::Data1 : Pid::Data0;
            if (ep.type != MxUsbTransferType::Control) {
                ep.toggle = row.toggle;
            }
            if (row.short_packet || row.actual == row.prepared.request.length) {
                if (ep.type == MxUsbTransferType::Control) {
                    row.phase = TransferPhase::Status;
                } else if (!row.short_packet && row.prepared.direction == MxUsbDirection::Out &&
                           (row.prepared.request.flags & MX_USB_TRANSFER_SEND_ZLP) && row.actual &&
                           !(row.actual % ep.max_packet)) {
                    row.phase = TransferPhase::Zlp;
                } else {
                    row.phase = TransferPhase::Done;
                }
            }
            ep.next = now + interval_ns(ep);
        }
        if (row.phase == TransferPhase::Done) {
            return true;
        }
    }
    if (now < row.retry) {
        return false;
    }
    const bool setup = row.phase == TransferPhase::Setup,
               status = row.phase == TransferPhase::Status;
    const bool input = status ? (row.prepared.direction != MxUsbDirection::In)
                              : !setup && row.prepared.direction == MxUsbDirection::In;
    const auto remaining = row.prepared.request.length - row.actual;
    const auto length = setup                                       ? 8u
                        : status || row.phase == TransferPhase::Zlp ? 0u
                        : remaining < ep.max_packet                 ? remaining
                                                                    : ep.max_packet;
    row.packet_length = static_cast<mx_u16_t>(length);
    auto bytes =
        setup ? libk::as_bytes(row.setup) : libk::slice(row.prepared.payload, row.actual, length);
    const Packet packet{bytes,
                        static_cast<mx_u16_t>(length),
                        ep.max_packet,
                        ep.device_address,
                        static_cast<mx_u8_t>((ep.address & 15) | (input ? 128 : 0)),
                        ep.speed,
                        ep.type,
                        setup    ? Pid::Setup
                        : status ? Pid::Data1
                                 : row.toggle,
                        ((frame + 1) & 1) != 0};
    const auto rc = packets.submit(packet, &row.packet);
    if (rc != MX_OK && rc != MX_ERR_WOULD_BLOCK) {
        row.result = MxUsbStatus::IoError;
        row.phase = TransferPhase::Done;
        return true;
    }
    return false;
}
} // namespace mochi::usb::dwc2
