// tests/kernel/shared_arena_probe.cc — host regression for dynamic shared
// arenas (Phase L, spec §12.3).
//
// Compiles kernel/vm/vm_shared.cc together with kernel/vm/stage2.cc and
// kernel/iommu/core.cc against stubbed physical-page, memset, object-registry
// and log services, and proves the invariants a native owner relies on when it
// attaches an existing pinned page set to a live VM:
//
//   (a) attach maps the arena's backing pages into stage 2 at the requested
//       guest IPA, as Normal memory, with the requested access rights;
//   (b) every malformed shape is rejected: unaligned/out-of-range guest_pa,
//       wrong extent, no backing, EXEC rights, unknown flags, and overlap with
//       an existing mapping;
//   (c) an attached but unregistered region is absent from every DMA domain —
//       the IOMMU core's page-owner query refuses it — while an explicitly
//       registered region maps as VmDmaClass::SharedGpu;
//   (d) the attachment couples the host arena lifetime and the guest mapping
//       lifetime: releasing its reference unmaps the range and drops both
//       retained records exactly once;
//   (e) the attachment pool is fixed-capacity.

#include "anko/iommu.h"
#include "anko/log.h"
#include "anko/panic.h"
#include "anko/vm.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

// Kernel VM data structures are referenced bare throughout (Vm, SharedArena,
// VM_*, kSharedArenaFlagNone); the probe reads best at the kernel's own level.
using namespace vm;

// --- kernel page allocator + memset stubs ----------------------------------
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

// --- logging + panic stubs --------------------------------------------------
namespace anko::log {
void write(Level, const char*, const char*) {}
void writef(Level, const char*, const char*, ...) {}
} // namespace anko::log

[[noreturn]] void panic(const char* msg) {
    std::fprintf(stderr, "panic: %s\n", msg);
    std::abort();
}
[[noreturn]] void panicf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::abort();
}

// --- object registry model --------------------------------------------------
// A tiny reference-counted registry, faithful enough to observe the lifetime
// coupling between an attachment and the records it retains.
namespace object {
struct Record {
    void* target;
    Destructor destroy;
    u64 refs;
};
namespace {
std::unordered_map<void*, Record*> g_records;
}

Record* create(void* target, Destructor destroy) {
    if (!target) {
        return nullptr;
    }
    assert(g_records.find(target) == g_records.end() && "duplicate object target");
    auto* record = new Record{target, destroy, 1};
    g_records.emplace(target, record);
    return record;
}
Record* acquire(void* target) {
    if (!target) {
        return nullptr;
    }
    auto it = g_records.find(target);
    if (it != g_records.end()) {
        ++it->second->refs;
        return it->second;
    }
    return create(target, nullptr);
}
void retain(Record* record) {
    if (record) {
        ++record->refs;
    }
}
void release(Record* record) {
    if (!record) {
        return;
    }
    assert(record->refs && "release of an unreferenced record");
    if (--record->refs) {
        return;
    }
    Destructor destroy = record->destroy;
    void* target = record->target;
    g_records.erase(target);
    delete record;
    if (destroy) {
        destroy(target);
    }
}
u64 live_count() {
    return g_records.size();
}
u64 refs_of(Record* record) {
    return record ? record->refs : 0;
}
} // namespace object

// --- vm / cap stubs for the capability-facing wrappers ----------------------
namespace vm {
void metric_inc(VmDebugCounter) {}
void note_error() {}
cap::Ref require_cap(Process&, CapHandle, CapType, u32, SysError*) {
    return cap::Ref(nullptr);
}
cap::Borrow<Vm> vm_from_cap(Process&, CapHandle, u32, SysError* out_error) {
    if (out_error) {
        *out_error = E_DEAD;
    }
    return cap::Borrow<Vm>(nullptr);
}
} // namespace vm

namespace cap {
CapHandle alloc(CapTable&, CapType, void*, u32) {
    return kNullCap;
}
} // namespace cap

// --- fixtures ---------------------------------------------------------------
namespace {
constexpr u64 kPage = PAGE_SIZE;
constexpr u32 kRw = RIGHT_READ | RIGHT_WRITE;
constexpr u32 kRwDma = static_cast<u32>(iommu::DmaPerm::Read) |
                       static_cast<u32>(iommu::DmaPerm::Write);

u8* alloc_host_pages(u64 pages) {
    void* p = std::aligned_alloc(kPage, pages * kPage);
    assert(p);
    std::memset(p, 0, pages * kPage);
    return static_cast<u8*>(p);
}

struct ArenaFixture {
    Arena arena{};
    u8* host = nullptr;
    object::Record* record = nullptr;
};

ArenaFixture make_arena(u64 pages) {
    ArenaFixture f;
    f.host = alloc_host_pages(pages);
    f.arena.phys_base = reinterpret_cast<PhysAddr>(f.host);
    f.arena.size = pages * kPage;
    f.arena.rights = kRw;
    f.arena.flags = kArenaFlagOwnsPages;
    f.record = object::create(&f.arena, nullptr);
    return f;
}

void destroy_arena(ArenaFixture& f) {
    object::release(f.record);
    std::free(f.host);
}

struct VmFixture {
    Vm* vm = nullptr;
};

VmFixture make_vm(u32 id) {
    VmFixture f;
    f.vm = new Vm{};
    std::memset(f.vm, 0, sizeof(*f.vm));
    f.vm->vm_id = id;
    f.vm->state = VM_CREATED;
    assert(vm::stage2_create(&f.vm->stage2) == E_OK);
    f.vm->lifetime = object::create(
        f.vm, [](void* target) { delete static_cast<Vm*>(target); });
    assert(f.vm->lifetime);
    return f;
}

// Every attachment of `vm` must already be released before this runs.
void destroy_vm(VmFixture& f) {
    vm::stage2_destroy(&f.vm->stage2);
    object::release(f.vm->lifetime);
    f.vm = nullptr;
}

iommu::IommuMap make_map(u64 iova, PhysAddr pa, u64 length, u32 perms) {
    iommu::IommuMap m{};
    m.iova = iova;
    m.pa = pa;
    m.length = length;
    m.perms = perms;
    return m;
}
} // namespace

int main() {
    // (a) Attach maps the pinned pages and couples both lifetimes.
    VmFixture base = make_vm(11);
    ArenaFixture fixt = make_arena(4);
    const u64 guest_pa = 0x5000'0000ull;
    const u64 arena_refs0 = object::refs_of(fixt.record);
    const u64 vm_refs0 = object::refs_of(base.vm->lifetime);

    vm::SharedArena* shared = nullptr;
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa, fixt.arena.size,
                                   kRw, vm::kSharedArenaFlagNone, &shared) == E_OK);
    assert(shared && base.vm->shared_arena_count == 1 && base.vm->shared_arenas[0] == shared);
    assert(object::refs_of(fixt.record) == arena_refs0 + 1);
    assert(object::refs_of(base.vm->lifetime) == vm_refs0 + 1);
    assert(shared->mapped_stage2 && !shared->dead);
    // (c) untrusted: enters stage 2 with no DMA class.
    assert(vm::shared_arena_dma_class(*shared) == iommu::VmDmaClass::None);

    for (u64 i = 0; i < fixt.arena.size / kPage; ++i) {
        u32 rights = 0;
        vm::Stage2MemType type = vm::Stage2MemType::Device;
        const PhysAddr pa = vm::stage2_lookup_page(&base.vm->stage2, guest_pa + i * kPage,
                                                   &rights, &type);
        assert(pa == reinterpret_cast<PhysAddr>(fixt.host) + i * kPage);
        assert(rights == kRw);
        assert(type == vm::Stage2MemType::NormalWB);
    }

    // (b) Malformed attachments are rejected without disturbing stage 2.
    const u64 mapped_before = base.vm->stage2.mapped_pages;
    vm::SharedArena* bad = nullptr;
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa + 1, fixt.arena.size,
                                   kRw, vm::kSharedArenaFlagNone, &bad) == E_INVALID);
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa, fixt.arena.size - kPage,
                                   kRw, vm::kSharedArenaFlagNone, &bad) == E_INVALID);
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, 0, fixt.arena.size, kRw,
                                   vm::kSharedArenaFlagNone, &bad) == E_INVALID);
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, 1ull << 39, fixt.arena.size,
                                   kRw, vm::kSharedArenaFlagNone, &bad) == E_INVALID);
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa + 0x100'0000ull,
                                   fixt.arena.size, 0, vm::kSharedArenaFlagNone, &bad) ==
           E_INVALID);
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa + 0x100'0000ull,
                                   fixt.arena.size, RIGHT_READ | RIGHT_EXEC,
                                   vm::kSharedArenaFlagNone, &bad) == E_INVALID);
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa + 0x100'0000ull,
                                   fixt.arena.size, kRw, 1u, &bad) == E_INVALID);
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa - kPage,
                                   fixt.arena.size, kRw, vm::kSharedArenaFlagNone, &bad) ==
           E_BUSY); // overlaps the attachment above
    // A VA-only arena (no pinned pages) cannot be attached.
    Arena va_only{};
    va_only.size = kPage;
    assert(vm::shared_arena_create(base.vm, &va_only, fixt.record, guest_pa + 0x200'0000ull, kPage,
                                   kRw, vm::kSharedArenaFlagNone, &bad) == E_INVALID);
    assert(base.vm->stage2.mapped_pages == mapped_before);
    assert(base.vm->shared_arena_count == 1);
    assert(object::refs_of(fixt.record) == arena_refs0 + 1);

    // (c) A registered region is DMA-visible to exactly its own domain; an
    //     unregistered one is owned by nobody.
    {
        VmFixture dv = make_vm(12);
        ArenaFixture df = make_arena(4);
        const u64 d_guest = 0x5010'0000ull;
        vm::SharedArena* ds = nullptr;
        assert(vm::shared_arena_create(dv.vm, &df.arena, df.record, d_guest, df.arena.size, kRw,
                                       vm::kSharedArenaFlagNone, &ds) == E_OK);

        iommu::IommuOwnerOps ops{};
        ops.query = vm::shared_arena_dma_query;
        ops.ctx = ds;
        iommu::IommuDomain* domain = nullptr;
        assert(iommu::domain_create(ds->object_tag, ops, iommu::null_backend(), &domain) == E_OK);

        const iommu::IommuMap map =
            make_map(0x6000'0000, ds->host_phys_base, ds->size, kRwDma);
        // Unregistered: the domain refuses the page (owner query returns false).
        assert(iommu::domain_map_batch(*domain, &map, 1) == E_PERM);
        assert(iommu::domain_pinned_pages(*domain) == 0);

        // The transition is guarded: unknown perms, and an attempted widening of
        // an already-registered set, are both rejected.
        assert(vm::shared_arena_register_dma(ds, 0) == E_INVALID);
        assert(vm::shared_arena_register_dma(ds, static_cast<u32>(iommu::DmaPerm::Read)) == E_OK);
        assert(vm::shared_arena_dma_class(*ds) == iommu::VmDmaClass::SharedGpu);
        assert(vm::shared_arena_register_dma(ds, kRwDma) == E_INVALID); // no upgrade
        assert(vm::shared_arena_register_dma(ds, static_cast<u32>(iommu::DmaPerm::Read)) == E_OK);
        // A page outside the region is never owned, even once registered.
        iommu::PageOwnership own{};
        assert(!vm::shared_arena_dma_query(ds, ds->host_phys_base + ds->size, &own));
        assert(vm::shared_arena_dma_query(ds, ds->host_phys_base, &own));
        assert(own.object == ds->object_tag && own.dma_class == iommu::VmDmaClass::SharedGpu);

        // Now it maps, as a homogeneous SharedGpu range.
        assert(iommu::domain_map_batch(*domain, &map, 1) == E_OK);
        assert(iommu::domain_pinned_pages(*domain) == ds->size / kPage);
        assert(iommu::domain_unmap_batch(*domain, &map, 1) == E_OK);
        iommu::domain_destroy(domain);

        // An unrelated domain may not reach the region even once registered.
        iommu::IommuDomain* foreign = nullptr;
        assert(iommu::domain_create(ds->object_tag ^ 0x1u, ops, iommu::null_backend(), &foreign) ==
               E_OK);
        assert(iommu::domain_map_batch(*foreign, &map, 1) == E_PERM);
        iommu::domain_destroy(foreign);

        object::release(ds->lifetime);
        assert(dv.vm->shared_arena_count == 0);
        destroy_arena(df);
        destroy_vm(dv);
    }

    // (d) Releasing the attachment tears the guest mapping down and drops both
    //     retained records exactly once.
    object::release(shared->lifetime);
    shared = nullptr;
    assert(base.vm->shared_arena_count == 0);
    assert(vm::stage2_lookup_page(&base.vm->stage2, guest_pa, nullptr) == 0);
    assert(object::refs_of(fixt.record) == arena_refs0);
    assert(object::refs_of(base.vm->lifetime) == vm_refs0);

    // Teardown is idempotent and safe on a retired VM.
    vm::SharedArena* tv = nullptr;
    assert(vm::shared_arena_create(base.vm, &fixt.arena, fixt.record, guest_pa, fixt.arena.size,
                                   kRw, vm::kSharedArenaFlagNone, &tv) == E_OK);
    assert(vm::shared_arena_teardown(tv) == E_OK);
    assert(vm::shared_arena_teardown(tv) == E_OK);
    assert(!tv->mapped_stage2 && tv->dead);
    assert(vm::stage2_lookup_page(&base.vm->stage2, guest_pa, nullptr) == 0);
    assert(vm::shared_arena_register_dma(tv, kRwDma) == E_DEAD);
    object::release(tv->lifetime);
    destroy_arena(fixt);

    // (e) release_for_vm tears down surviving attachments before stage-2 dies.
    {
        VmFixture rv = make_vm(13);
        ArenaFixture rf = make_arena(2);
        vm::SharedArena* rs = nullptr;
        assert(vm::shared_arena_create(rv.vm, &rf.arena, rf.record, 0x5020'0000ull, rf.arena.size,
                                       kRw, vm::kSharedArenaFlagNone, &rs) == E_OK);
        rv.vm->state = VM_DEAD;
        vm::shared_arena_release_for_vm(rv.vm);
        assert(rs->dead && !rs->mapped_stage2);
        assert(rv.vm->shared_arena_count == 0);
        object::release(rs->lifetime);
        destroy_arena(rf);
        destroy_vm(rv);
    }

    // (f) The attachment pool is fixed-capacity.
    {
        VmFixture cv = make_vm(14);
        ArenaFixture cf = make_arena(1);
        for (u32 i = 0; i < vm::kMaxSharedArenas; ++i) {
            vm::SharedArena* slot = nullptr;
            assert(vm::shared_arena_create(cv.vm, &cf.arena, cf.record,
                                           0x5100'0000ull + i * 0x1'0000ull, cf.arena.size, kRw,
                                           vm::kSharedArenaFlagNone, &slot) == E_OK);
        }
        vm::SharedArena* overflow = nullptr;
        assert(vm::shared_arena_create(cv.vm, &cf.arena, cf.record, 0x5100'0000ull + 0x20'0000ull,
                                       cf.arena.size, kRw, vm::kSharedArenaFlagNone, &overflow) ==
               E_OVERFLOW);
        while (cv.vm->shared_arena_count) {
            object::release(cv.vm->shared_arenas[cv.vm->shared_arena_count - 1]->lifetime);
        }
        destroy_arena(cf);
        destroy_vm(cv);
    }

    destroy_vm(base);

    assert(g_mm_pages.empty() && "stage-2 page-table pages leaked");
    assert(object::live_count() == 0 && "kernel objects leaked");
    std::printf("shared arena probe ok\n");
    return 0;
}
