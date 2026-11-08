#pragma once
#include "midix/abi/types.h"

// Managed arenas reserve virtual capacity and preallocate backing at creation.
// Resize within capacity and realloc at a new VA copy no payload. They still
// update PTEs and synchronize TLBs. Shrink retains capacity and its contents.
enum MxArenaOperation : mx_u64_t {
    MX_ARENA_NEW = 0, MX_ARENA_RESIZE, MX_ARENA_REALLOC, MX_ARENA_REORDER,
    MX_ARENA_CLONE, MX_ARENA_PRECOPY, MX_ARENA_COMMIT, MX_ARENA_CANCEL,
    MX_ARENA_INSPECT, MX_ARENA_PIN, MX_ARENA_UNPIN, MX_ARENA_POLICY,
    MX_ARENA_RESERVE,
    MX_ARENA_TRIM, MX_ARENA_UNMAP,
    MX_ARENA_PROCESS,
};
enum MxArenaState : mx_u32_t {
    MX_ARENA_STABLE = 0, MX_ARENA_PRECOPY_STATE, MX_ARENA_DIRTY,
    MX_ARENA_FROZEN, MX_ARENA_COMMITTING, MX_ARENA_RETIRED,
};
constexpr mx_u32_t MX_ARENA_COMPACT_IDLE = 1u << 0;
struct MxArenaCreate {
    mx_u64_t size, capacity;
    // All mappings, including realloc, must remain in this immutable cage.
    mx_u64_t cage_base, cage_size;
    mx_u64_t type_id;
    mx_u32_t policy, reserved;
};
struct MxArenaInfo {
    mx_u64_t size, capacity, generation, retired_generation, type_id;
    mx_u64_t copied_bytes, zero_copy_changes;
    mx_u32_t state, pins, mappings, dirty_pages;
};
static_assert(sizeof(MxArenaCreate) == 48);
static_assert(sizeof(MxArenaInfo) == 72);
constexpr mx_u64_t MX_PROCESS_ARENA_BASE = 0x38000000;
constexpr mx_u64_t MX_PROCESS_ARENA_DEFAULT = 65536;
constexpr mx_u64_t MX_PROCESS_ARENA_LIMIT = 1048576;
