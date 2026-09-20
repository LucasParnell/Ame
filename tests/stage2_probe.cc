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

int main() {
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

    assert(vm::stage2_map_page(s, 1ull << 39, 0x1000, RIGHT_READ) == E_INVALID);
    assert(vm::stage2_destroy(s) == E_OK);
    assert(pages.empty());
    delete s;
}
