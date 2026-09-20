// kernel/iommu/core.cc — backend-independent IOMMU core.
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.3–8.6, §8.10, §8.15.
//
// See anko/iommu.h for the contract. This file implements:
//   * the fixed-capacity domain pool, the permanent empty quarantine domain, and
//     the requester/stream binding table;
//   * per-domain 3-level, 4 KiB-granule translation tables (with 2 MiB block
//     leaves where alignment permits) allocated from pinned Anko memory — the
//     page allocator — and never mapped into any guest;
//   * mapping validation: shape/overflow, per-page ownership, and minimum
//     permissions derived from the memory DMA class;
//   * teardown ordered so a host page is released only after the backend proves
//     every translation that could reach it has been invalidated.

#include "anko/iommu.h"

#include "anko/log.h"
#include "libk/string.h"

namespace iommu {

namespace {

// --- AArch64 long-descriptor bits, used generically -------------------------
constexpr u64 kDescValid = 1ull << 0;
constexpr u64 kDescTable = 1ull << 1;
// Level-3 descriptor type field [1:0] = 0b11 selects a 4 KiB page.  It is
// numerically shared with kDescTable (which means "table" at levels 1-2) because
// the encoding is level dependent; block leaves at levels 1-2 stay 0b01.
constexpr u64 kDescPageType = 0b11ull;
constexpr u64 kDescMemNormal = 0xFull << 2;
constexpr u64 kDescShInner = 3ull << 8;
constexpr u64 kDescAf = 1ull << 10;
constexpr u64 kDescApRead = 1ull << 6;  // device read permitted
constexpr u64 kDescApWrite = 1ull << 7; // device write permitted
constexpr u64 kDescXn = 1ull << 54;     // DMA never executes
constexpr u64 kAddrMask = 0x0000'FFFF'FFFF'F000ull;
constexpr u64 kBlockAddrMask = 0x0000'FFFF'FFE0'0000ull; // 2 MiB granule

constexpr u64 kPageBytes = static_cast<u64>(PAGE_SIZE);
constexpr u64 kIovaLimit = 1ull << kIommuIovaBits;
constexpr u32 kPermMask = static_cast<u32>(DmaPerm::Read) | static_cast<u32>(DmaPerm::Write);
constexpr u32 kNoIndex = 0xFFFF'FFFFu;
constexpr u32 kQuarantineIndex = 0;

// --- module state -----------------------------------------------------------
IommuDomain g_domains[kMaxIommuDomains];

struct StreamBinding {
    u32 segment;
    u32 sid;
    u32 domain_index;
    bool used;
    u8 pad[3];
};
StreamBinding g_streams[kMaxIommuStreams];
SpinLock g_stream_lock;

bool g_initialised;
u64 g_counters[IOMMU_COUNTER_COUNT];
IommuFaultRecord g_last_fault;

const IommuBackendOps g_null_backend = {}; // all hooks null: no-op success

void counter_inc(u64 id) {
    __atomic_add_fetch(&g_counters[id], 1ull, __ATOMIC_RELAXED);
}

void counter_add(u64 id, u64 amount) {
    if (amount) {
        __atomic_add_fetch(&g_counters[id], amount, __ATOMIC_RELAXED);
    }
}

u64* table_ptr(PhysAddr pa) {
    return reinterpret_cast<u64*>(pa);
}

u32 l1_index(u64 addr) {
    return static_cast<u32>((addr >> 30) & 0x1FFu);
}
u32 l2_index(u64 addr) {
    return static_cast<u32>((addr >> 21) & 0x1FFu);
}
u32 l3_index(u64 addr) {
    return static_cast<u32>((addr >> 12) & 0x1FFu);
}

u64 leaf_attrs(u32 perms) {
    u64 attrs = kDescValid | kDescMemNormal | kDescShInner | kDescAf | kDescXn;
    if (perms & static_cast<u32>(DmaPerm::Read)) {
        attrs |= kDescApRead;
    }
    if (perms & static_cast<u32>(DmaPerm::Write)) {
        attrs |= kDescApWrite;
    }
    return attrs;
}

void publish_table(IommuDomain* domain, PhysAddr table_pa) {
    if (domain->backend_.publish_page_table_range) {
        (void)domain->backend_.publish_page_table_range(domain->backend_.ctx, table_pa, kPageBytes);
    }
}

// Allocate a zeroed page-table page from pinned kernel memory. The page is
// kernel-owned, physically contiguous, and never mapped into a guest.
PhysAddr alloc_table(IommuDomain* domain) {
    if (domain->table_count_ >= kMaxDomainTables) {
        return 0;
    }
    const PhysAddr pa = mm::alloc_page();
    if (!pa) {
        return 0;
    }
    libk::memset(reinterpret_cast<void*>(pa), 0, PAGE_SIZE);
    domain->tables_[domain->table_count_++] = pa;
    return pa;
}

void release_table(IommuDomain* domain, PhysAddr pa) {
    for (u32 i = 0; i < domain->table_count_; ++i) {
        if (domain->tables_[i] != pa) {
            continue;
        }
        domain->tables_[i] = domain->tables_[--domain->table_count_];
        domain->tables_[domain->table_count_] = 0;
        mm::free_page(pa);
        return;
    }
}

bool table_empty(PhysAddr pa) {
    for (u32 i = 0; i < 512; ++i) {
        if (table_ptr(pa)[i] & kDescValid) {
            return false;
        }
    }
    return true;
}

// Resolve (and create, if absent) the next-level table under `idx` in `table`.
SysError ensure_child(IommuDomain* domain, PhysAddr table, u32 idx, PhysAddr* out) {
    u64& entry = table_ptr(table)[idx];
    if (entry & kDescValid) {
        if (!(entry & kDescTable)) {
            return E_BUSY; // a block leaf already occupies this slot
        }
        *out = entry & kAddrMask;
        return E_OK;
    }
    const PhysAddr child = alloc_table(domain);
    if (!child) {
        return E_NOMEM;
    }
    entry = child | kDescValid | kDescTable;
    *out = child;
    return E_OK;
}

SysError emit_pages(IommuDomain* domain, const IommuMap& map) {
    for (u64 off = 0; off < map.length; off += kPageBytes) {
        const u64 iova = map.iova + off;
        const PhysAddr pa = map.pa + off;
        PhysAddr l2 = 0;
        PhysAddr l3 = 0;
        SysError rc = ensure_child(domain, domain->root_table_, l1_index(iova), &l2);
        if (rc != E_OK) {
            return rc;
        }
        rc = ensure_child(domain, l2, l2_index(iova), &l3);
        if (rc != E_OK) {
            return rc;
        }
        u64& leaf = table_ptr(l3)[l3_index(iova)];
        if (leaf & kDescValid) {
            return E_BUSY;
        }
        // A level-3 entry must be a page descriptor: type field [1:0] = 0b11.
        leaf = (pa & kAddrMask) | leaf_attrs(map.perms) | kDescPageType;
    }
    return E_OK;
}

SysError emit_blocks(IommuDomain* domain, const IommuMap& map) {
    for (u64 off = 0; off < map.length; off += kIommuBlockBytes) {
        const u64 iova = map.iova + off;
        const PhysAddr pa = map.pa + off;
        PhysAddr l2 = 0;
        SysError rc = ensure_child(domain, domain->root_table_, l1_index(iova), &l2);
        if (rc != E_OK) {
            return rc;
        }
        u64& block = table_ptr(l2)[l2_index(iova)];
        if (block & kDescValid) {
            return E_BUSY; // an existing table or block leaf owns this range
        }
        block = (pa & kBlockAddrMask) | leaf_attrs(map.perms);
    }
    return E_OK;
}

void clear_pages(IommuDomain* domain, u64 iova, u64 length) {
    for (u64 off = 0; off < length; off += kPageBytes) {
        const u64 addr = iova + off;
        u64& l1_entry = table_ptr(domain->root_table_)[l1_index(addr)];
        if (!(l1_entry & kDescValid) || !(l1_entry & kDescTable)) {
            continue;
        }
        u64& l2_entry = table_ptr(l1_entry & kAddrMask)[l2_index(addr)];
        if (!(l2_entry & kDescValid) || !(l2_entry & kDescTable)) {
            continue;
        }
        table_ptr(l2_entry & kAddrMask)[l3_index(addr)] = 0;
    }
}

void clear_blocks(IommuDomain* domain, u64 iova, u64 length) {
    for (u64 off = 0; off < length; off += kIommuBlockBytes) {
        const u64 addr = iova + off;
        u64& l1_entry = table_ptr(domain->root_table_)[l1_index(addr)];
        if (!(l1_entry & kDescValid) || !(l1_entry & kDescTable)) {
            continue;
        }
        u64& l2_entry = table_ptr(l1_entry & kAddrMask)[l2_index(addr)];
        if ((l2_entry & kDescValid) && !(l2_entry & kDescTable)) {
            l2_entry = 0;
        }
    }
}

void clear_record(IommuDomain* domain, const IommuMapping& record) {
    if (record.block) {
        clear_blocks(domain, record.iova, record.length);
    } else {
        clear_pages(domain, record.iova, record.length);
    }
}

void prune_chain(IommuDomain* domain, u64 addr) {
    u64& l1_entry = table_ptr(domain->root_table_)[l1_index(addr)];
    if (!(l1_entry & kDescValid) || !(l1_entry & kDescTable)) {
        return;
    }
    const PhysAddr l2_pa = l1_entry & kAddrMask;
    u64& l2_entry = table_ptr(l2_pa)[l2_index(addr)];
    if ((l2_entry & kDescValid) && (l2_entry & kDescTable)) {
        const PhysAddr l3_pa = l2_entry & kAddrMask;
        if (table_empty(l3_pa)) {
            l2_entry = 0;
            release_table(domain, l3_pa);
        }
    }
    if (table_empty(l2_pa)) {
        l1_entry = 0;
        release_table(domain, l2_pa);
    }
}

// Prune once per 2 MiB bucket covering the cleared range. A 2 MiB bucket lies
// within a single L2 entry, so this visits every table the range could empty.
void prune_range(IommuDomain* domain, u64 iova, u64 length) {
    const u64 first = iova & ~(kIommuBlockBytes - 1);
    for (u64 addr = first; addr < iova + length; addr += kIommuBlockBytes) {
        prune_chain(domain, addr);
    }
}

// IOVA/length shape shared by map and unmap. `pa` and `perms` are validated by
// the map path only: an unmap request is matched by IOVA and length, so it does
// not need to restate them.
SysError check_extent(const IommuMap& map) {
    if (map.length == 0) {
        return E_INVALID;
    }
    if ((map.iova & (kPageBytes - 1)) || (map.length & (kPageBytes - 1))) {
        return E_INVALID;
    }
    u64 iova_end = 0;
    if (__builtin_add_overflow(map.iova, map.length, &iova_end)) {
        return E_OVERFLOW;
    }
    if (iova_end > kIovaLimit) {
        return E_INVALID;
    }
    return E_OK;
}

SysError check_mapping(const IommuMap& map) {
    if (map.reserved != 0 || (map.pa & (kPageBytes - 1))) {
        return E_INVALID;
    }
    if (map.perms == 0 || (map.perms & ~kPermMask)) {
        return E_INVALID;
    }
    const SysError rc = check_extent(map);
    if (rc != E_OK) {
        return rc;
    }
    u64 pa_end = 0;
    if (__builtin_add_overflow(map.pa, map.length, &pa_end)) {
        return E_OVERFLOW;
    }
    return E_OK;
}

// The requested permissions must be non-empty and a subset of what the memory
// class permits, so a mapping can neither exceed its class nor be upgraded.
bool perms_allowed(VmDmaClass cls, u32 perms) {
    return perms != 0 && (perms & ~dma_class_perms(cls)) == 0;
}

// Validate that every host page in the map is owned by this domain and carries
// a DMA class that permits the requested access. The page-owner query is the
// only source of truth; the numeric PA is never trusted.
SysError validate_ownership(IommuDomain* domain, const IommuMap& map, u64* out_object,
                            VmDmaClass* out_class) {
    if (!domain->owner_.query) {
        return E_PERM;
    }
    PageOwnership first;
    if (!domain->owner_.query(domain->owner_.ctx, map.pa, &first)) {
        return E_PERM; // unknown page: not owned by any object
    }
    if (first.object != domain->authorized_object_) {
        return E_PERM; // owned by a different principal
    }
    if (first.dma_class == VmDmaClass::None) {
        return E_INVALID; // None is never reachable by an assigned device
    }
    if (!perms_allowed(first.dma_class, map.perms)) {
        return E_INVALID;
    }
    for (u64 pa = map.pa + kPageBytes; pa < map.pa + map.length; pa += kPageBytes) {
        PageOwnership page;
        if (!domain->owner_.query(domain->owner_.ctx, pa, &page)) {
            return E_PERM;
        }
        if (page.object != first.object || page.dma_class != first.dma_class) {
            return E_PERM; // a mapping entry must be homogeneous
        }
    }
    *out_object = first.object;
    *out_class = first.dma_class;
    return E_OK;
}

bool is_block_map(const IommuMap& map) {
    return (map.iova % kIommuBlockBytes) == 0 && (map.pa % kIommuBlockBytes) == 0 &&
           (map.length % kIommuBlockBytes) == 0;
}

void rollback(IommuDomain* domain, u32 record_start, u32 table_start) {
    for (u32 r = record_start; r < domain->mapping_count_; ++r) {
        clear_record(domain, domain->mappings_[r]);
    }
    domain->mapping_count_ = record_start;
    for (u32 t = domain->table_count_; t > table_start;) {
        --t;
        mm::free_page(domain->tables_[t]);
    }
    domain->table_count_ = table_start;
}

u32 find_record(IommuDomain* domain, u64 iova, u64 length) {
    for (u32 i = 0; i < domain->mapping_count_; ++i) {
        const IommuMapping& record = domain->mappings_[i];
        if (record.valid && record.iova == iova && record.length == length) {
            return i;
        }
    }
    return kNoIndex;
}

bool invalidate_and_sync_range(IommuDomain* domain, u64 iova, u64 length) {
    if (domain->backend_.invalidate_iotlb_range) {
        if (domain->backend_.invalidate_iotlb_range(domain->backend_.ctx, domain, iova, length) !=
            E_OK) {
            return false;
        }
        counter_inc(IOMMU_TLBI_RANGE);
    }
    if (domain->backend_.sync_command_stream) {
        if (domain->backend_.sync_command_stream(domain->backend_.ctx) != E_OK) {
            return false;
        }
    }
    return true;
}

bool invalidate_and_sync_domain(IommuDomain* domain) {
    if (domain->backend_.invalidate_iotlb_range) {
        if (domain->backend_.invalidate_iotlb_range(domain->backend_.ctx, domain, 0, ~0ull) !=
            E_OK) {
            return false;
        }
        counter_inc(IOMMU_TLBI_DOMAIN);
    }
    if (domain->backend_.sync_command_stream) {
        if (domain->backend_.sync_command_stream(domain->backend_.ctx) != E_OK) {
            return false;
        }
    }
    return true;
}

void ensure_initialised() {
    if (g_initialised) {
        return;
    }
    LockGuard guard(g_stream_lock);
    if (g_initialised) {
        return;
    }
    libk::memset(g_streams, 0, sizeof(g_streams));
    IommuDomain& quarantine = g_domains[kQuarantineIndex];
    libk::memset(&quarantine, 0, sizeof(quarantine));
    quarantine.index_ = kQuarantineIndex;
    quarantine.state_ = DomainState::Quarantined;
    quarantine.in_use_ = true;
    g_initialised = true;
}

// Install the requester's translation context through the backend. A null hook
// means the platform has no programmable IOMMU, so there is nothing to do.
bool program_stream(IommuDomain* domain, IommuDeviceId device) {
    if (!domain->backend_.attach_stream) {
        return true;
    }
    return domain->backend_.attach_stream(domain->backend_.ctx, device, domain) == E_OK;
}

} // namespace

const IommuBackendOps& null_backend() {
    return g_null_backend;
}

SysError domain_map_batch(IommuDomain& domain, const IommuMap* maps, u32 count) {
    if (domain.state_ != DomainState::Active) {
        return E_PERM; // quarantine (and free) domains never map
    }
    if (!maps || count == 0) {
        return E_INVALID;
    }
    LockGuard guard(domain.lock_);
    if (count > kMaxDomainMappings - domain.mapping_count_) {
        return E_OVERFLOW;
    }

    const u32 record_start = domain.mapping_count_;
    const u32 table_start = domain.table_count_;

    for (u32 i = 0; i < count; ++i) {
        const IommuMap& map = maps[i];
        SysError rc = check_mapping(map);
        if (rc != E_OK) {
            rollback(&domain, record_start, table_start);
            return rc;
        }
        u64 owner = 0;
        VmDmaClass cls = VmDmaClass::None;
        rc = validate_ownership(&domain, map, &owner, &cls);
        if (rc != E_OK) {
            rollback(&domain, record_start, table_start);
            return rc;
        }
        const bool block = is_block_map(map);
        rc = block ? emit_blocks(&domain, map) : emit_pages(&domain, map);
        if (rc != E_OK) {
            rollback(&domain, record_start, table_start);
            return rc;
        }
        IommuMapping& record = domain.mappings_[domain.mapping_count_++];
        libk::memset(&record, 0, sizeof(record));
        record.iova = map.iova;
        record.pa = map.pa;
        record.length = map.length;
        record.owner = owner;
        record.perms = map.perms;
        record.pages = static_cast<u32>(map.length / kPageBytes);
        record.dma_class = cls;
        record.block = block;
        record.valid = true;
    }

    // Publish freshly written page tables, then wait for the command stream.
    for (u32 t = table_start; t < domain.table_count_; ++t) {
        publish_table(&domain, domain.tables_[t]);
    }
    publish_table(&domain, domain.root_table_);
    if (domain.backend_.sync_command_stream &&
        domain.backend_.sync_command_stream(domain.backend_.ctx) != E_OK) {
        rollback(&domain, record_start, table_start);
        return E_FAULT;
    }

    for (u32 i = 0; i < count; ++i) {
        counter_inc(IOMMU_MAP_OPS);
        counter_add(IOMMU_MAP_BYTES, maps[i].length);
        if (is_block_map(maps[i])) {
            counter_add(IOMMU_BLOCK_MAP_BYTES, maps[i].length);
        }
        domain.pinned_pages_ += static_cast<u32>(maps[i].length / kPageBytes);
    }
    return E_OK;
}

SysError domain_unmap_batch(IommuDomain& domain, const IommuMap* maps, u32 count) {
    if (domain.state_ == DomainState::Free) {
        return E_INVALID;
    }
    if (!maps || count == 0) {
        return E_INVALID;
    }
    LockGuard guard(domain.lock_);
    for (u32 i = 0; i < count; ++i) {
        const SysError rc = check_extent(maps[i]);
        if (rc != E_OK) {
            return rc;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const IommuMap& map = maps[i];
        const u32 idx = find_record(&domain, map.iova, map.length);
        if (idx == kNoIndex) {
            return E_NOTFOUND;
        }
        IommuMapping record = domain.mappings_[idx];
        clear_record(&domain, record);
        if (!invalidate_and_sync_range(&domain, record.iova, record.length)) {
            // Cannot prove the device stopped; keep the pages pinned and stop
            // accepting work on this domain.
            domain.state_ = DomainState::Quarantined;
            counter_inc(IOMMU_QUARANTINES);
            return E_FAULT;
        }
        if (domain.owner_.release) {
            domain.owner_.release(domain.owner_.ctx, record.pa, record.length);
        }
        prune_range(&domain, record.iova, record.length);
        if (domain.pinned_pages_ >= record.pages) {
            domain.pinned_pages_ -= record.pages;
        } else {
            domain.pinned_pages_ = 0;
        }
        counter_inc(IOMMU_UNMAP_OPS);
        domain.mappings_[idx] = domain.mappings_[--domain.mapping_count_];
    }
    return E_OK;
}

SysError domain_sync(IommuDomain& domain) {
    LockGuard guard(domain.lock_);
    if (domain.backend_.sync_command_stream) {
        return domain.backend_.sync_command_stream(domain.backend_.ctx);
    }
    return E_OK;
}

SysError domain_attach(IommuDomain& domain, IommuDeviceId device) {
    // Attaching to the quarantine (or a faulted) domain is allowed: it is the
    // default-deny target a requester is bound to when it has no valid domain.
    if (domain.state_ == DomainState::Free) {
        return E_PERM; // slot not in use
    }
    ensure_initialised();
    LockGuard guard(g_stream_lock);
    for (u32 i = 0; i < kMaxIommuStreams; ++i) {
        StreamBinding& binding = g_streams[i];
        if (!binding.used || binding.segment != device.segment ||
            binding.sid != device.requester_or_stream_id) {
            continue;
        }
        if (binding.domain_index == domain.index_) {
            return E_OK; // redundant attach
        }
        if (binding.domain_index != kQuarantineIndex) {
            // A requester already bound to a *quarantined* (deny) domain, or to
            // a slot that no longer exists, may be re-attached to a fresh active
            // domain: a quarantined domain translates nothing, so this is a
            // rebinding away from a faulting context, not a bypass (§8.12).
            IommuDomain* current = domain_by_index(binding.domain_index);
            const bool current_denies =
                current == nullptr || domain_state(*current) == DomainState::Quarantined;
            if (!current_denies) {
                return E_BUSY; // owned by another live domain
            }
        }
        // Program the backend context before committing the binding, so a
        // failed install leaves the requester on its previous (deny) context.
        if (!program_stream(&domain, device)) {
            return E_FAULT;
        }
        binding.domain_index = domain.index_;
        __atomic_or_fetch(&domain.attached_mask_, 1ull << i, __ATOMIC_RELAXED);
        return E_OK;
    }
    for (u32 i = 0; i < kMaxIommuStreams; ++i) {
        StreamBinding& binding = g_streams[i];
        if (binding.used) {
            continue;
        }
        if (!program_stream(&domain, device)) {
            return E_FAULT;
        }
        binding.used = true;
        binding.segment = device.segment;
        binding.sid = device.requester_or_stream_id;
        binding.domain_index = domain.index_;
        __atomic_or_fetch(&domain.attached_mask_, 1ull << i, __ATOMIC_RELAXED);
        return E_OK;
    }
    return E_OVERFLOW;
}

SysError domain_detach(IommuDomain& domain, IommuDeviceId device) {
    ensure_initialised();
    LockGuard guard(g_stream_lock);
    for (u32 i = 0; i < kMaxIommuStreams; ++i) {
        StreamBinding& binding = g_streams[i];
        if (!binding.used || binding.segment != device.segment ||
            binding.sid != device.requester_or_stream_id) {
            continue;
        }
        if (binding.domain_index != domain.index_) {
            return E_NOTFOUND;
        }
        binding.domain_index = kQuarantineIndex;
        __atomic_and_fetch(&domain.attached_mask_, ~(1ull << i), __ATOMIC_RELAXED);
        counter_inc(IOMMU_QUARANTINES);
        // Redirect the requester to a faulting context. If the backend cannot
        // prove it stopped translating, quarantine this domain so its pages
        // stay pinned rather than being recycled (§8.12).
        if (domain.backend_.detach_stream &&
            domain.backend_.detach_stream(domain.backend_.ctx, device) != E_OK) {
            domain.state_ = DomainState::Quarantined;
            counter_inc(IOMMU_QUARANTINES);
            return E_FAULT;
        }
        return E_OK;
    }
    return E_NOTFOUND;
}

// --- accessors -------------------------------------------------------------
u32 domain_index(const IommuDomain& domain) {
    return domain.index_;
}

DomainState domain_state(const IommuDomain& domain) {
    return domain.state_;
}

bool domain_is_active(const IommuDomain& domain) {
    return domain.state_ == DomainState::Active;
}

u32 domain_live_mappings(const IommuDomain& domain) {
    return domain.mapping_count_;
}

u32 domain_pinned_pages(const IommuDomain& domain) {
    return domain.pinned_pages_;
}

u64 domain_attached_mask(const IommuDomain& domain) {
    return __atomic_load_n(&domain.attached_mask_, __ATOMIC_RELAXED);
}

SysError domain_create(u64 authorized_object, const IommuOwnerOps& owner,
                       const IommuBackendOps& backend, IommuDomain** out) {
    if (!out) {
        return E_INVALID;
    }
    *out = nullptr;
    ensure_initialised();
    LockGuard guard(g_stream_lock);
    for (u32 i = 0; i < kMaxIommuDomains; ++i) {
        if (i == kQuarantineIndex || g_domains[i].in_use_) {
            continue;
        }
        IommuDomain& domain = g_domains[i];
        libk::memset(&domain, 0, sizeof(domain));
        domain.index_ = i;
        domain.in_use_ = true;
        domain.state_ = DomainState::Active;
        domain.authorized_object_ = authorized_object;
        domain.owner_ = owner;
        domain.backend_ = backend;
        const PhysAddr root = alloc_table(&domain);
        if (!root) {
            libk::memset(&domain, 0, sizeof(domain));
            return E_NOMEM;
        }
        domain.root_table_ = root;
        publish_table(&domain, root);
        *out = &domain;
        return E_OK;
    }
    return E_OVERFLOW;
}

void domain_destroy(IommuDomain* domain) {
    if (!domain || !domain->in_use_ || domain->index_ == kQuarantineIndex) {
        return;
    }
    LockGuard guard(domain->lock_);

    // Clear every translation, then invalidate the domain and synchronise. Only
    // when the backend proves completion are the host pages released.
    for (u32 r = 0; r < domain->mapping_count_; ++r) {
        clear_record(domain, domain->mappings_[r]);
    }
    if (!invalidate_and_sync_domain(domain)) {
        domain->state_ = DomainState::Quarantined;
        counter_inc(IOMMU_QUARANTINES);
        return; // pages stay pinned; the slot is retained
    }
    for (u32 r = 0; r < domain->mapping_count_; ++r) {
        const IommuMapping& record = domain->mappings_[r];
        if (domain->owner_.release) {
            domain->owner_.release(domain->owner_.ctx, record.pa, record.length);
        }
        counter_inc(IOMMU_UNMAP_OPS);
    }
    for (u32 t = domain->table_count_; t > 0;) {
        --t;
        mm::free_page(domain->tables_[t]);
    }
    libk::memset(domain, 0, sizeof(*domain));
}

IommuDomain* domain_by_index(u32 index) {
    if (index >= kMaxIommuDomains || !g_domains[index].in_use_) {
        return nullptr;
    }
    return &g_domains[index];
}

IommuDomain* quarantine_domain() {
    ensure_initialised();
    return &g_domains[kQuarantineIndex];
}

IommuDomain* stream_domain(IommuDeviceId device) {
    ensure_initialised();
    LockGuard guard(g_stream_lock);
    for (u32 i = 0; i < kMaxIommuStreams; ++i) {
        const StreamBinding& binding = g_streams[i];
        if (!binding.used || binding.segment != device.segment ||
            binding.sid != device.requester_or_stream_id) {
            continue;
        }
        return domain_by_index(binding.domain_index);
    }
    return nullptr; // unbound requester: the backend must fault it
}

void init() {
    ensure_initialised();
}

void note_fault(IommuDeviceId device, u64 iova, u32 reason) {
    counter_inc(IOMMU_FAULTS);
    g_last_fault.device = device;
    g_last_fault.iova = iova;
    g_last_fault.reason = reason;
    g_last_fault.pad = 0;
}

void note_quarantine() {
    counter_inc(IOMMU_QUARANTINES);
}

u64 debug_counter(u64 id) {
    if (id >= IOMMU_COUNTER_COUNT) {
        return 0;
    }
    return __atomic_load_n(&g_counters[id], __ATOMIC_RELAXED);
}

IommuFaultRecord last_fault() {
    return g_last_fault;
}

} // namespace iommu
