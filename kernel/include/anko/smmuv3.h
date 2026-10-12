#pragma once
// anko/smmuv3.h — ARM SMMUv3 backend for the generic IOMMU core.
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.7 (SMMUv3 QEMU backend),
// §8.10 (fault handling and containment), §8.12 (DMA-safe revoke), §8.14.
//
// kernel/iommu/smmuv3.cc implements the backend hooks the generic core calls
// (publish/flush, scoped invalidation, command-stream sync) plus the two
// stream-programming hooks core.cc now invokes from attach()/detach().
// kernel/iommu/faults.cc drains and decodes the event queue.
//
// Layering:
//   smmuv3_enc.h  pure register/record encoders (host-testable, no MMIO)
//   smmuv3.cc     MMIO, queues, stream table, backend hooks
//   faults.cc     event decode, rate-limited records, quarantine state machine
//   selftest.cc   opt-in hostile-DMA verification against QEMU iommu-testdev
//
// There is no identity map and no bypass STE: an unbound, detached or invalid
// stream faults.

#include "anko/iommu.h"
#include "anko/smmuv3_enc.h"

namespace iommu {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Discover the SMMUv3 recorded in g_hw, program it into a default-deny state
// (empty stream table, GBPA abort, SMMUEN|CMDQEN|EVENTQEN) and install the
// backend into the generic core. Idempotent. When no SMMU is present this is a
// successful no-op that leaves the core on the null backend, so off-target
// boots and the default verification topology are unaffected.
SysError smmuv3_bring_up();

// True once the SMMU was found and successfully enabled.
bool smmuv3_ready();

// The backend hook table (attach/detach + the three cache/queue hooks).
const IommuBackendOps& smmuv3_backend();

// ---------------------------------------------------------------------------
// Stream programming
// ---------------------------------------------------------------------------

// Install a stage-2 translation context for `device` pointing at `domain`'s
// page table. A domain with no page table (the permanent quarantine domain) or
// a null pointer installs the faulting context instead. Publishes the STE,
// issues CFGI_STE for the stream, and synchronises the command queue.
SysError smmuv3_program_stream(IommuDeviceId device, IommuDomain* domain);

// Install the faulting context for `device`: an STE whose stage-2 root table is
// empty, so every translation misses and is recorded as a fault. This is the
// "switch to the faulting domain" transition from §8.12, not a bypass.
SysError smmuv3_program_deny(IommuDeviceId device);

// ---------------------------------------------------------------------------
// Fault handling (§8.10)
// ---------------------------------------------------------------------------

// Drain and decode the event queue: record each fault, rate-limit the log, and
// drive repeated/malformed streams into quarantine. Returns the number of
// events consumed. Safe to call at any time, from any context that does not
// hold the SMMU's internal lock.
u32 smmuv3_drain_events();

// Per-stream fault accounting, for diagnostics and tests.
u32 smmuv3_stream_faults(u32 sid);
// 0 unknown, 1 active, 2 faulted, 3 bus-master disabled (quarantined).
u32 smmuv3_stream_state(u32 sid);

// Total fault events drained from the event queue since bring-up.
u64 smmuv3_events_drained();

// ---------------------------------------------------------------------------
// Register access (diagnostics + the selftest)
// ---------------------------------------------------------------------------
PhysAddr smmuv3_base();
u32 smmuv3_gerror();

// Copy the raw stream-table entry for `sid` into `out` (16 words).  Used by the
// selftest and by failure diagnosis; returns false if the SMMU is not ready or
// the stream id is out of range.
bool smmuv3_debug_ste(u32 sid, u32 out[16]);

// ---------------------------------------------------------------------------
// Native-device DMA registration
//
// Native (non-assigned) drivers such as drv.usb.xhci own a pinned kernel DMA
// arena and present physical addresses to their device. Once the SMMU enforces
// translation those devices need a translation context too, or their DMA
// faults. register_native_dma_arena() maps a wired arena into a dedicated
// native domain with IOVA == PA and attaches `device`, which is exactly the
// "wired arena registered at the driver's stream ID" model. It is a scoped
// mapping of kernel-owned pages, never an identity map of RAM.
// ---------------------------------------------------------------------------
// The registrar itself is backend-independent and declared in anko/iommu.h.

// ---------------------------------------------------------------------------
// Opt-in hostile-DMA selftest (§8.14)
//
// Runs only when built with ANKO_BUILD_DEV and a QEMU `iommu-testdev` function
// is present in the PCI inventory. Exercises mapped/unmapped/read-only/
// detached DMA, a canary page, and repeated-fault quarantine, then prints the
// marker the verification runner looks for. A no-op otherwise.
// ---------------------------------------------------------------------------
void smmuv3_selftest_if_testdev();

} // namespace iommu
