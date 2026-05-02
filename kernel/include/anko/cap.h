#pragma once
// anko/cap.h — capability data layout.
//
// Spec ref: §5.5 Capability Tables
//
// Handle format:
//   bits  0..31  slot index into the per-process cap table
//   bits 32..63  generation counter (revocation check)

#include "anko/types.h"

// ---------------------------------------------------------------------------
// Handle encoding (spec §5.5)
// ---------------------------------------------------------------------------
using CapHandle = u64;

constexpr CapHandle kNullCap = 0;

constexpr u32 cap_index(CapHandle h)      { return static_cast<u32>(h & 0xFFFF'FFFFu); }
constexpr u32 cap_generation(CapHandle h) { return static_cast<u32>(h >> 32); }
constexpr CapHandle cap_make(u32 index, u32 gen) {
    return (static_cast<u64>(gen) << 32) | index;
}

// ---------------------------------------------------------------------------
// Cap types (spec §5.5)
// ---------------------------------------------------------------------------
enum CapType : u16 {
    CAP_NULL        = 0,
    CAP_THREAD      = 1,
    CAP_PROCESS     = 2,
    CAP_PAGE        = 3,
    CAP_SHARED_RING = 4,
    CAP_DOORBELL    = 5,
    CAP_ARENA       = 6,
    CAP_NAMESERVICE = 7,
    CAP_SUPERVISOR  = 8,
    CAP_UART        = 9,
    CAP_TIMER       = 10,
    CAP_VM_ROOT     = 11,
    CAP_VM          = 12,
    CAP_VCPU        = 13,
    CAP_VM_MEMORY   = 14,
    CAP_VM_CHANNEL  = 15,
};

// ---------------------------------------------------------------------------
// Rights bits (spec §5.5)
// ---------------------------------------------------------------------------
enum CapRights : u32 {
    RIGHT_READ     = 1u << 0,
    RIGHT_WRITE    = 1u << 1,
    RIGHT_MAP      = 1u << 2,
    RIGHT_EXEC     = 1u << 3,
    RIGHT_NOTIFY   = 1u << 4,
    RIGHT_WAIT     = 1u << 5,
    RIGHT_DUP      = 1u << 6,
    RIGHT_SPAWN    = 1u << 7,
    RIGHT_KILL     = 1u << 8,
    RIGHT_REGISTER = 1u << 9,
    RIGHT_THREAD_CONTROL = 1u << 10,
    RIGHT_VM_CONTROL = 1u << 11,
    RIGHT_VM_MAP = 1u << 12,
    RIGHT_VM_INSPECT = 1u << 13,
    RIGHT_VM_DESTROY = 1u << 14,
    RIGHT_VCPU_CONTROL = 1u << 15,
    RIGHT_VCPU_INSPECT = 1u << 16,
    RIGHT_VM_MEMORY_MAP = 1u << 17,
    RIGHT_VM_MEMORY_INSPECT = 1u << 18,
    RIGHT_VM_CHANNEL_SEND = 1u << 19,
    RIGHT_VM_CHANNEL_RECV = 1u << 20,
    RIGHT_VM_CHANNEL_NOTIFY = 1u << 21,
    RIGHT_VM_CHANNEL_INSPECT = 1u << 22,
    RIGHT_VM_CREATE = 1u << 23,
};

// ---------------------------------------------------------------------------
// CapEntry — one slot in the per-process capability table.
// Exact layout from spec §5.5.  Must be exactly 64 bytes.
// ---------------------------------------------------------------------------
struct CapEntry {
    void*  target;          // pointer to the kernel object (Thread*, Ring*, …)
    u32    rights_mask;     // CapRights bitmask
    u32    generation;      // matches the generation in the handle; mismatch = revoked
    u16    type_tag;        // CapType
    u16    flags;           // reserved for future use
    u8     reserved[44];
};
static_assert(sizeof(CapEntry) == 64, "CapEntry must be 64 bytes (spec §5.5)");

// ---------------------------------------------------------------------------
// Per-process cap table.
// V0: dense flat array of CapEntry[kCapTableSize].
// Lookup is O(1): extract index from handle, check generation.
// ---------------------------------------------------------------------------
constexpr u32 kCapTableSize = 256; // fixed quota per process for V0

struct CapTable {
    CapEntry entries[kCapTableSize];
    u32      next_free; // bump pointer into entries[]
};

namespace cap {

void init();
CapHandle alloc(CapTable& table, CapType type, void* target, u32 rights);
CapEntry* lookup(CapTable& table, CapHandle handle, CapType expected_type);
CapEntry* lookup_any(CapTable& table, CapHandle handle);
void revoke(CapTable& table, CapHandle handle);
CapHandle dup(CapTable& table, CapHandle src, u32 rights_mask);

} // namespace cap
