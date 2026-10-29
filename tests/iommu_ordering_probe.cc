// Exercise real core publication, rollback and revocation against a cached
// device walker and injected allocator/backend failures (ASan/UBSan).
#include "anko/iommu.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace {
std::vector<PhysAddr> pages;
std::map<PhysAddr, std::array<u64, 512>> visible;
std::vector<iommu::IommuMap> invalidations;
int allocation_budget = -1;
bool publish_fail, sync_fail, attach_fail, detach_fail, ready = true;
u32 publishes, syncs, releases;
PhysAddr device_roots[256];
constexpr u64 kOwner = 123;
}
namespace mm {
PhysAddr alloc_page() {
    if (!allocation_budget) return 0;
    if (allocation_budget > 0) --allocation_budget;
    const auto pa = reinterpret_cast<PhysAddr>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    assert(pa); pages.push_back(pa); return pa;
}
void free_page(PhysAddr pa) {
    for (auto i = pages.begin(); i != pages.end(); ++i) if (*i == pa) {
        for (auto root : device_roots) assert(root != pa && "free of a live stream root");
        pages.erase(i); visible.erase(pa); std::free(reinterpret_cast<void*>(pa)); return;
    }
    assert(false);
}
}
namespace libk { void* memset(void* p, int value, usize n) { return std::memset(p, value, n); } }
namespace {
PhysAddr walk(PhysAddr root, u64 iova) {
    if (!root) return 0;
    const u64 l1 = visible.at(root)[(iova >> 30) & 511];
    if (!(l1 & 1)) return 0;
    const u64 l2 = visible.at(l1 & iommu::kArmStage2Tables.address_mask)[(iova >> 21) & 511];
    if (!(l2 & 1)) return 0;
    if (!(l2 & 2)) return (l2 & iommu::kArmStage2Tables.address_mask) + (iova & ((1ull << 21) - 1));
    return visible.at(l2 & iommu::kArmStage2Tables.address_mask)[(iova >> 12) & 511] & iommu::kArmStage2Tables.address_mask;
}
bool query(void*, PhysAddr pa, iommu::PageOwnership* out) {
    if (pa < 0x200000 || pa >= 0x400000) return false;
    *out = {kOwner, iommu::DmaClass::NativeDevice, {}}; return true;
}
void release(void*, PhysAddr, u64) { assert(!sync_fail); ++releases; }
SysError publish(void*, PhysAddr pa, u64 length) {
    assert(length == PAGE_SIZE); ++publishes;
    if (publish_fail) return E_FAULT;
    std::memcpy(visible[pa].data(), reinterpret_cast<void*>(pa), length); return E_OK;
}
SysError invalidate(void*, iommu::IommuDomain*, u64 iova, u64 length) {
    invalidations.push_back({iova, 0, length, 0, 0}); return E_OK;
}
SysError sync(void*) { ++syncs; return sync_fail ? E_FAULT : E_OK; }
SysError attach(void*, iommu::IommuDeviceId device, iommu::IommuDomain* domain) {
    device_roots[device.requester_or_stream_id] = domain->root_table_;
    return attach_fail ? E_FAULT : E_OK; // models a partially published STE
}
SysError detach(void*, iommu::IommuDeviceId device) {
    if (detach_fail) return E_FAULT;
    device_roots[device.requester_or_stream_id] = 0; return E_OK;
}
bool is_ready(void*) { return ready; }
iommu::IommuBackendOps backend() {
    return {publish, invalidate, sync, attach, detach, nullptr,
            &iommu::kArmStage2Tables, 39, 48, true, is_ready};
}
iommu::IommuDomain* create(iommu::IommuBackendOps ops = backend()) {
    iommu::IommuDomain* domain = nullptr;
    assert(iommu::domain_create(kOwner, {query, release, nullptr}, ops, &domain) == E_OK);
    return domain;
}
iommu::IommuMap map(u64 iova, PhysAddr pa = 0x200000, u64 length = PAGE_SIZE) {
    return {iova, pa, length, 3, 0};
}
}

int main() {
    using namespace iommu;
    assert(!backend_for({0, 1}));
    auto incomplete = backend(); incomplete.sync_command_stream = nullptr;
    assert(register_backend({0, 0, 256}, incomplete) == E_INVALID);
    assert(register_backend({0, 0, 256}, backend()) == E_OK);
    assert(register_backend({0, 128, 64}, backend()) == E_BUSY);
    assert(!backend_for({0, 256}) && !backend_for({1, 1}));
    ready = false; assert(!backend_for({0, 1})); ready = true;

    publish_fail = true;
    IommuDomain* failed = reinterpret_cast<IommuDomain*>(1);
    assert(domain_create(kOwner, {query, release, nullptr}, backend(), &failed) == E_FAULT);
    assert(!failed && pages.empty()); publish_fail = false;

    auto* d = create();
    const auto first = map(0x40000000), second = map(0x40001000, 0x201000);
    assert(domain_map_batch(*d, &first, 1) == E_OK && walk(d->root_table_, first.iova) == first.pa);
    const auto p0 = publishes, s0 = syncs;
    assert(domain_map_batch(*d, &second, 1) == E_OK);
    assert(walk(d->root_table_, second.iova) == second.pa);
    assert(publishes == p0 + 1 && syncs == s0 + 1); // existing leaf table only
    const auto allocated = pages.size();
    IommuMap bad_batch[] = {map(0x50000000), map(0x50001000, 0x1000)};
    assert(domain_map_batch(*d, bad_batch, 2) == E_PERM);
    assert(pages.size() == allocated && domain_live_mappings(*d) == 2);
    auto high_pa = map(0x50000000, 1ull << 48);
    assert(domain_map_batch(*d, &high_pa, 1) == E_INVALID);
    IommuMap overlap[] = {map(0x50000000), map(0x50000000)};
    assert(domain_map_batch(*d, overlap, 2) == E_BUSY);
    IommuMap duplicate[] = {first, first};
    assert(domain_unmap_batch(*d, duplicate, 2) == E_INVALID);
    IommuMap missing[] = {first, map(0x60000000)};
    assert(domain_unmap_batch(*d, missing, 2) == E_NOTFOUND);
    assert(walk(d->root_table_, first.iova) == first.pa && releases == 0);
    IommuMap both[] = {first, second};
    const auto s1 = syncs;
    invalidations.clear();
    assert(domain_unmap_batch(*d, both, 2) == E_OK && syncs == s1 + 1);
    assert(releases == 2 && pages.size() == 1 && !walk(d->root_table_, first.iova));
    bool full_walk = false;
    for (const auto& extent : invalidations)
        full_walk |= extent.iova == 0x40000000 && extent.length == (1ull << 30);
    assert(full_walk && "retired L2 coverage must be invalidated before reuse");
    domain_destroy(d); assert(pages.empty());

    // Allocation failure after one leaf: unlink newly created parents and
    // publish/invalidate them before freeing. A failed rollback keeps pins.
    d = create(); allocation_budget = 2;
    auto crossing = map(0x1ff000, 0x200000, PAGE_SIZE * 2);
    assert(domain_map_batch(*d, &crossing, 1) == E_NOMEM);
    assert(domain_is_active(*d) && !domain_live_mappings(*d) && pages.size() == 1);
    allocation_budget = 2; publish_fail = true;
    assert(domain_map_batch(*d, &crossing, 1) == E_FAULT);
    assert(domain_state(*d) == DomainState::Quarantined && domain_pinned_pages(*d) == 2);
    const auto retained = pages.size();
    domain_destroy(d); assert(pages.size() == retained);
    publish_fail = false; allocation_budget = -1; domain_destroy(d); assert(pages.empty());

    d = create(); publish_fail = true;
    const auto r0 = releases;
    assert(domain_map_batch(*d, &first, 1) == E_FAULT);
    assert(domain_pinned_pages(*d) == 1 && domain_live_mappings(*d) == 1 && releases == r0);
    publish_fail = false; domain_destroy(d); assert(pages.empty() && releases == r0 + 1);

    // Attach can fail after publishing a root. Failed detach must retain the
    // binding; a fresh domain cannot take it until denial is actually proven.
    d = create(); assert(domain_map_batch(*d, &first, 1) == E_OK);
    const IommuDeviceId device{0, 7}; attach_fail = true;
    assert(domain_attach(*d, device) == E_FAULT && stream_domain(device) == d);
    assert(domain_attached_mask(*d) && domain_state(*d) == DomainState::Quarantined);
    attach_fail = false; detach_fail = true;
    auto* rival = create();
    assert(domain_attach(*rival, device) == E_BUSY);
    assert(quarantine_stream(device) == E_FAULT && stream_domain(device) == d);
    const auto before = pages.size(); domain_destroy(d); assert(pages.size() == before);
    detach_fail = false;
    assert(quarantine_stream(device) == E_OK && stream_domain(device) == quarantine_domain());
    assert(!domain_attached_mask(*d));
    assert(domain_attach(*rival, device) == E_OK);
    domain_destroy(d); assert(device_roots[7] == rival->root_table_);
    domain_destroy(rival); assert(!device_roots[7] && pages.empty());

    // Denied stream slots are reusable, so repeated hotplug is bounded by live
    // bindings, not all requester IDs ever seen.
    d = create();
    for (u32 sid = 20; sid < 200; ++sid) {
        assert(domain_attach(*d, {0, sid}) == E_OK);
        assert(domain_detach(*d, {0, sid}) == E_OK);
    }
    domain_destroy(d); assert(pages.empty());
    auto narrow = backend(); narrow.iova_bits = 32;
    d = create(narrow); auto beyond = map(1ull << 32);
    assert(domain_map_batch(*d, &beyond, 1) == E_INVALID);
    domain_destroy(d); assert(pages.empty());
    std::puts("iommu ordering probe ok");
}
