// Production AMM + stage-1 VM + object lifetime + ADMA, with allocator and CPU
// rendezvous substitution. Descriptor observations enforce break-before-make.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>
#include "anko/amm.h"
#include "anko/adma.h"
#include "anko/cap.h"
#include "../kernel/mm/vm_space.cc"

HwInfo g_hw{};
extern "C" {
alignas(4096) char __text_start[4096]{};
char __text_end[1]{};
alignas(4096) char __rodata_start[4096]{};
char __rodata_end[1]{};
char __kernel_end[1]{};
}
namespace {
std::unordered_set<PhysAddr> physical;
std::unordered_set<void*> metadata;
std::unordered_set<PhysAddr> owned;
std::unordered_map<PhysAddr, PhysAddr> allocation_base;
std::unordered_map<PhysAddr, usize> allocation_remaining;
bool contiguous_mode;
int page_budget = -1, slab_budget = -1;
bool can_stop = true, stopped;
u64 copies_before;
struct Watch { VmSpace space; VirtAddr va; u64 previous; bool broken; };
std::vector<Watch> watches;
}
namespace smp {
bool rendezvous() { assert(!stopped); stopped = can_stop; return stopped; }
void resume() { assert(stopped); stopped = false; }
}
namespace mm {
PhysAddr alloc_page() {
    if (!page_budget) return 0;
    if (page_budget > 0) --page_budget;
    void* p = std::aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    assert(p); std::memset(p, 0, PAGE_SIZE);
    auto pa = reinterpret_cast<PhysAddr>(p); assert(physical.insert(pa).second); return pa;
}
PhysAddr alloc_page_for_identity(u64) {
    const auto pa = alloc_page(); if (pa) owned.insert(pa); return pa;
}
PhysAddr alloc_pages_contiguous_for_identity(u64, usize count) {
    if (!contiguous_mode || (page_budget >= 0 && usize(page_budget) < count)) return 0;
    if (page_budget >= 0) page_budget -= count;
    void* p = std::aligned_alloc(PAGE_SIZE, count * PAGE_SIZE);
    assert(p);
    const auto base = reinterpret_cast<PhysAddr>(p);
    allocation_remaining[base] = count;
    for (usize i = 0; i < count; ++i) {
        const auto pa = base + i * PAGE_SIZE;
        assert(physical.insert(pa).second); owned.insert(pa); allocation_base[pa] = base;
    }
    return base;
}
void free_page(PhysAddr pa) {
    assert(physical.erase(pa) == 1);
    auto found = allocation_base.find(pa);
    if (found == allocation_base.end()) { std::free(reinterpret_cast<void*>(pa)); return; }
    const auto base = found->second;
    allocation_base.erase(found);
    if (!--allocation_remaining.at(base)) {
        allocation_remaining.erase(base); std::free(reinterpret_cast<void*>(base));
    }
}
void free_page_for(u32, PhysAddr pa) { free_page(pa); }
void free_page_for_identity(u64, PhysAddr pa) { assert(owned.erase(pa) == 1); free_page(pa); }
void* slab_alloc(usize size, usize) {
    if (!slab_budget) return nullptr;
    if (slab_budget > 0) --slab_budget;
    void* p = std::calloc(1, size); assert(p); metadata.insert(p); return p;
}
void slab_free(void* p, usize, usize) {
    if (p) { assert(metadata.erase(p) == 1); std::free(p); }
}
}
namespace sched {
object::Ref<Process> find_process(u32) { return {}; }
}
[[noreturn]] void panic(const char* message) { std::fprintf(stderr, "%s\n", message); std::abort(); }
extern "C" void anko_vm_test_tlbi() {
    for (auto& watch : watches) {
        const auto pte = mm::vm_debug_pte(watch.space, watch.va);
        if (!(pte & 1)) watch.broken = true;
        else {
            // AF 0->1 is an architecturally permitted update without BBM.
            if ((watch.previous & 1) && ((pte ^ watch.previous) & ~(1ull << 10))) assert(watch.broken);
            watch.previous = pte; watch.broken = false;
        }
    }
}
namespace {
constexpr VirtAddr base = 0x20000000, alias = 0x20010000, moved = 0x20020000;
constexpr u32 rw = RIGHT_READ | RIGHT_WRITE;
Arena* new_arena(u64 size, u64 capacity, u64 type = 1) {
    Arena* a = nullptr;
    const MxArenaCreate request{size, capacity, base, 0x1000000, type, 0, 0};
    assert(amm::create(1, 1, request, &a) == E_OK);
    assert(object::create(a, amm::destroy)); return a;
}
object::Record* record(Arena* a) { return object::acquire_existing(a); }
void map(Arena* a, VmSpace space, VirtAddr va, u32 rights = rw) {
    auto* ref = record(a); assert(ref);
    assert(amm::map(*a, ref, space, va, rights) == E_OK); object::release(ref);
    for (u64 i = 0; i < amm::inspect(*a).capacity; i += PAGE_SIZE)
        watches.push_back({space, va + i, mm::vm_debug_pte(space, va + i), false});
}
PhysAddr address(VmSpace space, VirtAddr va) {
    PhysAddr pa; assert(mm::user_va_to_pa(space, va, RIGHT_READ, &pa)); return pa;
}
void write(VmSpace space, VirtAddr va, u8 value) {
    PhysAddr pa;
    if (!mm::user_va_to_pa(space, va, RIGHT_WRITE, &pa)) {
        assert(amm::fault(space, va, true));
        if (!mm::user_va_to_pa(space, va, RIGHT_WRITE, &pa))
            std::fprintf(stderr, "write retry pid=%u va=%llx value=%u pte=%llx\n", space.pid,
                         static_cast<unsigned long long>(va), value,
                         static_cast<unsigned long long>(mm::vm_debug_pte(space, va)));
        assert(mm::user_va_to_pa(space, va, RIGHT_WRITE, &pa));
    }
    *reinterpret_cast<u8*>(pa) = value;
}
u8 read(VmSpace space, VirtAddr va) {
    if (!(mm::vm_debug_pte(space, va) & (1ull << 10))) assert(amm::fault(space, va, false));
    return *reinterpret_cast<u8*>(address(space, va));
}
void drop_creator(Arena* a) {
    auto* ref = record(a); assert(ref); object::release(ref); object::release(ref);
}
struct TestDma {
    Arena* arena;
    VmSpace writer;
    VirtAddr va;
    PhysAddr source, destination;
    bool complete, cancel_ok, inject_write, fail;
    unsigned submissions, fences;
};
SysError dma_submit(void* context, PhysAddr destination, PhysAddr source, usize size, u64* cookie) {
    auto& dma = *static_cast<TestDma*>(context);
    assert(size == PAGE_SIZE && amm::inspect(*dma.arena).state != MX_ARENA_STABLE);
    dma.source = source; dma.destination = destination; *cookie = ++dma.submissions;
    if (dma.inject_write) {
        dma.inject_write = false;
        const auto previous = *reinterpret_cast<u8*>(source);
        write(dma.writer, dma.va, 100);
        assert(*reinterpret_cast<u8*>(source) == previous); // immutable DMA snapshot
        assert(read(dma.writer, dma.va) == 100); // writer progressed during the actual copy round
    }
    return E_OK;
}
SysError dma_poll(void* context, u64) {
    auto& dma = *static_cast<TestDma*>(context);
    if (dma.fail) return E_FAULT;
    if (!dma.complete) return E_BUSY;
    adma::copy_page(dma.destination, dma.source); return E_OK;
}
SysError dma_cancel(void* context, u64) {
    auto& dma = *static_cast<TestDma*>(context);
    ++dma.fences;
    return dma.cancel_ok ? E_OK : E_BUSY;
}
SysError external_fence(void* context) { return *static_cast<bool*>(context) ? E_OK : E_BUSY; }
}
int main() {
    VmSpace first{mm::alloc_page(), 1}, second{mm::alloc_page(), 2};
    auto* a = new_arena(2 * PAGE_SIZE, 3 * PAGE_SIZE);
    map(a, first, base); map(a, second, alias); map(a, second, moved, RIGHT_READ);
    write(first, base, 10); write(first, base + PAGE_SIZE, 20);
    assert(read(second, alias) == 10 && read(second, moved) == 10);
    assert(!amm::fault(second, moved, true));
    auto info = amm::inspect(*a);
    const auto count = owned.size();
    page_budget = slab_budget = 0;
    assert(amm::resize(*a, 3 * PAGE_SIZE) == E_OK);
    assert(owned.size() == count && read(first, base + 2 * PAGE_SIZE) == 0);
    assert(amm::resize(*a, PAGE_SIZE) == E_OK);
    PhysAddr ignored;
    assert(!mm::user_va_to_pa(first, base + PAGE_SIZE, RIGHT_READ, &ignored));
    assert(!mm::vm_map(first, base + PAGE_SIZE, address(first, base), rw));
    assert(amm::resize(*a, 2 * PAGE_SIZE) == E_OK);
    assert(read(first, base + PAGE_SIZE) == 20);
    assert(amm::resize(*a, 0) == E_OK && amm::inspect(*a).size == 0);
    assert(!mm::user_va_to_pa(first, base, RIGHT_READ, &ignored));
    assert(amm::resize(*a, 2 * PAGE_SIZE) == E_OK && read(first, base) == 10);
    assert(amm::inspect(*a).copied_bytes == info.copied_bytes);
    page_budget = slab_budget = -1;

    // Relocation, including overlapping reservations, keeps the physical pages.
    const auto original = address(first, base);
    mm::kernel_heap_base = original; mm::kernel_heap_bytes = PAGE_SIZE;
    assert(!mm::vm_map(second, base + 0x2000000, original, rw));
    assert(!mm::vm_map_range(second, base + 0x2000000, original, PAGE_SIZE, RIGHT_READ));
    mm::kernel_heap_base = mm::kernel_heap_bytes = 0;
    assert(amm::realloc(*a, first, base, base + PAGE_SIZE, 2 * PAGE_SIZE) == E_OK);
    assert(address(first, base + PAGE_SIZE) == original);
    assert(amm::realloc(*a, first, base + PAGE_SIZE, moved, 2 * PAGE_SIZE) == E_OK);
    assert(address(first, moved) == original && !amm::reserved(first, base, PAGE_SIZE));
    watches.push_back({first, moved, mm::vm_debug_pte(first, moved), false});
    const u32 permutation[]{1, 0}, invalid[]{0, 0};
    assert(amm::reorder(*a, invalid, 2) == E_INVALID);
    assert(amm::reorder(*a, permutation, 2) == E_OK);
    assert(read(first, moved) == 20 && read(second, alias) == 20);

    // A clone initially shares backing; the first write isolates all its aliases.
    Arena* clone;
    assert(amm::clone(*a, 2, 1, &clone) == E_OK);
    assert(object::create(clone, amm::destroy));
    assert(owned.size() == count);
    map(clone, first, alias);
    assert(address(first, alias) == address(first, moved));
    assert(amm::pin(*clone, 1) == E_BUSY);
    write(first, alias, 99);
    assert(read(first, alias) == 99 && read(first, moved) == 20);
    assert(owned.size() == count + 1);
    assert(amm::prepare_write(second, alias, PAGE_SIZE));
    write(second, alias, 21);
    assert(read(first, moved) == 21 && read(first, alias) == 99);

    // Precopy reconciles stores through every alias and preserves RO rights.
    info = amm::inspect(*a); copies_before = info.copied_bytes;
    assert(amm::precopy(*a) == E_OK);
    write(second, alias + PAGE_SIZE, 42);
    assert(amm::inspect(*a).state == MX_ARENA_DIRTY);
    assert(amm::inspect(*a).dirty_pages == 1);
    const auto before_migration = address(first, moved);
    can_stop = false;
    assert(amm::commit(*a) == E_BUSY);
    assert(address(first, moved) == before_migration);
    can_stop = true;
    assert(amm::commit(*a) == E_OK);
    assert(address(first, moved) != before_migration);
    assert(read(first, moved + PAGE_SIZE) == 42 && read(second, moved + PAGE_SIZE) == 42);
    assert(!mm::user_va_to_pa(second, moved, RIGHT_WRITE, &ignored));
    info = amm::inspect(*a);
    assert(info.retired_generation && info.state == MX_ARENA_STABLE);
    assert(info.copied_bytes == copies_before + 4 * PAGE_SIZE);
    assert(amm::pin(*a, info.generation - 1) == E_BUSY);
    assert(amm::pin(*a, info.generation) == E_OK);
    assert(amm::resize(*a, PAGE_SIZE) == E_BUSY);
    assert(amm::precopy(*a) == E_BUSY);
    assert(amm::unpin(*a, info.generation) == E_OK);
    assert(amm::unpin(*a, info.generation) == E_INVALID);

    assert(amm::precopy(*a) == E_OK);
    write(first, moved, 77);
    assert(amm::precopy(*a) == E_OK); // iterative dirty reconciliation
    assert(amm::inspect(*a).dirty_pages == 0);
    assert(amm::cancel(*a) == E_OK && read(second, alias) == 77);
    assert(amm::reclaim() == E_OK);
    const auto pages_before_failure = owned.size();
    page_budget = 1;
    assert(amm::precopy(*a) == E_NOMEM);
    assert(owned.size() == pages_before_failure && amm::inspect(*a).state == MX_ARENA_STABLE);
    page_budget = -1;
    can_stop = false;
    assert(amm::resize(*a, PAGE_SIZE) == E_BUSY);
    assert(amm::inspect(*a).size == 2 * PAGE_SIZE);
    can_stop = true;

    // Capacity expansion is transactional and leaves existing contents in place.
    assert(amm::reserve(*a, 4 * PAGE_SIZE) == E_OK);
    assert(amm::resize(*a, 4 * PAGE_SIZE) == E_OK);
    assert(read(first, moved) == 77 && read(first, moved + 3 * PAGE_SIZE) == 0);
    const auto generation = amm::inspect(*a).generation;
    page_budget = 0;
    assert(amm::reserve(*a, 5 * PAGE_SIZE) == E_NOMEM);
    assert(amm::inspect(*a).generation == generation);
    page_budget = -1;
    // Failed preparation unwinds reservations; fresh maps need no rendezvous.
    auto* lifetime = record(a);
    const auto allocations = physical.size();
    page_budget = 0;
    assert(amm::map(*a, lifetime, first, base + 0x800000, rw) == E_BUSY);
    page_budget = -1; can_stop = false;
    assert(amm::map(*a, lifetime, first, base + 0x800000, rw) == E_OK);
    can_stop = true;
    assert(amm::unmap(*a, first, base + 0x800000) == E_OK);
    assert(physical.size() == allocations && !amm::reserved(first, base + 0x800000, PAGE_SIZE));
    object::release(lifetime);
    contiguous_mode = true;
    slab_budget = 1;
    const auto before_contiguous_failure = owned.size();
    assert(amm::precopy(*a) == E_NOMEM);
    assert(owned.size() == before_contiguous_failure);
    slab_budget = -1;

    // Type cages cannot overlap another type or be bypassed by relocation.
    Arena* bad = nullptr;
    MxArenaCreate request{PAGE_SIZE, PAGE_SIZE, base, 0x1000000, 2, 0, 0};
    assert(amm::create(1, 1, request, &bad) == E_POLICY && !bad);
    assert(amm::realloc(*a, first, moved, base - PAGE_SIZE, PAGE_SIZE) == E_INVALID);
    const auto wired_page = mm::alloc_page();
    const u64 owner = (u64{1} << 32) | 1;
    assert(!amm::map_wired(owner, first, base + 0x400000, wired_page, PAGE_SIZE, rw, false));
    assert(amm::map_wired(owner, first, base + 0x2000000, wired_page, PAGE_SIZE, rw, false));
    request = {PAGE_SIZE, PAGE_SIZE, base + 0x2000000, 0x1000000, 2, 0, 0};
    assert(amm::create(1, 1, request, &bad, first) == E_POLICY && !bad);
    mm::vm_unmap(first, base + 0x2000000);
    mm::free_page(wired_page);

    // Destroying an arena cannot turn its virtual addresses into another type.
    request = {PAGE_SIZE, PAGE_SIZE, base + 0x2000000, 0x1000000, 17, 0, 0};
    Arena* transient = nullptr;
    assert(amm::create(1, 1, request, &transient, first) == E_OK);
    auto* transient_record = object::create(transient, amm::destroy); assert(transient_record);
    object::release(transient_record);
    request.type_id = 18;
    assert(amm::create(1, 1, request, &bad, first) == E_POLICY && !bad);
    assert(!amm::map_wired(owner, first, request.cage_base, 0, PAGE_SIZE, rw, false));

    // Policy write sampling and bounded idle work use the same migration path.
    assert(amm::policy(*a, MX_ARENA_COMPACT_IDLE) == E_OK);
    write(first, moved, 88);
    for (unsigned i = 0; i < 512; ++i) amm::idle_step();
    assert(read(first, moved) == 88 && read(second, alias) == 88);
    assert(amm::cancel(*a) == E_OK);
    assert(address(first, moved + PAGE_SIZE) == address(first, moved) + PAGE_SIZE);

    // Genuine live precopy: the backend reenters AMM and writes while the copy
    // round owns immutable sources. An unfenced failure must retain every pin.
    assert(amm::policy(*a, 0) == E_OK);
    TestDma dma{a, first, moved, 0, 0, false, false, true, false, 0, 0};
    const adma::Backend backend{&dma, false, dma_submit, dma_poll, dma_cancel};
    assert(adma::install(&backend) == E_OK);
    assert(amm::precopy(*a) == E_BUSY && adma::active_count() == 1);
    assert(adma::install(nullptr) == E_BUSY);
    assert(amm::resize(*a, PAGE_SIZE) == E_BUSY);
    assert(amm::commit(*a) == E_BUSY);
    const auto snapshot = dma.source;
    dma.complete = true;
    assert(amm::precopy(*a) == E_OK && adma::active_count() == 0);
    assert(owned.count(snapshot)); // kernel physical readers still need a grace period
    can_stop = false;
    assert(amm::reclaim() == E_BUSY && owned.count(snapshot));
    can_stop = true;
    assert(amm::reclaim() == E_OK && !owned.count(snapshot));
    assert(amm::commit(*a) == E_OK && read(first, moved) == 100);
    dma.complete = false; dma.fail = true;
    assert(amm::precopy(*a) == E_BUSY);
    const auto pinned_count = owned.size();
    assert(amm::cancel(*a) == E_BUSY && owned.size() == pinned_count);
    dma.cancel_ok = true;
    assert(amm::cancel(*a) == E_OK && adma::active_count() == 0 && dma.fences >= 2);
    assert(adma::install(nullptr) == E_OK);

    // Real external leases have independent ownership and mandatory fences.
    assert(amm::precopy(*a, true) == E_OK && amm::commit(*a) == E_OK);
    bool fenced = false;
    amm::ExternalPin lease{};
    assert(amm::pin_external(*a, lease, external_fence, &fenced) == E_OK);
    assert(amm::prepare_dma(lease, false) == E_OK);
    amm::release_owner_pins(owner);
    assert(amm::inspect(*a).pins == 1 && amm::resize(*a, PAGE_SIZE) == E_BUSY);
    assert(amm::unpin_external(lease) == E_BUSY && lease.arena == a);
    fenced = true;
    assert(amm::unpin_external(lease) == E_OK && !lease.arena);

    // Growth and relocating realloc allocate only the added pages, atomically.
    const auto existing = address(first, moved);
    const auto copied_before_growth = amm::inspect(*a).copied_bytes;
    assert(amm::resize(*a, 5 * PAGE_SIZE) == E_OK && address(first, moved) == existing);
    assert(read(second, alias + 4 * PAGE_SIZE) == 0);
    info = amm::inspect(*a);
    page_budget = 0;
    assert(amm::realloc(*a, first, moved, base + 0x800000, 6 * PAGE_SIZE) == E_NOMEM);
    assert(amm::inspect(*a).generation == info.generation && address(first, moved) == existing);
    page_budget = -1;
    assert(amm::realloc(*a, first, moved, base + 0x800000, 6 * PAGE_SIZE) == E_OK);
    assert(address(first, base + 0x800000) == existing);
    assert(amm::inspect(*a).copied_bytes == copied_before_growth);
    assert(amm::resize(*a, 2 * PAGE_SIZE) == E_OK && amm::trim(*a) == E_OK);
    assert(amm::inspect(*a).capacity == 2 * PAGE_SIZE);
    assert(amm::unmap(*a, first, base + 0x800000) == E_OK);

    // Pins retain objects even after the last capability and map disappear.
    info = amm::inspect(*a);
    assert(amm::pin(*a, info.generation) == E_OK);
    drop_creator(a); drop_creator(clone);
    amm::detach_space(first); amm::detach_space(second);
    assert(object::live_count() == 1);
    request = {PAGE_SIZE, PAGE_SIZE, base, 0x1000000, 2, 0, 0};
    assert(amm::create(2, 1, request, &bad) == E_POLICY && !bad);
    amm::release_owner_pins((u64{1} << 32) | 2);
    amm::release_owner_pins((u64{1} << 32) | 1);
    assert(amm::reclaim() == E_OK);
    assert(object::live_count() == 0 && owned.empty());
    watches.clear();

    // Wired IPC mappings share one serialization boundary for the VA claim,
    // PTE and backing reference. A competing unmap must never erase a claim
    // before the mapper attaches its lifetime pin.
    {
        VmSpace space{mm::alloc_page(), 3};
        ArenaTable table{};
        Arena shared{};
        shared.phys_base = mm::alloc_page();
        shared.size = PAGE_SIZE;
        shared.rights = rw;
        shared.owner_pid = 3;
        const u64 shared_owner = (u64{1} << 32) | 3;
        constexpr VirtAddr shared_va = 0x34000000;
        auto* lifetime = object::create(&shared, nullptr);
        assert(lifetime);
        assert(amm::map_shared_wired(shared_owner, space, table, shared, lifetime,
                                     shared_va, rw) == E_OK);
        assert(object::reference_count(lifetime) == 2);
        can_stop = false;
        assert(amm::unmap_wired(space, shared_va, &table) == E_BUSY);
        assert(object::reference_count(lifetime) == 2 && table.count == 1);
        can_stop = true;
        assert(amm::unmap_wired(space, shared_va, &table) == E_OK);
        assert(object::reference_count(lifetime) == 1 && !table.count);
        std::atomic<bool> start{false};
        std::vector<std::thread> contenders;
        for (unsigned worker = 0; worker < 4; ++worker)
            contenders.emplace_back([&, worker] {
                while (!start.load()) {}
                for (unsigned round = 0; round < 500; ++round) {
                    if (worker & 1) {
                        assert(amm::unmap_wired(space, shared_va, &table) == E_OK);
                    } else {
                        const auto status = amm::map_shared_wired(shared_owner, space, table,
                            shared, lifetime, shared_va, rw);
                        assert(status == E_OK || status == E_INVALID);
                    }
                }
            });
        start = true;
        for (auto& contender : contenders) contender.join();
        assert(amm::unmap_wired(space, shared_va, &table) == E_OK);
        assert(!table.count && object::reference_count(lifetime) == 1);
        object::release(lifetime);
        struct Callback { VmSpace space; VirtAddr va; bool called; } callback{space, shared_va, false};
        auto* final_mapping = object::create(&callback, [](void* target) {
            auto& state = *static_cast<Callback*>(target);
            // Final releases must happen after the manager lock is dropped.
            assert(!amm::reserved(state.space, state.va, PAGE_SIZE));
            state.called = true;
        });
        assert(amm::map_shared_wired(shared_owner, space, table, shared, final_mapping,
                                     shared_va, rw) == E_OK);
        object::release(final_mapping);
        assert(amm::unmap_wired(space, shared_va, &table) == E_OK && callback.called);
        mm::free_page(shared.phys_base);
        mm::vm_space_destroy(space);
    }
    mm::vm_space_destroy(first); mm::vm_space_destroy(second);
    assert(physical.empty() && metadata.empty() && allocation_remaining.empty() && !stopped);
    std::puts("amm probe ok");
}
