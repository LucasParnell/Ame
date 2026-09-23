#pragma once
// Byte-exact shared-ring layout. Keep this synchronized with kernel/include/anko/ipc.h.

#include "midix/abi/types.h"

struct alignas(64) MxRingHeadLine { mx_u64_t v; mx_u8_t pad[56]; };
struct alignas(64) MxRingTailLine { mx_u64_t v; mx_u8_t pad[56]; };
struct alignas(64) MxRingFlagsLine {
    mx_u32_t receiver_about_to_sleep;
    mx_u32_t reserved0;
    mx_u64_t doorbell_counter_snapshot;
    mx_u8_t  pad[48];
};
struct alignas(64) MxRingSlot {
    mx_u32_t type;
    mx_u32_t length;
    mx_u64_t seq;
    mx_u8_t  payload[48];
};
struct MxRingPage {
    MxRingHeadLine  head;
    MxRingTailLine  tail;
    MxRingFlagsLine flags;
    mx_u8_t         _res[64];
};
static_assert(sizeof(MxRingHeadLine) == 64, "ring head must be one cache line");
static_assert(sizeof(MxRingTailLine) == 64, "ring tail must be one cache line");
static_assert(sizeof(MxRingFlagsLine) == 64, "ring flags must be one cache line");
static_assert(sizeof(MxRingSlot) == 64, "ring slot must be one cache line");
static_assert(sizeof(MxRingPage) == 0x100, "ring page header must be 256 bytes");

constexpr mx_u32_t MX_RING_INLINE_PAYLOAD = 48;
constexpr mx_u32_t MX_RING_HEADER_SIZE = 0x100;
constexpr mx_u32_t MX_DEFAULT_RING_ORDER = 6;

static_assert(MX_RING_HEADER_SIZE == sizeof(MxRingPage),
              "ring header size must equal the fixed ring page header");
static_assert(MX_RING_INLINE_PAYLOAD == sizeof(MxRingSlot::payload),
              "inline payload budget must equal the slot payload");

// --- Ring geometry ---------------------------------------------------------
//
// The byte layout is stated once here and derived everywhere else. Kernel,
// userspace and the console service all address slots through these helpers, so
// the header size (0x100) and the slot stride are never re-hardcoded. All of
// them are data-oriented: pure functions of (order, index) over the fixed page.

// Capacity in slots for a power-of-two ring of the given order.
constexpr mx_u32_t mx_ring_capacity(mx_u32_t order) {
    return 1u << order;
}

// Index mask (capacity - 1) for the given order.
constexpr mx_u32_t mx_ring_mask(mx_u32_t order) {
    return (1u << order) - 1u;
}

// Byte offset of slot `index` (wrapped into the ring) from the page base.
constexpr mx_size_t mx_ring_slot_offset(mx_u32_t order, mx_u32_t index) {
    return MX_RING_HEADER_SIZE +
           static_cast<mx_size_t>(index & mx_ring_mask(order)) * sizeof(MxRingSlot);
}

// Total bytes a ring of the given order occupies: header plus every slot.
constexpr mx_size_t mx_ring_page_bytes(mx_u32_t order) {
    return MX_RING_HEADER_SIZE +
           static_cast<mx_size_t>(mx_ring_capacity(order)) * sizeof(MxRingSlot);
}

// First slot of a mapped ring page.
inline MxRingSlot* mx_ring_slots(MxRingPage* page) {
    return reinterpret_cast<MxRingSlot*>(reinterpret_cast<mx_u8_t*>(page) +
                                         MX_RING_HEADER_SIZE);
}
inline const MxRingSlot* mx_ring_slots(const MxRingPage* page) {
    return reinterpret_cast<const MxRingSlot*>(
        reinterpret_cast<const mx_u8_t*>(page) + MX_RING_HEADER_SIZE);
}

// Slot `index` (wrapped into the ring) of a mapped ring page.
inline MxRingSlot* mx_ring_slot_at(MxRingPage* page, mx_u32_t order, mx_u32_t index) {
    return mx_ring_slots(page) + (index & mx_ring_mask(order));
}
