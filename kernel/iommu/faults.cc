// kernel/iommu/faults.cc — SMMUv3 fault decoding and containment (Phase C).
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.10 (fault handling and
// containment), §8.12 (DMA-safe revoke), §8.14 (verification).
//
// IOMMU faults are security events, not demand paging.  An unmapped fault is
// never answered by mapping a page: this file only *records* faults and, when a
// stream misbehaves repeatedly, moves it through
//
//     ACTIVE -> FAULTED -> BUS_MASTER_DISABLED (quarantine)
//
// Quarantine reprograms the stream's STE to the faulting context and clears the
// PCI command register's bus-master bit, so the device can neither translate
// nor keep mastering the bus.  Logging is rate-limited so a broken device
// cannot turn the kernel log into a denial of service.
//
// The event-queue drain itself lives in smmuv3.cc; this file only decodes the
// raw record it hands over, so it stays free of MMIO.

#include "anko/smmuv3.h"

#include "anko/log.h"
#include "anko/spinlock.h"

#include "smmuv3_internal.h"

namespace iommu {

namespace {

using smmuv3::Evt;
using smmuv3::evt_addr;
using smmuv3::evt_class;
using smmuv3::evt_rnw;
using smmuv3::evt_s2;
using smmuv3::evt_sid;
using smmuv3::evt_type_name;
using smmuv3::evt_word0_overflow;
using smmuv3::evt_word0_type;

// Streams we keep fault state for.  A fixed table, keyed by stream id: the
// kernel is data-oriented and the number of requesters is bounded.
constexpr u32 kStreamSlots = 64;
// Repeated faults before a stream is quarantined.
constexpr u32 kQuarantineThreshold = 8;
// Log at most one line per stream per 16 faults, plus the first fault.
constexpr u32 kLogStride = 16;

enum class StreamState : u8 {
    Unknown = 0,
    Active = 1,
    Faulted = 2,
    BusMasterDisabled = 3,
};

struct StreamFault {
    u32 sid;
    u32 count;      // total fault events seen
    u32 generation; // increments each time the stream is quarantined
    u64 last_iova;
    u32 last_type; // last decoded event type
    bool logged;
    u8 state; // StreamState
    u8 pad[2];
};

StreamFault g_streams[kStreamSlots];
kernel::SpinLock g_fault_lock;
u64 g_total_faults;

StreamFault* slot_for(u32 sid, bool create) {
    for (u32 i = 0; i < kStreamSlots; ++i) {
        if (g_streams[i].state != static_cast<u8>(StreamState::Unknown) &&
            g_streams[i].sid == sid) {
            return &g_streams[i];
        }
    }
    if (!create) {
        return nullptr;
    }
    for (u32 i = 0; i < kStreamSlots; ++i) {
        if (g_streams[i].state == static_cast<u8>(StreamState::Unknown)) {
            g_streams[i].sid = sid;
            g_streams[i].count = 0;
            g_streams[i].generation = 1;
            g_streams[i].last_iova = 0;
            g_streams[i].last_type = 0;
            g_streams[i].logged = false;
            g_streams[i].state = static_cast<u8>(StreamState::Active);
            return &g_streams[i];
        }
    }
    return nullptr; // table full: keep faulting without per-stream state
}

// Move a stream to quarantine: faulting STE + no bus mastering.  Called with
// g_fault_lock held.
void quarantine_stream(StreamFault& rec) {
    const IommuDeviceId device{0, rec.sid};
    smmuv3::program_deny(rec.sid);
    smmuv3::clear_pci_bus_master(rec.sid);
    rec.state = static_cast<u8>(StreamState::BusMasterDisabled);
    ++rec.generation;
    note_quarantine();
    // Stop accepting new mappings on the domain this stream belonged to, and
    // migrate the core binding so a later attach recovers the stream instead
    // of reporting E_BUSY against a domain that no longer trusts it.
    if (IommuDomain* domain = stream_domain(device)) {
        domain->state_ = DomainState::Quarantined;
    }
    note_stream_quarantined(device);
    ANKO_LOGF_WARN("iommu/faults",
                   "quarantine: sid=%u faults=%u generation=%u (STE faulting, bus-master cleared)",
                   rec.sid, rec.count, rec.generation);
}

} // namespace

namespace faults {

void on_event(const Evt& evt) {
    const u32 type = evt_word0_type(evt.w[0]);
    const u32 sid = evt_sid(evt);
    const u64 iova = evt_addr(evt);
    const bool is_write = evt_rnw(evt);
    const bool stage2 = evt_s2(evt);
    const u32 cls = evt_class(evt);
    const bool overflow = evt_word0_overflow(evt.w[0]);

    // Untrusted stream ids are validated before they index any table.
    if (sid >= smmuv3::sid_limit()) {
        kernel::LockGuard guard(g_fault_lock);
        ++g_total_faults;
        // A malformed requester id is itself a containment-worthy event, but
        // there is no stream slot to quarantine; rate-limit and drop.
        if ((g_total_faults % kLogStride) == 1) {
            ANKO_LOGF_WARN("iommu/faults", "malformed requester id %u type=%s", sid,
                           evt_type_name(type));
        }
        return;
    }

    // Record the fault with the generic core (increments IOMMU_FAULTS and
    // stores the last-fault record) — never by mapping a page.
    note_fault(IommuDeviceId{0, sid}, iova, type);

    kernel::LockGuard guard(g_fault_lock);
    ++g_total_faults;
    if (overflow) {
        ANKO_LOGF_ERROR("iommu/faults", "event queue overflow marker observed for sid=%u", sid);
    }

    StreamFault* rec = slot_for(sid, true);
    if (!rec) {
        ANKO_LOGF_WARN("iommu/faults", "fault table full; sid=%u dropped from tracking", sid);
        return;
    }

    ++rec->count;
    rec->last_iova = iova;
    rec->last_type = type;
    if (rec->state == static_cast<u8>(StreamState::Active)) {
        rec->state = static_cast<u8>(StreamState::Faulted);
    }

    if (!rec->logged || (rec->count % kLogStride) == 0) {
        ANKO_LOGF_WARN("iommu/faults",
                       "fault: sid=%u iova=0x%llx %s s2=%u class=%u type=%s count=%u", sid,
                       static_cast<unsigned long long>(iova), is_write ? "write" : "read",
                       stage2 ? 1u : 0u, cls, evt_type_name(type), rec->count);
        rec->logged = true;
    }

    if (rec->count >= kQuarantineThreshold &&
        rec->state != static_cast<u8>(StreamState::BusMasterDisabled)) {
        quarantine_stream(*rec);
    }
}

} // namespace faults

u32 smmuv3_stream_faults(u32 sid) {
    kernel::LockGuard guard(g_fault_lock);
    const StreamFault* rec = slot_for(sid, false);
    return rec ? rec->count : 0;
}

u32 smmuv3_stream_state(u32 sid) {
    kernel::LockGuard guard(g_fault_lock);
    const StreamFault* rec = slot_for(sid, false);
    return rec ? rec->state : 0;
}

} // namespace iommu
