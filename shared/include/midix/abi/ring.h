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
