#pragma once
// anko/ipc.h — IPC data layouts: SPSC ring and doorbell.
//
// Spec ref: §5.8 IPC, §5.9 Doorbells, §9 V0 IPC Ring Layout
//
// Ring page layout (spec §9):
//   0x000  64B   producer head cache line
//   0x040  64B   consumer tail cache line
//   0x080  64B   flags cache line
//   0x0C0  64B   reserved
//   0x100  N*64  slots   (N = 2^order, must be power of two)
//
// Producer ordering: relaxed stores into slot, STLR into head.
// Consumer ordering: LDAR from head, relaxed loads from slot.
// No DMB unless ring crosses device memory.

#include "anko/types.h"
#include "anko/cap.h"
#include "anko/syscall.h"

struct Thread;

// ---------------------------------------------------------------------------
// Ring head line — producer-owned, one cache line (spec §9)
// ---------------------------------------------------------------------------
struct alignas(64) RingHeadLine {
    u64 head;
    u8  pad[56];
};
static_assert(sizeof(RingHeadLine) == 64);

// ---------------------------------------------------------------------------
// Ring tail line — consumer-owned, one cache line (spec §9)
// ---------------------------------------------------------------------------
struct alignas(64) RingTailLine {
    u64 tail;
    u8  pad[56];
};
static_assert(sizeof(RingTailLine) == 64);

// ---------------------------------------------------------------------------
// Flags line — shared coordination (spec §9)
//
// receiver_about_to_sleep:
//   Set by consumer before blocking; cleared on wake.
//   Producer checks this (relaxed load) before deciding to call sys_notify.
//   If 0: receiver is polling → no syscall needed (Tier 1 path).
//   If 1: receiver is sleeping → must call sys_notify (Tier 2 path).
// ---------------------------------------------------------------------------
struct alignas(64) RingFlagsLine {
    u32 receiver_about_to_sleep;
    u32 reserved0;
    u64 doorbell_counter_snapshot; // consumer records doorbell counter before sleeping
    u8  pad[48];
};
static_assert(sizeof(RingFlagsLine) == 64);

// ---------------------------------------------------------------------------
// Ring slot — 64 bytes, inline payload only (V0 — spec §9)
// V0 supports inline payloads only; external arena payloads deferred to V1.
// ---------------------------------------------------------------------------
struct alignas(64) RingSlot {
    u32 type;
    u32 length;
    u64 seq;
    u8  payload[48];
};
static_assert(sizeof(RingSlot) == 64);

// ---------------------------------------------------------------------------
// Ring header — first 256 bytes of the shared ring page (spec §9)
// ---------------------------------------------------------------------------
struct RingHeader {
    RingHeadLine  head;      // offset 0x000
    RingTailLine  tail;      // offset 0x040
    RingFlagsLine flags;     // offset 0x080
    u8            _reserved[64]; // offset 0x0C0
};                           // total 0x100
static_assert(sizeof(RingHeader) == 0x100);
static_assert(__builtin_offsetof(RingHeader, head)  == 0x000);
static_assert(__builtin_offsetof(RingHeader, tail)  == 0x040);
static_assert(__builtin_offsetof(RingHeader, flags) == 0x080);

// Slots begin immediately after the header.
// Ring capacity in slots: 2^order  (V0: SPSC, power-of-two size).
constexpr usize ring_slot_offset(u32 order, u32 index) {
    return sizeof(RingHeader) + (index & ((1u << order) - 1)) * sizeof(RingSlot);
}

// ---------------------------------------------------------------------------
// Ring kernel object — tracks a live ring shared between two processes.
// ---------------------------------------------------------------------------
struct Ring {
    PhysAddr phys_base;  // physical page(s) backing the ring
    u32      order;      // capacity = 2^order slots
    u32      producer_pid;
    u32      consumer_pid;
    u32      _pad;
};

// ---------------------------------------------------------------------------
// Doorbell kernel object (spec §5.9)
//
// notify(cap):     atomically increment counter;
//                  if waiter != null, wake that thread.
// wait(cap, expected): if counter == expected, sleep and set waiter;
//                      if counter != expected, return immediately.
//
// This gives the futex-shaped race-free handoff required by the architecture.
// ---------------------------------------------------------------------------
struct alignas(64) Doorbell {
    u64  counter;       // incremented by notify; compared by wait
    Thread* waiter;     // sleeping thread, or null
    u32  owner_pid;
    u32  rights;
    u32  lock;
    u8   pad[36];
};
static_assert(sizeof(Doorbell) == 64);

namespace ipc {

void init();

CapHandle ring_create(u32 pid, u32 order, u32 flags);
CapHandle ring_share(u32 owner_pid, CapHandle ring_cap, u32 target_pid, u32 rights);
Ring* ring_from_cap(u32 pid, CapHandle cap);
Ring* ring_create_object(u32 pid, u32 order, u32 flags);
CapHandle ring_cap_alloc(u32 target_pid, Ring* ring, u32 rights);

CapHandle doorbell_create(u32 pid);
CapHandle doorbell_share(u32 owner_pid, CapHandle db_cap, u32 target_pid, u32 rights);
Doorbell* doorbell_create_object(u32 pid);
CapHandle doorbell_cap_alloc(u32 target_pid, Doorbell* db, u32 rights);
SysError notify(CapHandle cap, u32 caller_pid);
SysError notify_object(Doorbell* db);
SysError wait_doorbell(CapHandle cap, u64 expected, u32 caller_pid);
u64 doorbell_counter(CapHandle cap, u32 caller_pid);
void doorbell_clear_waiter(Thread* thread);
u64 doorbell_sleep_count();
u64 doorbell_wake_count();
u64 doorbell_wait_calls();
u64 doorbell_wait_immediate();
u64 notify_calls();
u64 notify_no_waiter();
u64 notify_woke_thread();
u64 doorbell_local_wake_count();
u64 doorbell_remote_wake_count();

} // namespace ipc
