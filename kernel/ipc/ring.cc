// kernel/ipc/ring.cc — SPSC ring creation, sharing, mapping.
//
// Spec ref: §5.8 IPC, §9 V0 IPC Ring Layout

#include "anko/ipc.h"
#include "anko/mm.h"
#include "anko/cap.h"
#include "anko/sched.h"
#include "libk/string.h"

namespace ipc {

static void destroy_ring(void* target) {
    auto* ring = static_cast<Ring*>(target);
    const u64 bytes = sizeof(RingHeader) + (1ull << ring->order) * sizeof(RingSlot);
    for (u64 off = 0; off < bytes; off += PAGE_SIZE) mm::free_page_for_identity((static_cast<u64>(ring->owner_generation) << 32) | ring->producer_pid, ring->phys_base + off);
    mm::slab_free(ring, sizeof(Ring), alignof(Ring));
}


void init() {
    // V0 keeps ring objects heap-allocated; no global registry required.
}

static constexpr u32 kRingOrderMin = 4;   // 16 slots
static constexpr u32 kRingOrderMax = 10;  // 1024 slots

static cap::Ref resolve_ring_entry(u32 pid, CapHandle ring_cap, u32 need_rights) {
    auto p = sched::find_process(pid);
    if (!p || !p->cap_table) return nullptr;

    cap::Ref e = cap::lookup(*p->cap_table, ring_cap, CAP_SHARED_RING);
    if (!e) return nullptr;
    if ((e->rights_mask & need_rights) != need_rights) return nullptr;
    return e;
}

Ring* ring_create_object(u32 pid, u32 order, u32 flags) {
    (void)flags;
    if (order < kRingOrderMin || order > kRingOrderMax) return nullptr;

    auto owner = sched::find_process(pid);
    if (!owner || !owner->cap_table) return nullptr;

    const usize slots = (1u << order);
    const usize bytes = sizeof(RingHeader) + slots * sizeof(RingSlot);
    const usize pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    const PhysAddr base = mm::alloc_pages_contiguous_for(pid, pages);
    if (!base) return nullptr;
    libk::memset(reinterpret_cast<void*>(base), 0, pages * PAGE_SIZE);

    Ring* ring = static_cast<Ring*>(mm::slab_alloc(sizeof(Ring), alignof(Ring)));
    if (!ring) {
        for (usize i = 0; i < pages; ++i) {
            mm::free_page_for(pid, base + i * PAGE_SIZE);
        }
        return nullptr;
    }
    ring->phys_base = base;
    ring->order = order;
    ring->producer_pid = pid;
    ring->consumer_pid = pid;
    ring->owner_generation = owner->generation;

    if (!object::create(ring, destroy_ring)) { destroy_ring(ring); return nullptr; }
    return ring;
}

CapHandle ring_cap_alloc(u32 target_pid, Ring* ring, u32 rights) {
    if (!ring) return kNullCap;
    auto target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    return cap::alloc(*target->cap_table, CAP_SHARED_RING, ring, requested);
}

CapHandle ring_create(u32 pid, u32 order, u32 flags) {
    Ring* ring = ring_create_object(pid, order, flags);
    if (!ring) return kNullCap;
    // acquire balances itself; the second release transfers the creator root.
    auto* root = object::acquire(ring);
    CapHandle handle = ring_cap_alloc(pid, ring, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
    object::release(root);
    object::release(root);
    return handle;
}

CapHandle ring_share(u32 owner_pid, CapHandle ring_cap,
                     u32 target_pid, u32 rights) {
    cap::Ref src = resolve_ring_entry(owner_pid, ring_cap, RIGHT_DUP);
    if (!src) return kNullCap;
    Ring* ring = static_cast<Ring*>(src->target);
    if (!ring) return kNullCap;

    auto target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    if ((requested & src->rights_mask) != requested) return kNullCap;
    const u32 granted = requested;
    const CapHandle h = ring_cap_alloc(target_pid, ring, granted);
    if (h == kNullCap) return kNullCap;
    if (ring->consumer_pid == owner_pid) ring->consumer_pid = target_pid;
    return h;
}

cap::Borrow<Ring> ring_from_cap(u32 pid, CapHandle cap) {
    cap::Ref e = resolve_ring_entry(pid, cap, RIGHT_READ);
    return e ? cap::Borrow<Ring>(e) : cap::Borrow<Ring>(nullptr);
}

} // namespace ipc
