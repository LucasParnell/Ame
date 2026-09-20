#pragma once
// kernel/iommu/smmuv3_internal.h — interfaces between the SMMUv3 hardware
// layer (smmuv3.cc) and the fault-containment policy layer (faults.cc).
//
// Not a public interface: only kernel/iommu/*.cc include this.

#include "anko/smmuv3_enc.h"

namespace iommu::smmuv3 {

// True once the SMMU was found and enabled.
bool ready();

// Install the faulting context (empty stage-2 table) for `sid`. Used by the
// quarantine transition and by detach. Returns E_OK on success.
SysError program_deny(u32 sid);

// Clear PCI bus-master enable for the function identified by `sid` (the SMMU
// stream id is the PCI requester id). Returns true when the write was issued.
bool clear_pci_bus_master(u32 sid);

// Number of stream-table entries the backend can address.
u32 sid_limit();

} // namespace iommu::smmuv3

namespace iommu::faults {

// Decode one event-queue record: record the fault and, when warranted, move
// the stream through the ACTIVE -> FAULTED -> BUS_MASTER_DISABLED quarantine
// transition. Called by the event-queue drain with the raw record.
void on_event(const smmuv3::Evt& evt);

} // namespace iommu::faults
