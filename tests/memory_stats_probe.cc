// Real allocator paths; substitute only unrelated process/object boundaries.
#include "../kernel/mm/phys.cc"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" { char __text_start[1]{}; char __kernel_end[1]{}; }
namespace libk { void* memset(void* p, int v, usize n) { return std::memset(p, v, n); } }
namespace sched {
bool process_charge_page(u32) { return true; }
void process_uncharge_page(u32) {}
object::Ref<Process> find_process(u32) { return nullptr; }
u64 process_identity(const Process&) { return 0; }
}
namespace object { void release(Record* r) { assert(!r); } }
[[noreturn]] void panic(const char*) { std::abort(); }
int main() {
    constexpr PhysAddr base = 0x10000000000; // outside host executable mappings
    mm::phys_init(base, 64 * 1024 * 1024);
    assert(mm::total_page_count() == 16384 && mm::allocated_page_count() == 512);
    mm::reserve_phys_range(base, 2 * 1024 * 1024); // repeat boot reservation
    assert(mm::allocated_page_count() == 512);
    mm::reserve_phys_range(base + 2 * 1024 * 1024 - 1, 2); // overlapping partial pages
    assert(mm::allocated_page_count() == 513);
    const auto single = mm::alloc_page();
    const auto run = mm::alloc_pages_contiguous(7);
    const auto aligned = mm::alloc_pages_contiguous_aligned(16, 16);
    assert(single && run && aligned && !(aligned % (16 * PAGE_SIZE)));
    assert(mm::allocated_page_count() == 537);
    mm::free_page(single);
    for (unsigned i = 0; i < 7; ++i) mm::free_page(run + i * PAGE_SIZE);
    for (unsigned i = 0; i < 16; ++i) mm::free_page(aligned + i * PAGE_SIZE);
    assert(mm::allocated_page_count() == 513);
    std::vector<PhysAddr> pages;
    while (const auto p = mm::alloc_page()) pages.push_back(p);
    assert(pages.size() == 16384 - 513);
    assert(mm::allocated_page_count() == mm::total_page_count());
    assert(!mm::alloc_pages_contiguous(1));
    for (const auto p : pages) mm::free_page(p);
    assert(mm::allocated_page_count() == 513);
    mm::phys_init(base, 8ull * 1024 * 1024 * 1024); // capacity is managed RAM, not raw DT size
    assert(mm::total_page_count() == 1024 * 1024 && mm::allocated_page_count() == 512);
}
