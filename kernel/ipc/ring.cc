// kernel/ipc/ring.cc — SPSC ring creation, sharing, mapping.
//
// Spec ref: §5.8 IPC, §9 V0 IPC Ring Layout

#include "anko/ipc.h"
#include "midix/abi/ring.h"
#include "anko/mm.h"
#include "anko/cap.h"
#include "anko/sched.h"
#include "libk/string.h"
#include "ipc_internal.h"

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

static cap::Ref resolve_ring_entry(u32 pid, CapHandle ring_cap, u32 need_rights) {
    return resolve_entry(pid, ring_cap, CAP_SHARED_RING, need_rights);
}

Ring* ring_create_object(u32 pid, u32 order, u32 flags) {
    (void)flags;
    if (order < MX_RING_ORDER_MIN || order > MX_RING_ORDER_MAX) return nullptr;

    auto owner = process_reference(pid);
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

    ring->lifetime = object::create(ring, destroy_ring);
    if (!ring->lifetime) { destroy_ring(ring); return nullptr; }
    return ring;
}

CapHandle ring_cap_alloc(u32 target_pid, Ring* ring, u32 rights) {
    if (!ring) return kNullCap;
    auto target = process_reference(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    return cap::alloc(*target->cap_table, CAP_SHARED_RING, ring, requested, ring->lifetime);
}

CapHandle ring_create(u32 pid, u32 order, u32 flags) {
    Ring* ring = ring_create_object(pid, order, flags);
    if (!ring) return kNullCap;
    CapHandle handle = ring_cap_alloc(pid, ring, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
    object::release(ring->lifetime); // transfer the creator root to the capability
    return handle;
}

CapHandle ring_share(u32 owner_pid, CapHandle ring_cap,
                     u32 target_pid, u32 rights) {
    cap::Ref src = resolve_ring_entry(owner_pid, ring_cap, RIGHT_DUP);
    if (!src) return kNullCap;
    Ring* ring = static_cast<Ring*>(src->target);
    if (!ring) return kNullCap;

    auto target = process_reference(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    if ((requested & src->rights_mask) != requested) return kNullCap;
    const CapHandle h = cap::alloc(*target->cap_table, CAP_SHARED_RING, ring, requested, src->object);
    if (h == kNullCap) return kNullCap;
    __atomic_compare_exchange_n(&ring->consumer_pid, &owner_pid, target_pid, false,
                                 __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return h;
}

cap::Borrow<Ring> ring_from_cap(u32 pid, CapHandle cap) {
    cap::Ref e = resolve_ring_entry(pid, cap, RIGHT_READ);
    return e ? cap::Borrow<Ring>(static_cast<cap::Ref&&>(e)) : cap::Borrow<Ring>(nullptr);
}

} // namespace ipc
