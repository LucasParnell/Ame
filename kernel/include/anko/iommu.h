#pragma once
// anko/iommu.h — backend-independent device-DMA isolation core.
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.3–8.6, §8.10, §8.15.
//
// The generic core owns device-DMA isolation policy: which host pages a device
// assignment may reach, with which permissions, and in what order translations
// are torn down. It is deliberately architecture-independent — the GPU
// appliance must not know whether isolation comes from ARM SMMUv3, AMD-Vi,
// Intel VT-d, or another mechanism. A backend supplies IommuBackendOps; Phase B
// ships a no-op default so the core runs and is testable without SMMU hardware,
// and the SMMUv3 backend (Phase C, kernel/iommu/smmuv3.cc) replaces it.
//
// Properties enforced here:
//   * default deny — an unmapped IOVA faults; there is no identity-DMA
//     bootstrap and no "temporarily trust the device" mode (§8.1);
//   * a requester/stream ID belongs to at most one active domain; a detached or
//     unknown requester is redirected to a permanent empty quarantine domain,
//     never to bypass translation (§8.3, §8.12);
//   * host-page ownership is proven through a page-owner query, never inferred
//     from the numeric PA (§8.3, §8.6);
//   * a domain's translation tables live in pinned Anko-owned memory and are
//     never guest writable (§8.3);
//   * domain teardown invalidates every translation before any formerly
//     reachable host page is released (§8.6, §8.12).

#include "anko/mm.h" // PhysAddr, PAGE_SIZE
#include "anko/spinlock.h"
#include "anko/syscall.h" // SysError, E_*
#include "anko/types.h"

namespace iommu {

using kernel::LockGuard;
using kernel::SpinLock;

// ---------------------------------------------------------------------------
// Identity, permissions, and memory DMA class (§8.3, §8.4)
// ---------------------------------------------------------------------------

// Device access permission requested for a mapping.
enum class DmaPerm : u32 {
    Read = 1u << 0,  // device reads system memory
    Write = 1u << 1, // device writes system memory
};

// A PCI requester (segment + requester ID) or, for non-PCI devices, a stream ID.
struct IommuDeviceId {
    u32 segment;
    u32 requester_or_stream_id;
};
static_assert(sizeof(IommuDeviceId) == 8);

// One mapping request. `length` is a byte count. IOVA, PA and length must be
// page aligned. The core validates overflow and per-page ownership before it
// installs any translation.
struct IommuMap {
    u64 iova;
    PhysAddr pa;
    u64 length;
    u32 perms;    // DmaPerm bitmask
    u32 reserved; // must be zero
};
static_assert(sizeof(IommuMap) == 32);

// DMA class of a guest mapping (§8.4), independent of CPU stage-2 permissions.
enum class VmDmaClass : u8 {
    None,         // never reachable by assigned devices
    ApplianceRam, // private RAM owned entirely by this appliance
    SharedGpu,    // explicitly shared GPU buffer/arena
};

// Device permissions a memory class permits. A mapping's requested perms must
// be a non-empty subset of this set, so a mapping can neither exceed its class
// nor be silently upgraded. None permits nothing: those pages never map.
constexpr u32 dma_class_perms(VmDmaClass cls) {
    switch (cls) {
    case VmDmaClass::ApplianceRam:
    case VmDmaClass::SharedGpu:
        return static_cast<u32>(DmaPerm::Read) | static_cast<u32>(DmaPerm::Write);
    case VmDmaClass::None:
        break;
    }
    return 0;
}

// The result of resolving a host page to its owner. `object` is an opaque
// identity (for the GPU appliance, the VM memory owner identity) that the domain
// compares against its own authorised identity; `dma_class` is the page's DMA
// class. The core never treats the numeric PA as authority.
struct PageOwnership {
    u64 object;
    VmDmaClass dma_class;
    u8 pad[7];
};
static_assert(sizeof(PageOwnership) == 16);

// Page-owner query: the only way the core learns that a host page belongs to a
// DMA-authorised object. It must return false for a page no object owns. It is
// pure — it must not mutate state; the owner keeps the page pinned for as long
// as a domain mapping can reach it.
using PageOwnerQuery = bool (*)(void* ctx, PhysAddr pa, PageOwnership* out);

// Called once per mapping record, after its translations are invalidated and
// the backend command stream is synchronised, so the owner may unpin the pages.
// It is never called while a translation can still reach them, and never for a
// page the domain did not map.
using PageRelease = void (*)(void* ctx, PhysAddr pa, u64 length);

// Page-ownership callbacks. Implemented by the VM memory subsystem. Both
// pointers default to null when the struct is zero-initialised.
struct IommuOwnerOps {
    PageOwnerQuery query;
    PageRelease release;
    void* ctx;
};

struct IommuDomain;

// Backend hooks (§8.8). Every hook is optional: a null pointer is a no-op that
// succeeds, which is exactly how the core runs on a platform with no IOMMU.
// The SMMUv3 backend (Phase C) fills them in:
//   publish_page_table_range — clean/flush page-table cache lines so freshly
//                              written descriptors are visible to the IOMMU;
//   invalidate_iotlb_range   — issue the narrowest range/domain IOTLB
//                              invalidation; global invalidation is never used
//                              on a normal per-domain mapping update;
//   sync_command_stream      — wait for architectural completion (CMD_SYNC).
// A hook that cannot prove completion must return an error; the core then keeps
// the affected pages pinned rather than recycling them (§8.12).
struct IommuBackendOps {
    SysError (*publish_page_table_range)(void* ctx, PhysAddr table_pa, u64 length);
    SysError (*invalidate_iotlb_range)(void* ctx, IommuDomain* domain, u64 iova, u64 length);
    SysError (*sync_command_stream)(void* ctx);
    // Bind a requester to a domain (its translation context) and unbind it back
    // to the default-deny state. These run inside domain_attach/domain_detach
    // after the core's binding table is updated; the backend programs the
    // architecture-specific stream entry so that a requester whose context is
    // not (yet) installed faults instead of bypassing translation. A null
    // pointer is a no-op success (the platform has no programmable IOMMU).
    SysError (*attach_stream)(void* ctx, IommuDeviceId device, IommuDomain* domain);
    SysError (*detach_stream)(void* ctx, IommuDeviceId device);
    void* ctx;
};

// The no-op backend: all hooks absent, so every operation succeeds without
// touching hardware. Selected by default so the core is unit-testable off
// target; Phase C installs the SMMUv3 backend in its place.
const IommuBackendOps& null_backend();

// ---------------------------------------------------------------------------
// Capacity (compile-time, fixed; no hot-path allocation)
// ---------------------------------------------------------------------------
constexpr u32 kMaxIommuDomains = 8;
constexpr u32 kMaxDomainMappings = 2048;             // outstanding mapping entries per domain
constexpr u32 kMaxDomainTables = 1024;               // pinned page-table pages per domain
constexpr u32 kMaxIommuStreams = 64;                 // requester/stream bindings, system-wide
constexpr u32 kIommuIovaBits = 39;                   // 3-level, 4 KiB-granule walk
constexpr u64 kIommuBlockBytes = 2ull * 1024 * 1024; // 2 MiB block granule

enum class DomainState : u8 {
    Free = 0,       // slot unused
    Active = 1,     // accepts mappings
    Quarantined = 2 // device could not be proven stopped; pages stay pinned
};

// One mapping record. Records are scanned on unmap and teardown, so the fields
// are ordered hot-first and the array is contiguous for scan locality.
struct IommuMapping {
    u64 iova;
    PhysAddr pa;
    u64 length;
    u64 owner; // owning object identity, from the page-owner query
    u32 perms; // DmaPerm bitmask
    u32 pages; // covered 4 KiB pages (= length / PAGE_SIZE)
    VmDmaClass dma_class;
    bool block; // emitted as 2 MiB block descriptors
    bool valid;
    u8 pad[4];
};
static_assert(sizeof(IommuMapping) == 48);

// ---------------------------------------------------------------------------
// IommuDomain
//
// A domain belongs to one device assignment / security principal. It is a plain
// data record — the kernel is data-oriented, so a domain is reached only
// through the `domain_*` free functions below, never through member functions.
// ---------------------------------------------------------------------------
struct IommuDomain {
    // Members carry no default initialisers so the static domain pool is
    // trivially default constructible and lands in .bss with no dynamic
    // initialisation; domain_create() establishes every value explicitly.
    u32 index_;
    u32 mapping_count_;
    u32 table_count_;
    u32 pinned_pages_;
    DomainState state_;
    bool in_use_;
    u8 pad0_[2];
    u64 authorized_object_;
    u64 attached_mask_;
    PhysAddr root_table_;
    PhysAddr tables_[kMaxDomainTables];
    IommuOwnerOps owner_;
    IommuBackendOps backend_;
    SpinLock lock_;
    IommuMapping mappings_[kMaxDomainMappings];
};

// ---------------------------------------------------------------------------
// Domain operations
//
// Every operation takes the domain by reference: pass `*ptr` when you hold a
// pointer (as returned by domain_create / stream_domain / quarantine_domain).
// ---------------------------------------------------------------------------

// Install translations for every map. All-or-nothing: on any validation or
// allocation failure nothing is installed and the domain is unchanged.
SysError domain_map_batch(IommuDomain& domain, const IommuMap* maps, u32 count);

// Remove translations previously installed by domain_map_batch. Each record's
// host pages are released to the owner only after the backend confirms
// invalidation; if it cannot, the domain is quarantined and the pages stay
// pinned (§8.6, §8.12).
SysError domain_unmap_batch(IommuDomain& domain, const IommuMap* maps, u32 count);

// Force the backend command stream to complete.
SysError domain_sync(IommuDomain& domain);

// Bind a requester/stream ID to this domain. Fails with E_BUSY if it is already
// bound to a different active domain. Rebinding away from the quarantine domain
// is permitted, as is a redundant attach.
SysError domain_attach(IommuDomain& domain, IommuDeviceId device);

// Unbind a requester from this domain and redirect it to the quarantine domain,
// so it faults instead of bypassing translation.
SysError domain_detach(IommuDomain& domain, IommuDeviceId device);

// Accessors. These read plain data; callers must not depend on the member
// layout, only on these functions.
u32 domain_index(const IommuDomain& domain);
DomainState domain_state(const IommuDomain& domain);
bool domain_is_active(const IommuDomain& domain);
u32 domain_live_mappings(const IommuDomain& domain);
u32 domain_pinned_pages(const IommuDomain& domain);
u64 domain_attached_mask(const IommuDomain& domain);

// ---------------------------------------------------------------------------
// Domain lifecycle and lookup
// ---------------------------------------------------------------------------

// Create an active domain authorised to DMA host pages whose page-owner query
// reports `authorized_object`. `owner` supplies the ownership callbacks;
// `backend` supplies the architecture hooks (pass null_backend() for none).
SysError domain_create(u64 authorized_object, const IommuOwnerOps& owner,
                       const IommuBackendOps& backend, IommuDomain** out);

// Destroy a domain. Every translation is invalidated, synchronised, and only
// then are the host pages released to their owner. If the backend cannot prove
// invalidation, the domain is quarantined and the pages stay pinned.
void domain_destroy(IommuDomain* domain);

IommuDomain* domain_by_index(u32 index);

// The permanent empty quarantine domain. Detached requesters are redirected
// here, where domain_map_batch always fails, so a stale translation can never
// reach a page. Created lazily; never destroyed.
IommuDomain* quarantine_domain();

// Resolve a requester/stream ID to its domain, or null if it is unbound. A null
// result means the backend must fault the requester rather than bypass.
IommuDomain* stream_domain(IommuDeviceId device);

// Create the quarantine domain and clear the stream table. Idempotent; also
// performed lazily on first use, so it need not be called from kernel_main.
void init();

// Record an IOMMU fault event (called by the backend's fault/event queue drain
// in Phase C). The full rate-limited logging and ACTIVE→FAULTED→… state machine
// live in kernel/iommu/faults.cc; this core entry point only records it.
void note_fault(IommuDeviceId device, u64 iova, u32 reason);

// Record that a requester or domain was moved into quarantine (called by the
// backend's fault-containment path alongside note_fault).
void note_quarantine();

// ---------------------------------------------------------------------------
// Debug counters (§8.15). Index with IommuCounter; unknown ids return 0.
// ---------------------------------------------------------------------------
enum IommuCounter : u64 {
    IOMMU_MAP_OPS = 0,         // mapping entries installed
    IOMMU_UNMAP_OPS = 1,       // mapping entries removed
    IOMMU_MAP_BYTES = 2,       // bytes covered by installed mappings
    IOMMU_BLOCK_MAP_BYTES = 3, // bytes installed as 2 MiB block mappings
    IOMMU_TLBI_RANGE = 4,      // range-scoped invalidations issued
    IOMMU_TLBI_DOMAIN = 5,     // domain-scoped invalidations issued
    IOMMU_TLBI_GLOBAL = 6,     // global invalidations (expected to stay zero)
    IOMMU_FAULTS = 7,          // recorded fault events
    IOMMU_QUARANTINES = 8,     // domains/requesters moved to quarantine
    IOMMU_COUNTER_COUNT = 9,
};

u64 debug_counter(u64 id);

// The most recent fault reported to note_fault(), for diagnostics.
struct IommuFaultRecord {
    IommuDeviceId device;
    u64 iova;
    u32 reason;
    u32 pad;
};
IommuFaultRecord last_fault();

} // namespace iommu
