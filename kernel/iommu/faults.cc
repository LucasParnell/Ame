// SMMUv3 fault records are bounded, indexed data. Faults never allocate memory
// or grant a translation. Hardware/core operations run outside the statistics
// lock; containment commits only after denial and command completion succeed.
#include "anko/smmuv3.h"
#include "anko/log.h"
#include "smmuv3_internal.h"

namespace iommu {
namespace {
constexpr u32 kStreamSlots = 256; // matches the backend's bounded linear table
constexpr u32 kQuarantineThreshold = 8;
constexpr u32 kLogStride = 16;
enum class StreamState : u8 {
    Unknown = 0, Active = 1, Faulted = 2, BusMasterDisabled = 3,
    Containing = 4, DmaDenied = 5,
};
struct StreamFault {
    u32 count;
    u32 generation;
    u64 last_iova;
    u32 last_type;
    StreamState state;
};
StreamFault g_streams[kStreamSlots];
kernel::SpinLock g_fault_lock;
u64 g_malformed;

bool valid_sid(u32 sid) { return sid < smmuv3::sid_limit() && sid < kStreamSlots; }
}

namespace faults {
void on_stream_attached(u32 sid) {
    if (!valid_sid(sid)) return;
    kernel::LockGuard guard(g_fault_lock);
    auto& rec = g_streams[sid & (kStreamSlots - 1)];
    ++rec.generation;
    rec.count = 0;
    rec.last_iova = 0;
    rec.last_type = 0;
    rec.state = StreamState::Active;
}

void on_event(const smmuv3::Evt& evt) {
    const u32 sid = smmuv3::evt_sid(evt);
    const u32 type = smmuv3::evt_word0_type(evt.w[0]);
    const u64 iova = smmuv3::evt_addr(evt);
    if (!valid_sid(sid)) {
        kernel::LockGuard guard(g_fault_lock);
        if ((++g_malformed % kLogStride) == 1)
            ANKO_LOGF_WARN("iommu/faults", "malformed requester id %u type=%s", sid,
                           smmuv3::evt_type_name(type));
        return;
    }
    const IommuDeviceId device{0, sid};
    note_fault(device, iova, type);
    bool contain = false;
    u32 generation = 0;
    u32 count = 0;
    {
        kernel::LockGuard guard(g_fault_lock);
        auto& rec = g_streams[sid & (kStreamSlots - 1)];
        if (rec.count != ~0u) ++rec.count;
        rec.last_iova = iova;
        rec.last_type = type;
        if (rec.state == StreamState::Unknown || rec.state == StreamState::Active)
            rec.state = StreamState::Faulted;
        if (rec.count == 1 || rec.count % kLogStride == 0)
            ANKO_LOGF_WARN("iommu/faults", "fault: sid=%u iova=0x%llx %s s2=%u class=%u type=%s count=%u",
                sid, static_cast<unsigned long long>(iova), smmuv3::evt_rnw(evt) ? "read" : "write",
                smmuv3::evt_s2(evt) ? 1u : 0u, smmuv3::evt_class(evt), smmuv3::evt_type_name(type), rec.count);
        if (rec.count >= kQuarantineThreshold && rec.state == StreamState::Faulted) {
            contain = true;
            generation = rec.generation;
            count = rec.count;
            rec.state = StreamState::Containing;
        }
    }
    if (!contain) return;
    const bool stopped = smmuv3::clear_pci_bus_master(sid);
    const auto rc = quarantine_stream(device);
    {
        kernel::LockGuard guard(g_fault_lock);
        auto& rec = g_streams[sid & (kStreamSlots - 1)];
        // A new assignment may have installed a context while we returned
        // from containment. Never overwrite that assignment's fault state.
        if (rec.generation != generation) return;
        rec.state = rc != E_OK ? StreamState::Faulted :
            stopped ? StreamState::BusMasterDisabled : StreamState::DmaDenied;
    }
    if (rc == E_OK) {
        note_quarantine();
        ANKO_LOGF_WARN("iommu/faults", "quarantine: sid=%u faults=%u generation=%u deny=1 bus_master_disabled=%u",
            sid, count, generation, stopped ? 1u : 0u);
    } else if (count == kQuarantineThreshold || count % kLogStride == 0) {
        ANKO_LOGF_ERROR("iommu/faults", "containment incomplete: sid=%u rc=%d; binding and pins retained",
            sid, static_cast<int>(rc));
    }
}
} // namespace faults

u32 smmuv3_stream_faults(u32 sid) {
    if (!valid_sid(sid)) return 0;
    kernel::LockGuard guard(g_fault_lock);
    return g_streams[sid & (kStreamSlots - 1)].count;
}
u32 smmuv3_stream_state(u32 sid) {
    if (!valid_sid(sid)) return 0;
    kernel::LockGuard guard(g_fault_lock);
    return static_cast<u32>(g_streams[sid & (kStreamSlots - 1)].state);
}
} // namespace iommu
