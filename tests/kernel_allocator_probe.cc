#define ANKO_HOST_TEST 1
#define ANKO_MANAGED_ALLOCATOR_TEST 1
#include "../kernel/mm/phys.cc"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" { char __text_start[1]{}; char __kernel_end[1]{}; }
namespace libk { void* memset(void* p, int c, usize n) { return std::memset(p, c, n); } }
namespace sched {
bool process_charge_page(u32) { return true; }
void process_uncharge_page(u32) {}
object::Ref<Process> find_process(u32) { return {}; }
u64 process_identity(const Process&) { return 0; }
}
namespace object { void release(Record* p) { assert(!p); } }
[[noreturn]] void panic(const char* message) { std::fprintf(stderr, "%s\n", message); std::_Exit(77); }
struct alignas(64) First { u64 value[8]; };
struct alignas(64) Second { u64 value[8]; };
int main(int argc, char** argv) {
    constexpr usize bytes = 64 * 1024 * 1024;
    auto* memory = std::aligned_alloc(PAGE_SIZE, bytes); assert(memory);
    mm::phys_init(reinterpret_cast<PhysAddr>(memory), bytes);
    mm::heap_init();
    auto* first = mm::allocate<First>(); auto* second = mm::allocate<Second>();
    assert(first && second && ((reinterpret_cast<usize>(first) ^ reinterpret_cast<usize>(second)) >> PAGE_SHIFT));
    assert(mm::kernel_heap_overlaps(reinterpret_cast<PhysAddr>(first), sizeof(First)));
    first->value[0] = 55; second->value[0] = 44;
    if (argc > 1) {
        if (!std::strcmp(argv[1], "type")) mm::deallocate<Second>(first);
        if (!std::strcmp(argv[1], "double")) {
            mm::deallocate<First>(first); mm::deallocate<First>(first);
        }
        if (!std::strcmp(argv[1], "overrun")) {
            reinterpret_cast<volatile u8*>(first)[sizeof(First)] = 0;
            mm::deallocate<First>(first);
        }
        if (!std::strcmp(argv[1], "stale")) {
            mm::deallocate<First>(first); first->value[0] = 1;
            for (int i = 0; i < 2048; ++i) { auto* p = mm::allocate<First>(); assert(p); mm::deallocate<First>(p); }
        }
        return 1;
    }
    mm::deallocate<First>(first);
    auto* reused = mm::allocate<First>();
    assert(reused != first && !reused->value[0] && second->value[0] == 44);
    mm::deallocate<First>(reused); mm::deallocate<Second>(second);
    auto* raw = mm::slab_alloc(37, 16); assert(raw);
    mm::slab_free(raw, 37, 16);
    std::free(memory);
    std::puts("kernel managed allocator isolated");
}
