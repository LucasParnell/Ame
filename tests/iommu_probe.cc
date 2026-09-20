// tests/iommu_probe.cc — host regression for the backend-independent IOMMU core.
//
// Compiles kernel/iommu/core.cc against stubbed physical-page and memset
// services, then proves the isolation semantics required by AME GPU DRIVER
// APPLIANCE SPEC §8.3–8.6, §8.10, §8.15:
//
//   (a) mapping a page the caller does not own fails;
//   (b) domain teardown cannot release still-reachable pages;
//   (c) a VmDmaClass::None page never maps;
//   (d) IOVA overflow/bounds are rejected;
//   (e) the debug counters move, including block-mapping bytes;
// plus permission minimality, attach/detach redirection to the quarantine
// domain, and that an unbound requester resolves to no domain (so a backend
// faults it instead of bypassing).

#include "anko/iommu.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// --- kernel memory + memset stubs ------------------------------------------
namespace {
std::vector<PhysAddr> g_mm_pages;
} // namespace

namespace mm {
PhysAddr alloc_page() {
    auto* p = static_cast<u8*>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    assert(p);
    std::memset(p, 0, PAGE_SIZE);
    g_mm_pages.push_back(reinterpret_cast<PhysAddr>(p));
    return reinterpret_cast<PhysAddr>(p);
}
void free_page(PhysAddr pa) {
    for (auto it = g_mm_pages.begin(); it != g_mm_pages.end(); ++it) {
        if (*it != pa) {
            continue;
        }
        g_mm_pages.erase(it);
        std::free(reinterpret_cast<void*>(pa));
        return;
    }
    assert(false && "free_page of an unknown page");
}
} // namespace mm

namespace libk {
void* memset(void* dst, int c, usize n) {
    return std::memset(dst, c, n);
}
} // namespace libk

// --- page-owner model -------------------------------------------------------
namespace {
struct OwnedRange {
    PhysAddr base;
    u64 length;
    u64 object;
    iommu::VmDmaClass cls;
};

constexpr u64 kOwner = 0x1111;
constexpr u64 kOther = 0x2222;

OwnedRange g_ranges[16];
u32 g_range_count = 0;

bool owner_query(void*, PhysAddr pa, iommu::PageOwnership* out) {
    for (u32 i = 0; i < g_range_count; ++i) {
        const OwnedRange& r = g_ranges[i];
        if (pa >= r.base && pa < r.base + r.length) {
            out->object = r.object;
            out->dma_class = r.cls;
            std::memset(out->pad, 0, sizeof(out->pad));
            return true;
        }
    }
    return false;
}

int g_release_calls = 0;
u64 g_release_bytes = 0;
PhysAddr g_last_release_pa = 0;
u64 g_last_release_len = 0;

void owner_release(void*, PhysAddr pa, u64 length) {
    ++g_release_calls;
    g_release_bytes += length;
    g_last_release_pa = pa;
    g_last_release_len = length;
}

// --- backend model ----------------------------------------------------------
bool g_sync_fail = false;
bool g_invalidated_since_release_check = false;
int g_publish_calls = 0;
int g_invalidate_calls = 0;
u64 g_last_invalidate_iova = 0;
u64 g_last_invalidate_len = 0;

SysError be_publish(void*, PhysAddr, u64) {
    ++g_publish_calls;
    return E_OK;
}

SysError be_invalidate(void*, iommu::IommuDomain*, u64 iova, u64 length) {
    ++g_invalidate_calls;
    g_last_invalidate_iova = iova;
    g_last_invalidate_len = length;
    g_invalidated_since_release_check = true;
    return E_OK;
}

SysError be_sync(void*) {
    return g_sync_fail ? E_FAULT : E_OK;
}

iommu::IommuOwnerOps owner_ops() {
    iommu::IommuOwnerOps ops{};
    ops.query = owner_query;
    ops.release = owner_release;
    ops.ctx = nullptr;
    return ops;
}

iommu::IommuBackendOps backend_ops() {
    iommu::IommuBackendOps ops{};
    ops.publish_page_table_range = be_publish;
    ops.invalidate_iotlb_range = be_invalidate;
    ops.sync_command_stream = be_sync;
    ops.ctx = nullptr;
    return ops;
}

u32 constexpr kPermRW =
    static_cast<u32>(iommu::DmaPerm::Read) | static_cast<u32>(iommu::DmaPerm::Write);

iommu::IommuMap make_map(u64 iova, PhysAddr pa, u64 length, u32 perms) {
    iommu::IommuMap m{};
    m.iova = iova;
    m.pa = pa;
    m.length = length;
    m.perms = perms;
    return m;
}

// Descriptor accessors — the core's root table and per-domain mapping records
// are plain data, so the probe can inspect what the core actually installed.
constexpr u64 kDescValid = 1ull << 0;
constexpr u64 kDescTable = 1ull << 1;
constexpr u64 kDescApRead = 1ull << 6;
constexpr u64 kDescApWrite = 1ull << 7;
constexpr u64 kAddrMask = 0x0000'FFFF'FFFF'F000ull;

u64* table_ptr(PhysAddr pa) {
    return reinterpret_cast<u64*>(pa);
}

u64 page_leaf(iommu::IommuDomain* d, u64 iova) {
    const u64 l1 = table_ptr(d->root_table_)[(iova >> 30) & 0x1FFu];
    assert((l1 & kDescValid) && (l1 & kDescTable));
    const u64 l2 = table_ptr(l1 & kAddrMask)[(iova >> 21) & 0x1FFu];
    assert((l2 & kDescValid) && (l2 & kDescTable));
    return table_ptr(l2 & kAddrMask)[(iova >> 12) & 0x1FFu];
}

u64 block_leaf(iommu::IommuDomain* d, u64 iova) {
    const u64 l1 = table_ptr(d->root_table_)[(iova >> 30) & 0x1FFu];
    assert((l1 & kDescValid) && (l1 & kDescTable));
    return table_ptr(l1 & kAddrMask)[(iova >> 21) & 0x1FFu];
}
} // namespace

int main() {
    // Driver-owned guest memory: one owned page, one owned read-only-visible
    // page, one page owned by another principal, one None-class page, one page
    // owned by nobody, and one 2 MiB block backing store.
    auto* page_owned = static_cast<u8*>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    auto* page_other = static_cast<u8*>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    auto* page_none = static_cast<u8*>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    auto* page_unowned = static_cast<u8*>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    auto* block = static_cast<u8*>(std::aligned_alloc(2u * 1024 * 1024, 2u * 1024 * 1024));
    assert(page_owned && page_other && page_none && page_unowned && block);

    const PhysAddr pa_owned = reinterpret_cast<PhysAddr>(page_owned);
    const PhysAddr pa_other = reinterpret_cast<PhysAddr>(page_other);
    const PhysAddr pa_none = reinterpret_cast<PhysAddr>(page_none);
    const PhysAddr pa_unowned = reinterpret_cast<PhysAddr>(page_unowned);
    const PhysAddr pa_block = reinterpret_cast<PhysAddr>(block);

    g_ranges[g_range_count++] = {pa_owned, PAGE_SIZE, kOwner, iommu::VmDmaClass::ApplianceRam};
    g_ranges[g_range_count++] = {pa_other, PAGE_SIZE, kOther, iommu::VmDmaClass::ApplianceRam};
    g_ranges[g_range_count++] = {pa_none, PAGE_SIZE, kOwner, iommu::VmDmaClass::None};
    g_ranges[g_range_count++] = {pa_block, 2u * 1024 * 1024, kOwner,
                                 iommu::VmDmaClass::ApplianceRam};
    // page_unowned is intentionally left unregistered.

    iommu::IommuDomain* d = nullptr;
    assert(iommu::domain_create(kOwner, owner_ops(), backend_ops(), &d) == E_OK);
    assert(d && d->is_active() && d->root_table_ != 0);

    // (a) Ownership is proven by the query, never by the numeric PA.
    {
        const iommu::IommuMap foreign = make_map(0x4000'0000, pa_other, PAGE_SIZE, kPermRW);
        const iommu::IommuMap unknown = make_map(0x4000'0000, pa_unowned, PAGE_SIZE, kPermRW);
        assert(d->map_batch(&foreign, 1) == E_PERM);
        assert(d->map_batch(&unknown, 1) == E_PERM);
        assert(d->live_mappings() == 0 && d->pinned_pages() == 0);
    }

    // (c) A None-class page is never reachable by a device.
    {
        const iommu::IommuMap none_map = make_map(0x4000'0000, pa_none, PAGE_SIZE, kPermRW);
        assert(d->map_batch(&none_map, 1) == E_INVALID);
        assert(d->live_mappings() == 0);
    }

    // (d) IOVA arithmetic overflow and out-of-range addresses are rejected.
    {
        const iommu::IommuMap wrap =
            make_map(~0ull - (PAGE_SIZE - 1), pa_owned, PAGE_SIZE, kPermRW);
        assert(d->map_batch(&wrap, 1) == E_OVERFLOW);
        const iommu::IommuMap beyond =
            make_map(1ull << iommu::kIommuIovaBits, pa_owned, PAGE_SIZE, kPermRW);
        assert(d->map_batch(&beyond, 1) == E_INVALID);
        const iommu::IommuMap misaligned = make_map(0x4000'0000, pa_owned + 1, PAGE_SIZE, kPermRW);
        assert(d->map_batch(&misaligned, 1) == E_INVALID);
    }

    // (e) Counters move, and installed translations carry the right permissions.
    {
        const u64 map_ops0 = iommu::debug_counter(iommu::IOMMU_MAP_OPS);
        const u64 map_bytes0 = iommu::debug_counter(iommu::IOMMU_MAP_BYTES);
        const u64 block_bytes0 = iommu::debug_counter(iommu::IOMMU_BLOCK_MAP_BYTES);
        const u64 range0 = iommu::debug_counter(iommu::IOMMU_TLBI_RANGE);

        // A read-only mapping must stay read-only: no silent upgrade.
        const iommu::IommuMap ro =
            make_map(0x4000'0000, pa_owned, PAGE_SIZE, static_cast<u32>(iommu::DmaPerm::Read));
        assert(d->map_batch(&ro, 1) == E_OK);
        const u64 leaf = page_leaf(d, 0x4000'0000);
        assert((leaf & kDescApRead) && !(leaf & kDescApWrite));
        assert((leaf & kAddrMask) == pa_owned);
        assert(d->pinned_pages() == 1);

        // A 2 MiB-aligned read/write run is installed as block descriptors.
        const iommu::IommuMap blk = make_map(0x4020'0000, pa_block, 2u * 1024 * 1024, kPermRW);
        assert(d->map_batch(&blk, 1) == E_OK);
        const u64 be = block_leaf(d, 0x4020'0000);
        assert((be & kDescValid) && !(be & kDescTable));
        assert((be & kAddrMask) == pa_block);
        assert(d->pinned_pages() == 1 + 512);

        assert(iommu::debug_counter(iommu::IOMMU_MAP_OPS) == map_ops0 + 2);
        assert(iommu::debug_counter(iommu::IOMMU_MAP_BYTES) ==
               map_bytes0 + PAGE_SIZE + 2u * 1024 * 1024);
        assert(iommu::debug_counter(iommu::IOMMU_BLOCK_MAP_BYTES) ==
               block_bytes0 + 2u * 1024 * 1024);
        assert(g_publish_calls > 0);

        // Unmap releases exactly the recorded range, after invalidation.
        g_invalidated_since_release_check = false;
        assert(d->unmap_batch(&blk, 1) == E_OK);
        assert(g_invalidated_since_release_check);
        assert(g_invalidate_calls > 0);
        assert(g_last_invalidate_iova == 0x4020'0000);
        assert(g_last_invalidate_len == 2u * 1024 * 1024);
        assert(g_last_release_pa == pa_block && g_last_release_len == 2u * 1024 * 1024);
        assert(iommu::debug_counter(iommu::IOMMU_TLBI_RANGE) == range0 + 1);
        assert(d->pinned_pages() == 1);

        // The cleared IOVA can be remapped with a different backing page.
        assert(d->unmap_batch(&ro, 1) == E_OK);
        const iommu::IommuMap fresh = make_map(0x4000'0000, pa_owned, PAGE_SIZE, kPermRW);
        assert(d->map_batch(&fresh, 1) == E_OK);
        // Unmap is matched by IOVA/length only, so an unmap entry need not
        // restate permissions.
        const iommu::IommuMap fresh_unmap = make_map(0x4000'0000, 0, PAGE_SIZE, 0);
        assert(d->unmap_batch(&fresh_unmap, 1) == E_OK);
        assert(d->live_mappings() == 0 && d->pinned_pages() == 0);
    }

    // (b) Teardown cannot release still-reachable pages.
    {
        iommu::IommuDomain* q = nullptr;
        assert(iommu::domain_create(kOwner, owner_ops(), backend_ops(), &q) == E_OK);
        const iommu::IommuMap mm = make_map(0x5000'0000, pa_owned, PAGE_SIZE, kPermRW);
        assert(q->map_batch(&mm, 1) == E_OK);

        const int releases0 = g_release_calls;
        const u64 quarantines0 = iommu::debug_counter(iommu::IOMMU_QUARANTINES);

        g_sync_fail = true; // the backend cannot prove the device stopped
        assert(q->unmap_batch(&mm, 1) == E_FAULT);
        assert(g_release_calls == releases0); // page stays pinned
        assert(q->pinned_pages() == 1);
        assert(q->state() == iommu::DomainState::Quarantined);

        // Destruct while still unprovable: still nothing released, domain kept.
        iommu::domain_destroy(q);
        assert(g_release_calls == releases0);
        assert(iommu::debug_counter(iommu::IOMMU_QUARANTINES) > quarantines0);

        // Once the device is provably stopped, teardown releases and frees.
        g_sync_fail = false;
        iommu::domain_destroy(q);
        assert(g_release_calls == releases0 + 1);
        assert(g_last_release_pa == pa_owned && g_last_release_len == PAGE_SIZE);
    }

    // Attach/detach redirection and the permanent quarantine domain.
    {
        const iommu::IommuDeviceId dev{0, 42};
        const iommu::IommuDeviceId dev2{0, 43};
        assert(d->attach(dev) == E_OK);
        assert(d->attach(dev) == E_OK); // redundant
        assert(d->attach(dev2) == E_OK);
        assert(iommu::stream_domain(dev) == d);

        iommu::IommuDomain* rival = nullptr;
        assert(iommu::domain_create(kOwner, owner_ops(), backend_ops(), &rival) == E_OK);
        assert(rival->attach(dev) == E_BUSY); // one active domain per requester

        assert(d->detach(dev) == E_OK);
        assert(iommu::stream_domain(dev) == iommu::quarantine_domain());
        assert(d->detach(dev) == E_NOTFOUND);

        // The quarantine domain is empty and never maps.
        const iommu::IommuMap qm = make_map(0x4000'0000, pa_owned, PAGE_SIZE, kPermRW);
        assert(iommu::quarantine_domain()->map_batch(&qm, 1) == E_PERM);
        assert(iommu::quarantine_domain()->live_mappings() == 0);

        // A requester may be bound to the quarantine domain directly.
        const iommu::IommuDeviceId dev3{0, 99};
        assert(iommu::quarantine_domain()->attach(dev3) == E_OK);
        assert(iommu::stream_domain(dev3) == iommu::quarantine_domain());

        // An unbound requester resolves to no domain, so the backend faults it.
        assert(iommu::stream_domain(iommu::IommuDeviceId{7, 7}) == nullptr);
        assert(d->detach(dev2) == E_OK);
        iommu::domain_destroy(rival);
    }

    // The no-op backend installs and reclaims without hardware, and records
    // faults through the diagnostic entry point.
    {
        iommu::IommuDomain* dn = nullptr;
        assert(iommu::domain_create(kOwner, owner_ops(), iommu::null_backend(), &dn) == E_OK);
        const iommu::IommuMap mm = make_map(0x4000'0000, pa_owned, PAGE_SIZE, kPermRW);
        assert(dn->map_batch(&mm, 1) == E_OK);
        assert(dn->sync() == E_OK);
        const int releases0 = g_release_calls;
        assert(dn->unmap_batch(&mm, 1) == E_OK);
        assert(g_release_calls == releases0 + 1);
        iommu::domain_destroy(dn);

        const u64 faults0 = iommu::debug_counter(iommu::IOMMU_FAULTS);
        iommu::note_fault(iommu::IommuDeviceId{0, 9}, 0xdead'0000, 3);
        assert(iommu::debug_counter(iommu::IOMMU_FAULTS) == faults0 + 1);
        assert(iommu::last_fault().iova == 0xdead'0000);
    }

    // Globals never go global; unknown counter ids read as zero.
    assert(iommu::debug_counter(iommu::IOMMU_TLBI_GLOBAL) == 0);
    assert(iommu::debug_counter(0xFFFF) == 0);

    iommu::domain_destroy(d);

    std::free(page_owned);
    std::free(page_other);
    std::free(page_none);
    std::free(page_unowned);
    std::free(block);

    assert(g_mm_pages.empty() && "page-table pages leaked");
    // Every mapped page whose invalidation was proven was released exactly once.
    assert(g_release_calls == 5);
    assert(g_release_bytes == 2u * 1024 * 1024 + 4u * PAGE_SIZE);

    std::printf("iommu core probe ok\n");
    return 0;
}
