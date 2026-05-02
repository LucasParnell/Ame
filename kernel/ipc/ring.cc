// kernel/ipc/ring.cc — SPSC ring creation, sharing, mapping.
//
// Spec ref: §5.8 IPC, §9 V0 IPC Ring Layout

#include "anko/ipc.h"
#include "anko/mm.h"
#include "anko/cap.h"
#include "anko/sched.h"
#include "libk/string.h"

namespace ipc {

void init() {
    // V0 keeps ring objects heap-allocated; no global registry required.
}

static constexpr u32 kRingOrderMin = 4;   // 16 slots
static constexpr u32 kRingOrderMax = 10;  // 1024 slots

static bool alloc_contiguous_pages(usize pages, PhysAddr* out_base) {
    if (!out_base || pages == 0) return false;
    if (pages > 64) return false;

    PhysAddr allocs[64]{};
    usize got = 0;

    PhysAddr first = mm::alloc_page();
    if (!first) return false;
    allocs[got++] = first;

    PhysAddr expected = first + PAGE_SIZE;
    for (usize i = 1; i < pages; ++i) {
        PhysAddr pa = mm::alloc_page();
        if (!pa || pa != expected) {
            if (pa) mm::free_page(pa);
            for (usize j = 0; j < got; ++j) mm::free_page(allocs[j]);
            return false;
        }
        allocs[got++] = pa;
        expected += PAGE_SIZE;
    }

    *out_base = first;
    return true;
}

static CapEntry* resolve_ring_entry(u32 pid, CapHandle ring_cap, u32 need_rights) {
    Process* p = sched::find_process(pid);
    if (!p || !p->cap_table) return nullptr;

    CapEntry* e = cap::lookup(*p->cap_table, ring_cap, CAP_SHARED_RING);
    if (!e) return nullptr;
    if ((e->rights_mask & need_rights) != need_rights) return nullptr;
    return e;
}

Ring* ring_create_object(u32 pid, u32 order, u32 flags) {
    if (order < kRingOrderMin || order > kRingOrderMax) return nullptr;

    Process* owner = sched::find_process(pid);
    if (!owner || !owner->cap_table) return nullptr;

    const usize slots = (1u << order);
    const usize bytes = sizeof(RingHeader) + slots * sizeof(RingSlot);
    const usize pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    PhysAddr base = 0;
    if (!alloc_contiguous_pages(pages, &base)) return nullptr;
    libk::memset(reinterpret_cast<void*>(base), 0, pages * PAGE_SIZE);

    u32 arena_idx = 0;
    if (!owner->arena_table ||
        !mm::arena_register(*owner->arena_table,
                            static_cast<VirtAddr>(base),
                            pages * PAGE_SIZE,
                            pid,
                            RIGHT_READ | RIGHT_WRITE,
                            flags,
                            &arena_idx)) {
        for (usize i = 0; i < pages; ++i) {
            mm::free_page(base + i * PAGE_SIZE);
        }
        return nullptr;
    }

    Ring* ring = static_cast<Ring*>(mm::heap_alloc(sizeof(Ring), alignof(Ring)));
    if (!ring) {
        for (usize i = 0; i < pages; ++i) {
            mm::free_page(base + i * PAGE_SIZE);
        }
        Arena& arena = owner->arena_table->entries[arena_idx];
        libk::memset(&arena, 0, sizeof(arena));
        if (owner->arena_table->count == arena_idx + 1) owner->arena_table->count = arena_idx;
        return nullptr;
    }
    ring->phys_base = base;
    ring->order = order;
    ring->producer_pid = pid;
    ring->consumer_pid = pid;
    ring->_pad = 0;

    return ring;
}

CapHandle ring_cap_alloc(u32 target_pid, Ring* ring, u32 rights) {
    if (!ring) return kNullCap;
    Process* target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    return cap::alloc(*target->cap_table, CAP_SHARED_RING, ring, requested);
}

CapHandle ring_create(u32 pid, u32 order, u32 flags) {
    Ring* ring = ring_create_object(pid, order, flags);
    return ring_cap_alloc(pid, ring, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
}

CapHandle ring_share(u32 owner_pid, CapHandle ring_cap,
                     u32 target_pid, u32 rights) {
    CapEntry* src = resolve_ring_entry(owner_pid, ring_cap, RIGHT_DUP);
    if (!src) return kNullCap;
    Ring* ring = static_cast<Ring*>(src->target);
    if (!ring) return kNullCap;

    Process* target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    if (ring->consumer_pid == owner_pid) ring->consumer_pid = target_pid;

    const u32 allowed = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    if ((requested & src->rights_mask) != requested) return kNullCap;
    const u32 granted = requested;
    return ring_cap_alloc(target_pid, ring, granted);
}

Ring* ring_from_cap(u32 pid, CapHandle cap) {
    CapEntry* e = resolve_ring_entry(pid, cap, RIGHT_READ);
    return e ? static_cast<Ring*>(e->target) : nullptr;
}

} // namespace ipc
