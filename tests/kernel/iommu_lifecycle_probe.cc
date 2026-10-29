// tests/kernel/iommu_lifecycle_probe.cc — host regression for appliance-death
// IOMMU quarantine (Phase K, spec §8.10/§8.12).
//
// Compiles kernel/iommu/core.cc against stubbed physical-page and memset
// services and proves the lifecycle invariants the appliance depends on:
//
//   (a) a cleanly revocable domain returns its slot to the pool (reusable);
//   (b) a domain that faults into quarantine is never handed out again while
//       its pages are still pinned, and its teardown releases nothing until the
//       backend can prove the requester stopped;
//   (c) a detached requester is redirected to the faulting (quarantine) domain,
//       never to a bypass;
//   (d) once the backend is provably quiesced the slot is reclaimed exactly
//       once.
//
// This complements tests/iommu_probe.cc (hostile-DMA containment): here the
// focus is the pool-reuse / never-reuse lifecycle an appliance reset relies on.

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

// --- page-owner + backend models -------------------------------------------
namespace {
constexpr u64 kOwner = 0x4242;

PhysAddr g_owned_page = 0;

bool owner_query(void*, PhysAddr pa, iommu::PageOwnership* out) {
    if (pa != g_owned_page) {
        return false;
    }
    out->object = kOwner;
    out->dma_class = iommu::VmDmaClass::ApplianceRam;
    std::memset(out->pad, 0, sizeof(out->pad));
    return true;
}

int g_release_calls = 0;
void owner_release(void*, PhysAddr, u64) { ++g_release_calls; }

bool g_sync_fail = false;
SysError be_sync(void*) { return g_sync_fail ? E_FAULT : E_OK; }

iommu::IommuOwnerOps owner_ops() {
    iommu::IommuOwnerOps ops{};
    ops.query = owner_query;
    ops.release = owner_release;
    ops.ctx = nullptr;
    return ops;
}

iommu::IommuBackendOps backend_ops() {
    iommu::IommuBackendOps ops{};
    ops.sync_command_stream = be_sync;
    ops.ctx = nullptr;
    return ops;
}

constexpr u32 kPermRW =
    static_cast<u32>(iommu::DmaPerm::Read) | static_cast<u32>(iommu::DmaPerm::Write);

iommu::IommuMap make_map(u64 iova, PhysAddr pa, u64 length, u32 perms) {
    iommu::IommuMap m{};
    m.iova = iova;
    m.pa = pa;
    m.length = length;
    m.perms = perms;
    return m;
}

iommu::IommuDomain* create_domain(u32* out_index) {
    iommu::IommuDomain* d = nullptr;
    assert(iommu::domain_create(kOwner, owner_ops(), backend_ops(), &d) == E_OK);
    assert(d);
    if (out_index) *out_index = iommu::domain_index(*d);
    return d;
}
} // namespace

int main() {
    auto* page = static_cast<u8*>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    assert(page);
    g_owned_page = reinterpret_cast<PhysAddr>(page);

    // (a) A cleanly revocable domain returns its slot to the pool.
    {
        u32 idx0 = 0;
        iommu::IommuDomain* first = create_domain(&idx0);
        iommu::domain_destroy(first);
        u32 idx1 = 0;
        iommu::IommuDomain* second = create_domain(&idx1);
        assert(idx1 == idx0 && "a cleanly destroyed domain slot must be reusable");
        iommu::domain_destroy(second);
    }

    // (b) A quarantined domain is never handed out again while it holds pinned
    //     pages, and its teardown releases nothing until the backend is provable.
    u32 quarantined_index = 0;
    {
        iommu::IommuDomain* d = create_domain(&quarantined_index);
        const iommu::IommuMap mm =
            make_map(0x6000'0000, g_owned_page, PAGE_SIZE, kPermRW);
        assert(iommu::domain_map_batch(*d, &mm, 1) == E_OK);

        const int releases0 = g_release_calls;
        const u64 quarantines0 = iommu::debug_counter(iommu::IOMMU_QUARANTINES);

        g_sync_fail = true; // the backend cannot prove the device stopped
        assert(iommu::domain_unmap_batch(*d, &mm, 1) == E_FAULT);
        assert(iommu::domain_state(*d) == iommu::DomainState::Quarantined);
        assert(iommu::domain_pinned_pages(*d) == 1);
        assert(g_release_calls == releases0); // page stays pinned
        assert(iommu::debug_counter(iommu::IOMMU_QUARANTINES) > quarantines0);

        // Force-quarantined teardown (appliance death): still nothing released.
        iommu::domain_destroy(d);
        assert(g_release_calls == releases0);
        assert(iommu::domain_state(*d) == iommu::DomainState::Quarantined);

        // The pool must not recycle the quarantined slot: fill every remaining
        // slot and prove exhaustion rather than aliasing the faulted context.
        iommu::IommuDomain* filler[iommu::kMaxIommuDomains];
        u32 filled = 0;
        for (;;) {
            iommu::IommuDomain* f = nullptr;
            const SysError rc = iommu::domain_create(kOwner, owner_ops(), backend_ops(), &f);
            if (rc != E_OK) {
                assert(rc == E_OVERFLOW);
                break;
            }
            assert(iommu::domain_index(*f) != quarantined_index &&
                   "a quarantined domain must never be reused");
            filler[filled++] = f;
            assert(filled < iommu::kMaxIommuDomains);
        }
        // kMaxIommuDomains - quarantine slot - the one pinned slot.
        assert(filled == iommu::kMaxIommuDomains - 2);

        // A quarantined context can never accept new translations.
        const iommu::IommuMap again =
            make_map(0x6000'0000, g_owned_page, PAGE_SIZE, kPermRW);
        assert(iommu::domain_map_batch(*d, &again, 1) == E_PERM);

        // (d) Once the backend is provably quiesced the slot is reclaimed once.
        g_sync_fail = false;
        iommu::domain_destroy(d);
        assert(g_release_calls == releases0 + 1);
        u32 reclaimed = 0;
        iommu::IommuDomain* reused = create_domain(&reclaimed);
        assert(reclaimed == quarantined_index && "a provably-quiesced slot is reclaimed");
        iommu::domain_destroy(reused);

        for (u32 i = 0; i < filled; ++i) {
            iommu::domain_destroy(filler[i]);
        }
    }

    // (c) A detached requester is redirected to the faulting domain, never to a
    //     bypass; the quarantine domain refuses every mapping.
    {
        iommu::IommuDomain* d = create_domain(nullptr);
        const iommu::IommuDeviceId dev{0, 42};
        assert(iommu::domain_attach(*d, dev) == E_OK);
        assert(iommu::stream_domain(dev) == d);
        assert(iommu::domain_detach(*d, dev) == E_OK);
        assert(iommu::stream_domain(dev) == iommu::quarantine_domain());
        const iommu::IommuMap qm =
            make_map(0x6000'0000, g_owned_page, PAGE_SIZE, kPermRW);
        assert(iommu::domain_map_batch(*iommu::quarantine_domain(), &qm, 1) == E_PERM);
        iommu::domain_destroy(d);
    }

    std::free(page);
    assert(g_mm_pages.empty() && "page-table pages leaked");
    std::printf("iommu lifecycle probe ok\n");
    return 0;
}
