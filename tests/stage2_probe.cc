#include "anko/vm.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

static std::unordered_set<PhysAddr> pages;
static int allocation_budget = -1;
namespace mm {
PhysAddr alloc_page() {
    if (allocation_budget == 0) return 0;
    if (allocation_budget > 0) --allocation_budget;
    auto p = reinterpret_cast<PhysAddr>(std::aligned_alloc(PAGE_SIZE, PAGE_SIZE));
    assert(p);
    pages.insert(p);
    return p;
}
void free_page(PhysAddr p) {
    assert(pages.erase(p) == 1);
    std::free(reinterpret_cast<void*>(p));
}
}
namespace vm { void metric_inc(VmDebugCounter) {} }
namespace libk { void* memset(void* p, int c, usize n) { return std::memset(p, c, n); } }

// Independent stage-2 long-descriptor decoding.  Deliberately duplicated from
// kernel/vm/stage2.cc so the probe proves the field encodings themselves rather
// than re-invoking the code under test.
namespace {
constexpr u64 kAddrMask = 0x0000'FFFF'FFFF'F000ull;
constexpr u64 kValid = 1ull << 0;
constexpr u64 kTable = 1ull << 1;
constexpr u64 kS2apRead = 1ull << 6;
constexpr u64 kS2apWrite = 1ull << 7;
constexpr u64 kShInner = 3ull << 8;
constexpr u64 kAf = 1ull << 10;
constexpr u64 kXn = 1ull << 54;
constexpr u64 kMemNormalWB = 0xFull << 2;
constexpr u64 kMemNormalNC = 0x3ull << 2;
constexpr u64 kMemDeviceNGnRE = 0x1ull << 2;

u64 index_of(u64 ipa, u32 level) {
    const u32 shift = (level == 1) ? 30 : (level == 2 ? 21 : 12);
    return (ipa >> shift) & 0x1FFull;
}

// Resolve the level-3 leaf descriptor for `ipa`, failing if any level is not a
// live table link.
u64 leaf_desc(const vm::Stage2AddressSpace* s, u64 ipa) {
    const u64* root = reinterpret_cast<const u64*>(s->root_table_phys);
    const u64 l1 = root[index_of(ipa, 1)];
    assert((l1 & (kValid | kTable)) == (kValid | kTable));
    const u64* l2 = reinterpret_cast<const u64*>(l1 & kAddrMask);
    const u64 l2e = l2[index_of(ipa, 2)];
    assert((l2e & (kValid | kTable)) == (kValid | kTable));
    const u64* l3 = reinterpret_cast<const u64*>(l2e & kAddrMask);
    return l3[index_of(ipa, 3)];
}
} // namespace

static void split_failure_preserves_mapping() {
    auto* s = new vm::Stage2AddressSpace{};
    assert(vm::stage2_create(s) == E_OK);
    constexpr u64 base = 0x40000000;
    constexpr PhysAddr backing = 0x200000;
    assert(vm::stage2_map_block(s, base, backing, RIGHT_READ, vm::Stage2MemType::NormalWB) == E_OK);
    // Leave one fewer free record than the net expansion of a block split.
    const u32 occupied = vm::kMaxStage2Mappings - (vm::kStage2BlockPages - 2);
    for (u32 i = 1; i < occupied; ++i) {
        assert(vm::stage2_map_page(s, base + vm::kStage2BlockSize + i * PAGE_SIZE, 0x1000,
                                   RIGHT_READ) == E_OK);
    }
    const auto records = s->mapped_records;
    const auto covered = s->mapped_pages;
    const auto tables = pages.size();
    assert(vm::stage2_unmap_page(s, base + PAGE_SIZE) == E_OVERFLOW);
    assert(s->mapped_records == records && s->mapped_pages == covered);
    assert(pages.size() == tables);
    for (u32 i = 0; i < vm::kStage2BlockPages; ++i) {
        assert(vm::stage2_lookup_page(s, base + i * PAGE_SIZE, nullptr) == backing + i * PAGE_SIZE);
    }
    // Free exactly one slot: the split now has its net 511-record budget.
    assert(vm::stage2_unmap_page(s, base + vm::kStage2BlockSize + (occupied - 1) * PAGE_SIZE) ==
           E_OK);
    assert(vm::stage2_unmap_page(s, base + PAGE_SIZE) == E_OK);
    assert(s->mapped_records == vm::kMaxStage2Mappings - 1);
    assert(s->mapped_pages == covered - 2);
    assert(vm::stage2_lookup_page(s, base + PAGE_SIZE, nullptr) == 0);
    assert(leaf_desc(s, base) ==
           (backing | kValid | kTable | kAf | kShInner | kMemNormalWB | kS2apRead | kXn));
    assert(vm::stage2_lookup_page(s, base + 2 * PAGE_SIZE, nullptr) == backing + 2 * PAGE_SIZE);
    assert(vm::stage2_destroy(s) == E_OK);
    delete s;
    assert(pages.empty());
}

static void block_teardown_and_coverage() {
    auto* s = new vm::Stage2AddressSpace{};
    assert(vm::stage2_create(s) == E_OK);
    constexpr u64 base = 0x40000000;
    constexpr u64 ram_bytes = 512ull * 1024 * 1024;
    constexpr PhysAddr backing = 0x200000;
    for (u64 offset = 0; offset < ram_bytes; offset += vm::kStage2BlockSize) {
        assert(vm::stage2_map_block(s, base + offset, backing + offset, RIGHT_READ | RIGHT_EXEC,
                                    vm::Stage2MemType::NormalWB) == E_OK);
    }
    assert(s->mapped_records == ram_bytes / vm::kStage2BlockSize);
    assert(s->mapped_pages == ram_bytes / PAGE_SIZE);
    assert(vm::stage2_has_capacity(s, vm::kMaxStage2Mappings - s->mapped_records));
    assert(!vm::stage2_has_capacity(s, UINT64_MAX));
    const auto records = s->mapped_records;
    s->mapped_records = vm::kMaxStage2Mappings + 1;
    assert(!vm::stage2_has_capacity(s, 1));
    s->mapped_records = records;
    assert(vm::stage2_lookup_page(s, base + ram_bytes - PAGE_SIZE, nullptr) ==
           backing + ram_bytes - PAGE_SIZE);
    // Independent L2 decoding: block attributes and PA, not just soft metadata.
    const auto* root = reinterpret_cast<const u64*>(s->root_table_phys);
    const auto* l2 = reinterpret_cast<const u64*>(root[index_of(base, 1)] & kAddrMask);
    assert(l2[index_of(base, 2)] == (backing | kValid | kAf | kShInner | kMemNormalWB | kS2apRead));
    assert(vm::stage2_validate_no_overlap(s, base - PAGE_SIZE, 2 * PAGE_SIZE) == E_BUSY);
    const auto tables = pages.size();
    allocation_budget = 0;
    assert(vm::stage2_map_page(s, base + PAGE_SIZE, 0x1000, RIGHT_WRITE) == E_BUSY);
    assert(vm::stage2_unmap_page(s, base + PAGE_SIZE) == E_NOMEM);
    assert(s->mapped_records == records && pages.size() == tables);
    assert(vm::stage2_unmap_range(s, base + PAGE_SIZE, vm::kStage2BlockSize) == E_INVALID);
    assert(s->mapped_records == records && pages.size() == tables);
    // Removing complete blocks needs neither new table pages nor free records.
    assert(vm::stage2_unmap_range(s, base, ram_bytes) == E_OK);
    assert(s->mapped_records == 0 && s->mapped_pages == 0 && s->next_level_table_count == 0);
    allocation_budget = -1;

    constexpr u64 bar_bytes = 8 * vm::kStage2BlockSize;
    for (u64 off = 0; off < bar_bytes; off += vm::kStage2BlockSize) {
        assert(vm::stage2_map_block(s, base + off, backing + off, RIGHT_READ | RIGHT_WRITE,
                                    vm::Stage2MemType::Device) == E_OK);
    }
    constexpr u64 other = base + bar_bytes;
    const auto device_records = s->mapped_records;
    const auto ram_pages = vm::kMaxStage2Mappings - device_records;
    for (u64 i = 0; i < ram_pages; ++i) {
        assert(vm::stage2_map_page(s, other + i * PAGE_SIZE, 0x1000, RIGHT_READ) == E_OK);
    }
    assert(!vm::stage2_has_capacity(s, 1));
    assert(vm::stage2_activate(s) == E_OK);
    allocation_budget = 0;
    assert(vm::stage2_unmap_range(s, base, bar_bytes) == E_INVALID);
    assert(vm::stage2_revoke_device_range(s, base, bar_bytes + PAGE_SIZE) == E_PERM);
    assert(s->mapped_records == vm::kMaxStage2Mappings);
    assert(vm::stage2_revoke_device_range(s, base, bar_bytes) == E_OK);
    assert(s->mapped_records == ram_pages && s->mapped_pages == ram_pages);
    assert(vm::stage2_lookup_page(s, base, nullptr) == 0);
    assert(vm::stage2_lookup_page(s, other, nullptr) == 0x1000);
    assert(vm::stage2_revoke_device_range(s, base, bar_bytes) == E_OK); // idempotent
    allocation_budget = -1;
    assert(vm::stage2_destroy(s) == E_OK);
    delete s;
    assert(pages.empty());
}

int main() {
    split_failure_preserves_mapping();
    block_teardown_and_coverage();
    auto* s = new vm::Stage2AddressSpace{};
    assert(vm::stage2_create(s) == E_OK);
    const auto baseline = pages.size();
    for (int limit = 0; limit < 2; ++limit) {
        allocation_budget = limit;
        assert(vm::stage2_map_page(s, 0x40000000, 0x1000, RIGHT_READ) == E_NOMEM);
        assert(pages.size() == baseline && s->mapped_pages == 0 && s->next_level_table_count == 0);
    }
    allocation_budget = -1;
    for (unsigned i = 0; i < 1000; ++i) {
        const u64 ipa = 0x40000000ull + (static_cast<u64>(i) << 21);
        assert(vm::stage2_map_page(s, ipa, 0x1000, RIGHT_READ) == E_OK);
        assert(vm::stage2_map_page(s, ipa, 0x2000, RIGHT_WRITE) == E_BUSY);
        assert(vm::stage2_lookup_page(s, ipa, nullptr) == 0x1000);
        assert(vm::stage2_unmap_page(s, ipa) == E_OK);
        assert(pages.size() == baseline && s->next_level_table_count == 0);
    }

    // A trap owns an unmapped IPA range; neither bulk validation nor a direct
    // leaf insertion may turn it into RAM/MMIO. Failure allocates no tables.
    constexpr u64 ecam = 0x3f000000;
    assert(vm::stage2_reserve_trap(s, ecam, 1 << 20) == E_OK);
    assert(vm::stage2_lookup_page(s, ecam, nullptr) == 0);
    assert(vm::stage2_validate_no_overlap(s, ecam - PAGE_SIZE, 2 * PAGE_SIZE) == E_BUSY);
    assert(vm::stage2_map_page(s, ecam, 0x1000, RIGHT_READ) == E_BUSY);
    assert(vm::stage2_map_page(s, ecam + (1 << 20) - PAGE_SIZE, 0x1000, RIGHT_READ) == E_BUSY);
    assert(pages.size() == baseline && s->next_level_table_count == 0);
    assert(vm::stage2_reserve_trap(s, ecam, PAGE_SIZE) == E_BUSY);
    assert(vm::stage2_reserve_trap(s, (1ull << 39) - PAGE_SIZE, 2 * PAGE_SIZE) == E_INVALID);
    for (unsigned i = 1; i < 8; ++i)
        assert(vm::stage2_reserve_trap(s, ecam + (i << 20), 1 << 20) == E_OK);
    assert(vm::stage2_reserve_trap(s, ecam + (8 << 20), 1 << 20) == E_OVERFLOW);
    for (unsigned i = 0; i < 8; ++i)
        vm::stage2_release_trap(s, ecam + (i << 20), 1 << 20);
    assert(vm::stage2_map_page(s, ecam, 0x1000, RIGHT_READ) == E_OK);
    assert(vm::stage2_reserve_trap(s, ecam, 1 << 20) == E_BUSY);
    assert(vm::stage2_unmap_page(s, ecam) == E_OK);

    // --- stage-2 memory attribute classes -----------------------------------
    // The default mapping class must remain plain NormalWB with no XN when the
    // guest may execute from it: identical to the pre-existing RAM behaviour.
    const u64 wb_ipa = 0x40000000ull;
    const u64 nc_ipa = 0x40001000ull;
    const u64 dev_ipa = 0x40002000ull;
    const u64 wb2_ipa = 0x40003000ull;
    assert(vm::stage2_map_page_ex(s, wb_ipa, 0x10000, RIGHT_READ | RIGHT_EXEC,
                                  vm::Stage2MemType::NormalWB) == E_OK);
    assert(leaf_desc(s, wb_ipa) ==
           (0x10000ull | kValid | kTable | kAf | kShInner | kMemNormalWB | kS2apRead));

    // NormalNC: same access, normal non-cacheable, execute-never (no EXEC).
    assert(vm::stage2_map_page_ex(s, nc_ipa, 0x11000, RIGHT_READ, vm::Stage2MemType::NormalNC) ==
           E_OK);
    assert(leaf_desc(s, nc_ipa) ==
           (0x11000ull | kValid | kTable | kAf | kShInner | kMemNormalNC | kS2apRead | kXn));

    // Device: Device-nGnRE and execute-never even though the caller asked for
    // RIGHT_EXEC in the S2AP rights.
    assert(vm::stage2_map_page_ex(s, dev_ipa, 0x12000, RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC,
                                  vm::Stage2MemType::Device) == E_OK);
    assert(leaf_desc(s, dev_ipa) == (0x12000ull | kValid | kTable | kAf | kShInner |
                                     kMemDeviceNGnRE | kS2apRead | kS2apWrite | kXn));

    // The lookup path reports the recorded class alongside rights and PA.
    u32 rights = 0;
    vm::Stage2MemType type = vm::Stage2MemType::NormalWB;
    assert(vm::stage2_lookup_page(s, dev_ipa, &rights, &type) == 0x12000);
    assert(type == vm::Stage2MemType::Device);
    assert(rights == (RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC));
    assert(vm::stage2_lookup_page(s, nc_ipa, &rights, &type) == 0x11000);
    assert(type == vm::Stage2MemType::NormalNC);

    // stage2_map_page is a NormalWB wrapper: identical leaf encoding.
    assert(vm::stage2_map_page(s, wb2_ipa, 0x13000, RIGHT_READ) == E_OK);
    type = vm::Stage2MemType::Device;
    assert(vm::stage2_lookup_page(s, wb2_ipa, nullptr, &type) == 0x13000);
    assert(type == vm::Stage2MemType::NormalWB);
    assert(leaf_desc(s, wb2_ipa) ==
           (0x13000ull | kValid | kTable | kAf | kShInner | kMemNormalWB | kS2apRead | kXn));

    // Device bits survive unmap/remap, and a remap may change the class: the
    // record and the leaf descriptor both track the newest mapping.
    assert(vm::stage2_unmap_page(s, dev_ipa) == E_OK);
    type = vm::Stage2MemType::NormalWB;
    assert(vm::stage2_lookup_page(s, dev_ipa, nullptr, &type) == 0);
    assert(vm::stage2_map_page_ex(s, dev_ipa, 0x14000, RIGHT_READ, vm::Stage2MemType::Device) ==
           E_OK);
    assert(leaf_desc(s, dev_ipa) ==
           (0x14000ull | kValid | kTable | kAf | kShInner | kMemDeviceNGnRE | kS2apRead | kXn));
    type = vm::Stage2MemType::NormalWB;
    assert(vm::stage2_lookup_page(s, dev_ipa, nullptr, &type) == 0x14000);
    assert(type == vm::Stage2MemType::Device);

    assert(vm::stage2_unmap_page(s, dev_ipa) == E_OK);
    assert(vm::stage2_map_page_ex(s, dev_ipa, 0x15000, RIGHT_READ, vm::Stage2MemType::NormalWB) ==
           E_OK);
    assert(leaf_desc(s, dev_ipa) ==
           (0x15000ull | kValid | kTable | kAf | kShInner | kMemNormalWB | kS2apRead | kXn));
    type = vm::Stage2MemType::Device;
    assert(vm::stage2_lookup_page(s, dev_ipa, nullptr, &type) == 0x15000);
    assert(type == vm::Stage2MemType::NormalWB);

    // Unknown classes are rejected before any table is walked or allocated.
    const auto tables_before = s->next_level_table_count;
    assert(vm::stage2_map_page_ex(s, 0x42000000, 0x1000, RIGHT_READ,
                                  static_cast<vm::Stage2MemType>(99)) == E_INVALID);
    assert(s->next_level_table_count == tables_before);

    assert(vm::stage2_unmap_page(s, wb_ipa) == E_OK);
    assert(vm::stage2_unmap_page(s, nc_ipa) == E_OK);
    assert(vm::stage2_unmap_page(s, dev_ipa) == E_OK);
    assert(vm::stage2_unmap_page(s, wb2_ipa) == E_OK);
    assert(s->mapped_pages == 0 && s->next_level_table_count == 0);

    assert(vm::stage2_map_page_ex(s, dev_ipa, 0x12000, RIGHT_READ | RIGHT_WRITE,
                                  vm::Stage2MemType::Device) == E_OK);
    assert(vm::stage2_map_page(s, wb_ipa, 0x13000, RIGHT_READ) == E_OK);
    assert(vm::stage2_activate(s) == E_OK);
    assert(vm::stage2_unmap_page(s, dev_ipa) == E_INVALID);
    assert(vm::stage2_revoke_device_page(s, wb_ipa) == E_PERM);
    assert(vm::stage2_revoke_device_page(s, dev_ipa) == E_OK);
    assert(vm::stage2_lookup_page(s, dev_ipa, nullptr) == 0);
    assert(vm::stage2_lookup_page(s, wb_ipa, nullptr) == 0x13000);

    assert(vm::stage2_map_page(s, 1ull << 39, 0x1000, RIGHT_READ) == E_INVALID);
    assert(vm::stage2_destroy(s) == E_OK);
    assert(pages.empty());
    delete s;
}
