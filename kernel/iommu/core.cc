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

// Descriptor encodings and address limits are supplied by the backend.
constexpr u64 kPageBytes = static_cast<u64>(PAGE_SIZE);
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

const IommuBackendOps g_null_backend = {}; // explicit software model
struct BackendRoute { IommuRoute route; IommuBackendOps ops; };
BackendRoute g_routes[kMaxIommuBackends];
u32 g_route_count;
constexpr u8 kTableDirty = 1;
constexpr u8 kTableRetired = 2;

void set_state(IommuDomain& domain, DomainState state) {
    __atomic_store(&domain.state_, &state, __ATOMIC_RELEASE);
}
void quarantine(IommuDomain& domain) {
    set_state(domain, DomainState::Quarantined);
    __atomic_add_fetch(&g_counters[IOMMU_QUARANTINES], 1ull, __ATOMIC_RELAXED);
}
bool valid_backend(const IommuBackendOps& ops) {
    if (ops.iova_bits > kIommuIovaBits || ops.pa_bits > 48 ||
        (ops.pa_bits && ops.pa_bits < 12)) return false;
    if (ops.hardware && (!ops.iova_bits || !ops.pa_bits || !ops.table_format ||
        !ops.publish_page_table_range || !ops.invalidate_iotlb_range ||
        !ops.sync_command_stream || !ops.attach_stream || !ops.detach_stream || !ops.ready)) return false;
    const auto& f = ops.table_format ? *ops.table_format : kArmStage2Tables;
    // The shared walker needs a 48-bit descriptor address field and distinct
    // table/block types. Reject unsupported shapes instead of masking a PA.
    return f.address_mask == kArmStage2Tables.address_mask && f.valid_mask &&
        f.table_mask && (f.table_bits & f.valid_mask) &&
        (f.table_bits & f.table_mask) && !(f.block_bits & f.table_mask) &&
        (f.block_bits & f.valid_mask) && (f.page_bits & f.valid_mask) &&
        !((f.table_bits | f.block_bits | f.page_bits | f.attributes |
           f.read_bits | f.write_bits) & f.address_mask);
}
const IommuTableFormat& format(const IommuDomain& domain) {
    return *domain.backend_.table_format;
}
bool valid(const IommuDomain& domain, u64 entry) {
    return (entry & format(domain).valid_mask) != 0;
}
bool is_table(const IommuDomain& domain, u64 entry) {
    return valid(domain, entry) && (entry & format(domain).table_mask) != 0;
}
PhysAddr address(const IommuDomain& domain, u64 entry) {
    return entry & format(domain).address_mask;
}

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

u64 leaf_attrs(const IommuDomain& domain, u32 perms) {
    const auto& f = format(domain);
    return f.attributes | ((perms & 1u) ? f.read_bits : 0) |
        ((perms & 2u) ? f.write_bits : 0);
}

void write_entry(IommuDomain& domain, PhysAddr table, u32 index, u64 value) {
    table_ptr(table)[index] = value;
    for (u32 t = 0; t < domain.table_count_; ++t) {
        if (domain.tables_[t] == table) {
            domain.table_flags_[t] |= kTableDirty;
            return;
        }
    }
}

SysError publish_dirty(IommuDomain& domain) {
    // New children precede their parents; only touched tables are published,
    // including existing leaf tables and cleared descriptors before TLBI.
    for (u32 t = domain.table_count_; t > 0;) {
        --t;
        if (!(domain.table_flags_[t] & kTableDirty)) continue;
        if (domain.backend_.publish_page_table_range) {
            const auto rc = domain.backend_.publish_page_table_range(
                domain.backend_.ctx, domain.tables_[t], kPageBytes);
            if (rc != E_OK) return rc;
        }
        domain.table_flags_[t] &= ~kTableDirty;
    }
    return E_OK;
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
    if ((pa & (kPageBytes - 1)) || pa >= (1ull << domain->backend_.pa_bits)) {
        mm::free_page(pa);
        return 0;
    }
    domain->tables_[domain->table_count_] = pa;
    domain->table_flags_[domain->table_count_++] = kTableDirty;
    return pa;
}

void retire_table(IommuDomain* domain, PhysAddr pa) {
    for (u32 t = 0; t < domain->table_count_; ++t) {
        if (domain->tables_[t] == pa) domain->table_flags_[t] |= kTableRetired;
    }
}

// Called only after parent-descriptor publication and IOTLB completion.
void release_retired(IommuDomain& domain) {
    for (u32 t = 0; t < domain.table_count_;) {
        if (!(domain.table_flags_[t] & kTableRetired)) { ++t; continue; }
        mm::free_page(domain.tables_[t]);
        --domain.table_count_;
        // Stable compaction preserves child-before-parent publication order.
        for (u32 j = t; j < domain.table_count_; ++j) {
            domain.tables_[j] = domain.tables_[j + 1];
            domain.table_flags_[j] = domain.table_flags_[j + 1];
        }
    }
}

bool table_empty(const IommuDomain& domain, PhysAddr pa) {
    for (u32 i = 0; i < 512; ++i) {
        if (valid(domain, table_ptr(pa)[i])) return false;
    }
    return true;
}

// Resolve (and create, if absent) the next-level table under `idx` in `table`.
SysError ensure_child(IommuDomain* domain, PhysAddr table, u32 idx, PhysAddr* out) {
    u64& entry = table_ptr(table)[idx];
    if (valid(*domain, entry)) {
        if (!is_table(*domain, entry)) {
            return E_BUSY; // a block leaf already occupies this slot
        }
        *out = address(*domain, entry);
        return E_OK;
    }
    const PhysAddr child = alloc_table(domain);
    if (!child) {
        return E_NOMEM;
    }
    write_entry(*domain, table, idx, child | format(*domain).table_bits);
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
        if (valid(*domain, leaf)) {
            return E_BUSY;
        }
        // A level-3 entry must be a page descriptor: type field [1:0] = 0b11.
        write_entry(*domain, l3, l3_index(iova), pa | leaf_attrs(*domain, map.perms) | format(*domain).page_bits);
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
        if (valid(*domain, block)) {
            return E_BUSY; // an existing table or block leaf owns this range
        }
        write_entry(*domain, l2, l2_index(iova), pa | leaf_attrs(*domain, map.perms) | format(*domain).block_bits);
    }
    return E_OK;
}

void clear_pages(IommuDomain* domain, u64 iova, u64 length) {
    for (u64 off = 0; off < length; off += kPageBytes) {
        const u64 addr = iova + off;
        u64& l1_entry = table_ptr(domain->root_table_)[l1_index(addr)];
        if (!is_table(*domain, l1_entry)) {
            continue;
        }
        u64& l2_entry = table_ptr(address(*domain, l1_entry))[l2_index(addr)];
        if (!is_table(*domain, l2_entry)) {
            continue;
        }
        write_entry(*domain, address(*domain, l2_entry), l3_index(addr), 0);
    }
}

void clear_blocks(IommuDomain* domain, u64 iova, u64 length) {
    for (u64 off = 0; off < length; off += kIommuBlockBytes) {
        const u64 addr = iova + off;
        u64& l1_entry = table_ptr(domain->root_table_)[l1_index(addr)];
        if (!is_table(*domain, l1_entry)) {
            continue;
        }
        u64& l2_entry = table_ptr(address(*domain, l1_entry))[l2_index(addr)];
        if (valid(*domain, l2_entry) && !is_table(*domain, l2_entry)) {
            write_entry(*domain, address(*domain, l1_entry), l2_index(addr), 0);
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
    if (!is_table(*domain, l1_entry)) {
        return;
    }
    const PhysAddr l2_pa = address(*domain, l1_entry);
    u64& l2_entry = table_ptr(l2_pa)[l2_index(addr)];
    if (is_table(*domain, l2_entry)) {
        const PhysAddr l3_pa = address(*domain, l2_entry);
        if (table_empty(*domain, l3_pa)) {
            write_entry(*domain, l2_pa, l2_index(addr), 0);
            retire_table(domain, l3_pa);
        }
    }
    if (table_empty(*domain, l2_pa)) {
        write_entry(*domain, domain->root_table_, l1_index(addr), 0);
        retire_table(domain, l2_pa);
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
SysError check_extent(const IommuDomain& domain, const IommuMap& map) {
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
    if (iova_end > (1ull << domain.backend_.iova_bits)) {
        return E_INVALID;
    }
    return E_OK;
}

SysError check_mapping(const IommuDomain& domain, const IommuMap& map) {
    if (map.reserved != 0 || (map.pa & (kPageBytes - 1))) {
        return E_INVALID;
    }
    if (map.perms == 0 || (map.perms & ~kPermMask)) {
        return E_INVALID;
    }
    const SysError rc = check_extent(domain, map);
    if (rc != E_OK) {
        return rc;
    }
    u64 pa_end = 0;
    if (__builtin_add_overflow(map.pa, map.length, &pa_end)) {
        return E_OVERFLOW;
    }
    if (pa_end > (1ull << domain.backend_.pa_bits)) return E_INVALID;
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

u32 find_record(IommuDomain* domain, u64 iova, u64 length) {
    for (u32 i = 0; i < domain->mapping_count_; ++i) {
        const IommuMapping& record = domain->mappings_[i];
        if (record.valid && record.iova == iova && record.length == length) {
            return i;
        }
    }
    return kNoIndex;
}

SysError invalidate_range(IommuDomain& domain, u64 iova, u64 length) {
    if (!domain.backend_.invalidate_iotlb_range) return E_OK;
    const auto rc = domain.backend_.invalidate_iotlb_range(domain.backend_.ctx, &domain, iova, length);
    if (rc == E_OK) counter_inc(length == ~0ull ? IOMMU_TLBI_DOMAIN : IOMMU_TLBI_RANGE);
    return rc;
}
SysError sync(IommuDomain& domain) {
    return domain.backend_.sync_command_stream ?
        domain.backend_.sync_command_stream(domain.backend_.ctx) : E_OK;
}

SysError publish_records(IommuDomain& domain, u32 start, bool invalidate) {
    auto rc = publish_dirty(domain);
    if (rc != E_OK) return rc;
    if (invalidate) {
        for (u32 r = start; r < domain.mapping_count_; ++r) {
            const auto& m = domain.mappings_[r];
            rc = invalidate_range(domain, m.iova, m.length);
            if (rc != E_OK) return rc;
        }
    }
    return sync(domain);
}

SysError rollback(IommuDomain& domain, u32 start, SysError error) {
    // Even a coherent walker can observe partial writes before publication.
    // Unlink tables, publish the removals and synchronise before freeing them.
    for (u32 r = start; r < domain.mapping_count_; ++r) {
        const auto& m = domain.mappings_[r];
        clear_record(&domain, m);
        prune_range(&domain, m.iova, m.length);
    }
    if (publish_records(domain, start, true) != E_OK) {
        quarantine(domain);
        return E_FAULT;
    }
    release_retired(domain);
    for (u32 r = start; r < domain.mapping_count_; ++r) domain.pinned_pages_ -= domain.mappings_[r].pages;
    domain.mapping_count_ = start;
    return error;
}

void ensure_initialised() {
    if (__atomic_load_n(&g_initialised, __ATOMIC_ACQUIRE)) {
        return;
    }
    LockGuard guard(g_stream_lock);
    if (__atomic_load_n(&g_initialised, __ATOMIC_ACQUIRE)) {
        return;
    }
    libk::memset(g_streams, 0, sizeof(g_streams));
    IommuDomain& quarantine = g_domains[kQuarantineIndex];
    libk::memset(&quarantine, 0, sizeof(quarantine));
    quarantine.index_ = kQuarantineIndex;
    quarantine.state_ = DomainState::Quarantined;
    quarantine.in_use_ = true;
    __atomic_store_n(&g_initialised, true, __ATOMIC_RELEASE);
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

SysError register_backend(IommuRoute route, const IommuBackendOps& backend) {
    if (!valid_backend(backend) || !backend.hardware || !backend.ready(backend.ctx) || !route.stream_count ||
        static_cast<u64>(route.first_stream) + route.stream_count > (1ull << 32)) return E_INVALID;
    LockGuard guard(g_stream_lock);
    for (u32 i = 0; i < g_route_count; ++i) {
        const auto& r = g_routes[i].route;
        if (r.segment == route.segment && static_cast<u64>(r.first_stream) <
            static_cast<u64>(route.first_stream) + route.stream_count &&
            static_cast<u64>(route.first_stream) < static_cast<u64>(r.first_stream) + r.stream_count)
            return E_BUSY;
    }
    if (g_route_count == kMaxIommuBackends) return E_OVERFLOW;
    g_routes[g_route_count++] = {route, backend};
    return E_OK;
}

const IommuBackendOps* backend_for(IommuDeviceId device) {
    LockGuard guard(g_stream_lock);
    for (u32 i = 0; i < g_route_count; ++i) {
        const auto& r = g_routes[i].route;
        if (r.segment == device.segment && device.requester_or_stream_id >= r.first_stream &&
            static_cast<u64>(device.requester_or_stream_id) < static_cast<u64>(r.first_stream) + r.stream_count)
            return g_routes[i].ops.ready(g_routes[i].ops.ctx) ? &g_routes[i].ops : nullptr;
    }
    return nullptr;
}

SysError domain_map_batch(IommuDomain& domain, const IommuMap* maps, u32 count) {
    if (!maps || !count) return E_INVALID;
    LockGuard guard(domain.lock_);
    if (domain_state(domain) != DomainState::Active) return E_PERM;
    if (count > kMaxDomainMappings - domain.mapping_count_) return E_OVERFLOW;

    // Complete validation before any descriptor write. Existing records retain
    // their pins even when a previous hardware operation was uncertain.
    u64 new_pages = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto& m = maps[i];
        auto rc = check_mapping(domain, m);
        if (rc != E_OK) return rc;
        u64 owner = 0;
        VmDmaClass cls = VmDmaClass::None;
        rc = validate_ownership(&domain, m, &owner, &cls);
        if (rc != E_OK) return rc;
        for (u32 j = 0; j < domain.mapping_count_; ++j) {
            const auto& r = domain.mappings_[j];
            if (m.iova < r.iova + r.length && r.iova < m.iova + m.length) return E_BUSY;
        }
        for (u32 j = 0; j < i; ++j) {
            if (m.iova < maps[j].iova + maps[j].length && maps[j].iova < m.iova + m.length) return E_BUSY;
        }
        new_pages += m.length / kPageBytes;
    }
    if (new_pages > 0xFFFF'FFFFull - domain.pinned_pages_) return E_OVERFLOW;

    const u32 start = domain.mapping_count_;
    for (u32 i = 0; i < count; ++i) {
        const auto& map = maps[i];
        PageOwnership ownership{};
        // Owners must keep the authorization stable under this cold operation.
        if (!domain.owner_.query(domain.owner_.ctx, map.pa, &ownership))
            return rollback(domain, start, E_PERM);
        auto& record = domain.mappings_[domain.mapping_count_++];
        record = {map.iova, map.pa, map.length, ownership.object, map.perms,
                  static_cast<u32>(map.length / kPageBytes), ownership.dma_class,
                  is_block_map(map), true, {}};
        domain.pinned_pages_ += record.pages;
        const auto rc = record.block ? emit_blocks(&domain, map) : emit_pages(&domain, map);
        if (rc != E_OK) return rollback(domain, start, rc);
    }
    // Cache publication is required for existing tables too. Invalidate the
    // changed ranges (including cached walk state), then synchronise once.
    if (publish_records(domain, start, true) != E_OK) {
        quarantine(domain);
        return E_FAULT;
    }
    for (u32 i = 0; i < count; ++i) {
        counter_inc(IOMMU_MAP_OPS);
        counter_add(IOMMU_MAP_BYTES, maps[i].length);
        if (is_block_map(maps[i])) counter_add(IOMMU_BLOCK_MAP_BYTES, maps[i].length);
    }
    return E_OK;
}

SysError domain_unmap_batch(IommuDomain& domain, const IommuMap* maps, u32 count) {
    if (!maps || !count) return E_INVALID;
    LockGuard guard(domain.lock_);
    if (!domain.in_use_ || domain.index_ == kQuarantineIndex) return E_PERM;
    if (count > domain.mapping_count_) return E_NOTFOUND;
    // Preflight the complete batch; duplicates or missing records cannot cause
    // a partial release. Exact ranges are intentional, keeping pins auditable.
    for (u32 i = 0; i < count; ++i) {
        const auto rc = check_extent(domain, maps[i]);
        if (rc != E_OK) return rc;
        if (find_record(&domain, maps[i].iova, maps[i].length) == kNoIndex) return E_NOTFOUND;
        for (u32 j = 0; j < i; ++j) if (maps[i].iova == maps[j].iova) return E_INVALID;
    }
    for (u32 i = 0; i < count; ++i) {
        const auto& r = domain.mappings_[find_record(&domain, maps[i].iova, maps[i].length)];
        clear_record(&domain, r);
        prune_range(&domain, r.iova, r.length);
    }
    auto rc = publish_dirty(domain);
    if (rc == E_OK) {
        for (u32 i = 0; i < count && rc == E_OK; ++i)
            rc = invalidate_range(domain, maps[i].iova, maps[i].length);
    }
    if (rc != E_OK || sync(domain) != E_OK) {
        quarantine(domain);
        return E_FAULT;
    }
    release_retired(domain);
    for (u32 i = 0; i < count; ++i) {
        const u32 idx = find_record(&domain, maps[i].iova, maps[i].length);
        const auto record = domain.mappings_[idx];
        if (domain.owner_.release) domain.owner_.release(domain.owner_.ctx, record.pa, record.length);
        domain.pinned_pages_ -= record.pages;
        counter_inc(IOMMU_UNMAP_OPS);
        domain.mappings_[idx] = domain.mappings_[--domain.mapping_count_];
    }
    return E_OK;
}

SysError domain_sync(IommuDomain& domain) {
    LockGuard guard(domain.lock_);
    if (!domain.in_use_) return E_INVALID;
    const auto rc = sync(domain);
    if (rc != E_OK) quarantine(domain);
    return rc;
}

void domain_quarantine(IommuDomain& domain) {
    LockGuard guard(domain.lock_);
    if (domain.in_use_ && domain_state(domain) != DomainState::Quarantined) quarantine(domain);
}

SysError domain_attach(IommuDomain& domain, IommuDeviceId device) {
    ensure_initialised();
    LockGuard streams(g_stream_lock);
    LockGuard state(domain.lock_); // lifecycle lock order: streams -> domain
    if (!domain.in_use_ || (domain_state(domain) != DomainState::Active &&
        domain.index_ != kQuarantineIndex)) return E_PERM;
    // A software model cannot replace a real hardware context. Routing also
    // rejects a hardware backend serving a different controller/namespace.
    const IommuBackendOps* route = nullptr;
    for (u32 i = 0; i < g_route_count; ++i) {
        const auto& r = g_routes[i].route;
        if (r.segment == device.segment && device.requester_or_stream_id >= r.first_stream &&
            static_cast<u64>(device.requester_or_stream_id) < static_cast<u64>(r.first_stream) + r.stream_count)
            route = &g_routes[i].ops;
    }
    if (domain.backend_.hardware && (!route || !route->ready(route->ctx) || route->ctx != domain.backend_.ctx ||
        route->attach_stream != domain.backend_.attach_stream)) return E_PERM;
    if (route && !domain.backend_.hardware && domain.index_ != kQuarantineIndex) return E_PERM;
    u32 slot = kNoIndex;
    for (u32 i = 0; i < kMaxIommuStreams; ++i) {
        const auto& b = g_streams[i];
        if (b.used && b.segment == device.segment && b.sid == device.requester_or_stream_id) {
            slot = i;
            if (b.domain_index == domain.index_) return E_OK;
            // Quarantined metadata does not prove that the old STE denies DMA.
            if (b.domain_index != kQuarantineIndex) return E_BUSY;
            break;
        }
        if (!b.used && slot == kNoIndex) slot = i;
    }
    if (slot == kNoIndex) return E_OVERFLOW;
    auto rc = E_OK;
    if (domain.index_ == kQuarantineIndex && route) {
        rc = route->detach_stream(route->ctx, device);
        if (rc == E_OK) rc = route->sync_command_stream(route->ctx);
    } else {
        if (!program_stream(&domain, device)) rc = E_FAULT;
        if (rc == E_OK) rc = sync(domain);
    }
    if (rc != E_OK) {
        // The STE may have been partly installed. Keep its binding and table
        // lifetime so destroy can retry deny + completion before reclamation.
        g_streams[slot] = {device.segment, device.requester_or_stream_id, domain.index_, true, {}};
        __atomic_or_fetch(&domain.attached_mask_, 1ull << slot, __ATOMIC_RELAXED);
        quarantine(domain);
        return E_FAULT;
    }
    g_streams[slot] = {device.segment, device.requester_or_stream_id, domain.index_, true, {}};
    __atomic_or_fetch(&domain.attached_mask_, 1ull << slot, __ATOMIC_RELAXED);
    return E_OK;
}

SysError domain_detach(IommuDomain& domain, IommuDeviceId device) {
    ensure_initialised();
    LockGuard streams(g_stream_lock);
    LockGuard state(domain.lock_);
    for (u32 i = 0; i < kMaxIommuStreams; ++i) {
        auto& b = g_streams[i];
        if (!b.used || b.segment != device.segment || b.sid != device.requester_or_stream_id) continue;
        if (b.domain_index != domain.index_) return E_NOTFOUND;
        const auto rc = domain.backend_.detach_stream ?
            domain.backend_.detach_stream(domain.backend_.ctx, device) : E_OK;
        if (rc != E_OK || sync(domain) != E_OK) {
            quarantine(domain);
            return E_FAULT; // binding and pins retained until denial is proven
        }
        b.domain_index = kQuarantineIndex;
        __atomic_and_fetch(&domain.attached_mask_, ~(1ull << i), __ATOMIC_RELAXED);
        counter_inc(IOMMU_QUARANTINES);
        return E_OK;
    }
    return E_NOTFOUND;
}

// --- accessors -------------------------------------------------------------
u32 domain_index(const IommuDomain& domain) {
    return domain.index_;
}

DomainState domain_state(const IommuDomain& domain) {
    DomainState state;
    __atomic_load(&domain.state_, &state, __ATOMIC_ACQUIRE);
    return state;
}

bool domain_is_active(const IommuDomain& domain) {
    return domain_state(domain) == DomainState::Active;
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
    if (!valid_backend(backend)) return E_INVALID;
    ensure_initialised();
    LockGuard guard(g_stream_lock);
    for (u32 i = 0; i < kMaxIommuDomains; ++i) {
        // A quarantined slot is never handed out again: even if a teardown
        // released in_use_, the faults that put it here must not be able to
        // reuse it. This is belt-and-braces on top of the in_use_ check.
        if (i == kQuarantineIndex || g_domains[i].in_use_ ||
            domain_state(g_domains[i]) == DomainState::Quarantined) {
            continue;
        }
        IommuDomain& domain = g_domains[i];
        LockGuard state(domain.lock_);
        domain.index_ = i;
        domain.mapping_count_ = domain.table_count_ = domain.pinned_pages_ = 0;
        domain.root_table_ = 0;
        __atomic_store_n(&domain.attached_mask_, 0ull, __ATOMIC_RELAXED);
        domain.in_use_ = true;
        set_state(domain, DomainState::Active);
        domain.authorized_object_ = authorized_object;
        domain.owner_ = owner;
        domain.backend_ = backend;
        if (!domain.backend_.table_format) domain.backend_.table_format = &kArmStage2Tables;
        if (!domain.backend_.iova_bits) domain.backend_.iova_bits = kIommuIovaBits;
        if (!domain.backend_.pa_bits) domain.backend_.pa_bits = 48;
        const PhysAddr root = alloc_table(&domain);
        if (!root) {
            domain.in_use_ = false;
            set_state(domain, DomainState::Free);
            return E_NOMEM;
        }
        domain.root_table_ = root;
        if (publish_dirty(domain) != E_OK) {
            // This root was never reachable by any stream; no device can hold
            // a walk reference, so failed initial publication is reclaimable.
            mm::free_page(root);
            domain.table_count_ = 0;
            domain.root_table_ = 0;
            domain.in_use_ = false;
            set_state(domain, DomainState::Free);
            return E_FAULT;
        }
        *out = &domain;
        return E_OK;
    }
    return E_OVERFLOW;
}

void domain_destroy(IommuDomain* domain) {
    if (!domain) return;
    ensure_initialised();
    LockGuard streams(g_stream_lock);
    LockGuard state(domain->lock_);
    if (!domain->in_use_ || domain->index_ == kQuarantineIndex) return;
    // First remove every hardware reference to the root. A failed detach keeps
    // the binding intact so later teardown retries it rather than freeing it.
    for (u32 i = 0; i < kMaxIommuStreams; ++i) {
        auto& b = g_streams[i];
        if (!b.used || b.domain_index != domain->index_) continue;
        const IommuDeviceId device{b.segment, b.sid};
        const auto rc = domain->backend_.detach_stream ?
            domain->backend_.detach_stream(domain->backend_.ctx, device) : E_OK;
        if (rc != E_OK || sync(*domain) != E_OK) { quarantine(*domain); return; }
        b.domain_index = kQuarantineIndex;
        __atomic_and_fetch(&domain->attached_mask_, ~(1ull << i), __ATOMIC_RELAXED);
    }
    for (u32 r = 0; r < domain->mapping_count_; ++r) clear_record(domain, domain->mappings_[r]);
    if (publish_dirty(*domain) != E_OK || invalidate_range(*domain, 0, ~0ull) != E_OK ||
        sync(*domain) != E_OK) { quarantine(*domain); return; }
    for (u32 r = 0; r < domain->mapping_count_; ++r) {
        const auto& record = domain->mappings_[r];
        if (domain->owner_.release) domain->owner_.release(domain->owner_.ctx, record.pa, record.length);
        counter_inc(IOMMU_UNMAP_OPS);
    }
    for (u32 t = 0; t < domain->table_count_; ++t) mm::free_page(domain->tables_[t]);
    domain->mapping_count_ = domain->table_count_ = domain->pinned_pages_ = 0;
    domain->root_table_ = 0;
    domain->in_use_ = false;
    set_state(*domain, DomainState::Free);
    // Never memset a live lock. Creation resets the free record under the
    // stream/lifecycle lock after this guard has released it.
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
