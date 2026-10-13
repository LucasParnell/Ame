#pragma once
// mochi/usb/xhci_exec.h — route xHCI transfer events into TransferEngine results.
//
// The controller records each submitted transfer (an engine token) together
// with the TRB whose completion terminates it. Incoming transfer events are
// matched to records; a short-packet data stage accumulates the actual length
// and keeps a control transfer live until its status stage; terminal events
// publish the result through the engine's finish(). Bounded, CPU-only, and
// host-testable: the HCD still owns the rings and the doorbells.
#include "midix/usb/xhci_ring.h"
#include "mochi/usb/transfer_engine.h"

namespace mochi::usb {

namespace xhci = midix::usb::xhci;

constexpr mx_u32_t kXhciCcSuccess = 1;
constexpr mx_u32_t kXhciCcStall = 6;
constexpr mx_u32_t kXhciCcShortPacket = 13;
constexpr mx_u32_t kXhciEventTransfer = 32;
constexpr mx_u32_t kXhciEventPortStatusChange = 34;

struct ExecEntry {
    MxUsbToken token = 0;                            // engine transfer token; 0 = free
    mx_u64_t request_id = 0;
    xhci::ProducerRing* ring = nullptr;              // ring holding the descriptors
    mx_u64_t head_phys = 0;                          // first TRB: a stall here is terminal
    mx_u64_t status_phys = 0;                        // TRB whose completion terminates
    mx_u64_t data_phys = 0;                          // control data stage TRB, or 0
    mx_u32_t requested = 0;
    mx_u32_t actual = 0;
    mx_u32_t descriptors = 0;                        // ring descriptors occupied
    mx_u8_t slot_id = 0;
    mx_u8_t endpoint_id = 0;
    mx_u8_t flags = 0;
    mx_u8_t reserved = 0;
};
constexpr mx_u8_t kExecSawShort = 1u << 0;   // a short data stage was observed
constexpr mx_u8_t kExecDone = 1u << 1;       // finish() published; awaiting pickup
constexpr mx_u8_t kExecInvalid = 1u << 2;    // finish() refused: treat as a fault
constexpr mx_u8_t kExecOrphan = 1u << 3;     // claimant gave up; reclaim on completion
constexpr mx_u8_t kExecRingRetired = 1u << 4; // ring descriptors already retired
constexpr mx_u8_t kExecNotified = 1u << 5;   // caller saw this terminal result once

class ExecTable {
public:
    ExecTable() = default;
    ExecTable(const ExecTable&) = delete;
    ExecTable& operator=(const ExecTable&) = delete;

    bool init(ExecEntry* storage, mx_u32_t capacity) {
        if (!storage || !capacity) return false;
        storage_ = storage;
        capacity_ = capacity;
        // The caller's storage may be freshly bump-allocated, not zeroed; a
        // non-zero token field would read as a live record.
        for (mx_u32_t i = 0; i < capacity_; ++i) storage_[i] = ExecEntry{};
        return true;
    }

    mx_u32_t capacity() const { return capacity_; }
    ExecEntry* at(mx_u32_t index) { return index < capacity_ ? storage_ + index : nullptr; }
    const ExecEntry* at(mx_u32_t index) const { return index < capacity_ ? storage_ + index : nullptr; }
    ExecEntry* find(MxUsbToken token) {
        if (!token) return nullptr;
        for (mx_u32_t i = 0; i < capacity_; ++i)
            if (storage_[i].token == token) return storage_ + i;
        return nullptr;
    }

    mx_status_t record(MxUsbToken token, mx_u64_t request_id, mx_u8_t slot_id,
                       mx_u8_t endpoint_id, xhci::ProducerRing* ring, mx_u32_t descriptors,
                       mx_u64_t head_phys, mx_u64_t status_phys, mx_u64_t data_phys,
                       mx_u32_t requested) {
        // `requested` may be zero: a control transfer with no data stage still
        // terminates through its status stage.
        if (!token || !ring || !descriptors || !head_phys || !status_phys)
            return MX_ERR_INVALID;
        if (find(token)) return MX_ERR_BUSY;
        for (mx_u32_t i = 0; i < capacity_; ++i) {
            if (storage_[i].token) continue;
            storage_[i] = {token, request_id, ring, head_phys, status_phys, data_phys,
                           requested, 0, descriptors, slot_id, endpoint_id, 0, 0};
            return MX_OK;
        }
        return MX_ERR_OVERFLOW;
    }

    // Route one event TRB. Returns true when it terminated (or advanced) a
    // recorded transfer. When the terminal event arrives, the engine's
    // finish() publishes the completion unless the transfer already stopped.
    bool on_event(const xhci::Trb& event, TransferEngine& engine) {
        const mx_u32_t type = (event.control >> 10) & 0x3fu;
        if (type != kXhciEventTransfer) return false;
        const mx_u32_t code = (event.status >> 24) & 0xffu;
        const mx_u8_t slot = static_cast<mx_u8_t>((event.control >> 24) & 0xffu);
        const mx_u8_t endpoint = static_cast<mx_u8_t>((event.control >> 16) & 0x1fu);
        for (mx_u32_t i = 0; i < capacity_; ++i) {
            ExecEntry& entry = storage_[i];
            if (!entry.token || (entry.flags & kExecDone)) continue;
            if (entry.slot_id != slot || entry.endpoint_id != endpoint) continue;
            // Any TRB of the transfer can carry the terminal event: a stall is
            // reported against the stage that failed (often the setup stage),
            // while success and short packets arrive on the data/status stage.
            if (event.parameter != entry.head_phys && event.parameter != entry.status_phys &&
                (!entry.data_phys || event.parameter != entry.data_phys)) continue;
            const mx_u32_t residual = event.status & 0xffffffu;
            if (event.parameter == entry.data_phys && code == kXhciCcShortPacket) {
                // The status stage still owns the descriptors and the DMA.
                entry.actual = residual <= entry.requested ? entry.requested - residual : 0;
                entry.flags |= kExecSawShort;
                return true;
            }
            mx_u32_t actual = entry.requested;
            if (entry.flags & kExecSawShort) actual = entry.actual;
            else if (residual <= entry.requested)
                actual = entry.requested - residual;
            const MxUsbStatus status = code == kXhciCcSuccess || code == kXhciCcShortPacket
                ? MxUsbStatus::Success
                : code == kXhciCcStall ? MxUsbStatus::Stall : MxUsbStatus::IoError;
            if (engine.finish(entry.token, status, actual, code) != MX_OK)
                entry.flags |= kExecInvalid;
            entry.flags |= kExecDone;
            return true;
        }
        return false;
    }

    // Reclaim a terminal record: retire the ring descriptors (hardware proof
    // already received), release the engine token, free the entry. The caller
    // must have published the completion first. Idempotent across ring-retire
    // being already done by an earlier partial attempt.
    mx_status_t reclaim(ExecEntry& entry, TransferEngine& engine) {
        if (!entry.token) return MX_ERR_INVALID;
        if (!(entry.flags & kExecDone)) return MX_ERR_BUSY;
        if (!(entry.flags & kExecRingRetired)) {
            if (!entry.ring || !entry.descriptors || !entry.ring->retire(entry.descriptors))
                return MX_ERR_INVALID;
            entry.flags |= kExecRingRetired;
        }
        if (engine.retire(entry.token) != MX_OK) return MX_ERR_BUSY;
        entry = ExecEntry{};
        return MX_OK;
    }

private:
    ExecEntry* storage_ = nullptr;
    mx_u32_t capacity_ = 0;
};

} // namespace mochi::usb
