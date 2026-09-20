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
    assert(vm::stage2_map_page(s, 1ull << 39, 0x1000, RIGHT_READ) == E_INVALID);
    assert(vm::stage2_destroy(s) == E_OK);
    assert(pages.empty());
    delete s;
}
