#pragma once

#include "anko/types.h"

namespace platform {

// Cold boot data, separate from HwInfo: decode once while the immutable DTB is
// cached, then stream these records into the allocator after phys_init(). No
// node graph, DTB property pointers, or allocator dependency survives discovery.
struct BootReservations {
    struct Range {
        PhysAddr base;
        u64 size;
    };
    static constexpr u32 kMaxRanges = 128;
    Range ranges[kMaxRanges];
    const u8* source = nullptr; // non-null only after both DT sections validate
    u32 count = 0;
    u32 memreserve_count = 0; // header entries precede /reserved-memory records
    bool overflow = false;

    void append(PhysAddr base, u64 size) {
        if (count == kMaxRanges) {
            overflow = true;
            return;
        }
        ranges[count++] = {base, size};
    }
};
static_assert(sizeof(BootReservations::Range) == 16);

// Published by probe() on the sole boot CPU. A missing/mismatched source means
// the consumer must replay the original walkers, never use a partial batch.
const BootReservations& boot_reservations();

} // namespace platform
