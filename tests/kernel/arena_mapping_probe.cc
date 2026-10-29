// Exercise the production mapping tracker across every bitmap word.
#include "anko/mm.h"
#include <cassert>
#include <cstdlib>

namespace {
unsigned references;
auto* record = reinterpret_cast<object::Record*>(0x1000);
}
namespace object {
void retain(Record* value) {
    assert(value == record);
    ++references;
}
void release(Record* value) {
    if (value) {
        assert(value == record && references);
        --references;
    }
}
}
namespace mm {
void free_page_for_identity(u64, PhysAddr) { std::abort(); }
void slab_free(void*, usize, usize) { std::abort(); }
}
[[noreturn]] void panic(const char*) { std::abort(); }

int main() {
    ArenaTable table{};
    constexpr VirtAddr base = 0x20000000;
    constexpr u64 sizes[] = {1, 64, 65, 256};
    for (const u64 pages : sizes) {
        Arena arena{};
        arena.phys_base = 0x40000000;
        arena.size = pages * PAGE_SIZE;
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            assert(mm::arena_claim_va(table, arena, base, arena.size));
            mm::arena_retain_mapping(table, base, record);
            assert(references == 1 && table.count == 1);
            for (u64 page = pages; page-- > 1;) {
                mm::arena_unmap_page(table, base + page * PAGE_SIZE);
                mm::arena_unmap_page(table, base + page * PAGE_SIZE);
                assert(references == 1 && table.count == 1);
            }
            mm::arena_unmap_page(table, base);
            assert(references == 0 && table.count == 0);
        }
    }
}
