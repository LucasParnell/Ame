#pragma once
// mochi/ring.h — userspace SPSC ring push/pop.
//
// Spec ref: §5.8 IPC, §5.9 Doorbells, §9 V0 IPC Ring Layout,
//           V1 Context-Switch Reduction
//
// This header is the userspace side of the Midix ring contract.  The layout
// MUST match the kernel's anko/ipc.h exactly.
//
// Tier 1 (zero kernel cycles):
//   Producer publishes with a release-store to head; consumer observes with an
//   acquire-load from head.  If receiver_about_to_sleep == 0, producer does not
//   enter the kernel.
//
// Tier 2 (doorbell path):
//   Consumer sets receiver_about_to_sleep, re-checks the ring, then calls
//   wait_doorbell(counter).  Producer observes the flag and calls notify.
//
// Important V1 rule:
//   ring_pop() is non-blocking.  Use ring_wait_pop() only in code paths where
//   it is correct for the current thread to sleep on the doorbell.

#include <cstdint>
#include <cstddef>
#include "mochi/syscall.h"

// ---------------------------------------------------------------------------
// Ring layout — must match kernel RingHeader / RingSlot exactly.
// ---------------------------------------------------------------------------
struct alignas(64) RingHead  { uint64_t v; uint8_t pad[56]; };
struct alignas(64) RingTail  { uint64_t v; uint8_t pad[56]; };
struct alignas(64) RingFlags {
    uint32_t receiver_about_to_sleep;
    uint32_t reserved0;
    uint64_t doorbell_counter_snapshot;
    uint8_t  pad[48];
};
struct alignas(64) RingSlot {
    uint32_t type;
    uint32_t length;
    uint64_t seq;
    uint8_t  payload[48];
};

struct RingPage {
    RingHead  head;      // 0x000
    RingTail  tail;      // 0x040
    RingFlags flags;     // 0x080
    uint8_t   _res[64];  // 0x0C0
    // slots begin at 0x100 — pointer arithmetic from ring_slots()
};

static inline RingSlot* ring_slots(RingPage* r) {
    return reinterpret_cast<RingSlot*>(reinterpret_cast<uint8_t*>(r) + 0x100);
}

// ---------------------------------------------------------------------------
// Producer — push one slot.
// Returns false if ring is full.
//
// Producer never notifies merely because the ring was empty.  The only V1
// reason to notify is that the consumer explicitly published
// receiver_about_to_sleep in the shared flags cache line.
// ---------------------------------------------------------------------------
static inline bool ring_push(RingPage* r, uint32_t order,
                             uint32_t type, const void* data, uint32_t len,
                             CapHandle doorbell_cap) {
    const uint64_t mask = (1ull << order) - 1ull;

    const uint64_t tail = __atomic_load_n(&r->tail.v, __ATOMIC_ACQUIRE);
    const uint64_t head = __atomic_load_n(&r->head.v, __ATOMIC_RELAXED);

    if (head - tail > mask) return false; // full

    RingSlot* slot = ring_slots(r) + (head & mask);
    slot->type   = type;
    slot->length = len < 48 ? len : 48;
    slot->seq    = head;

    if (data && len > 0) {
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < slot->length; ++i) {
            slot->payload[i] = src[i];
        }
    }

    // Publish with release ordering so the consumer's acquire-load of head sees
    // the completed slot contents.
    __atomic_store_n(&r->head.v, head + 1, __ATOMIC_RELEASE);

    // The sleep flag is a one-shot wake request.  Claim it by clearing the
    // flag before notifying.  This keeps the hot path out of the kernel and
    // prevents a burst of messages from turning one sleeping receiver into N
    // notify syscalls.
    //
    // Race safety with ring_wait_pop(): if the consumer has set the flag but
    // has not yet entered wait_doorbell(), this exchange still works because
    // sys_notify increments the doorbell counter.  The later wait sees the
    // counter changed and returns immediately instead of sleeping.
    (void)__atomic_exchange_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL);

    // Always notify after publishing.  Doorbells coalesce naturally, and this
    // avoids a lost-wake race in the sleep-flag fast path.
    (void)sys_notify(doorbell_cap);

    return true;
}

// ---------------------------------------------------------------------------
// Consumer — non-blocking pop.
// Returns false if ring is empty.  Does not yield, sleep, or touch doorbells.
// ---------------------------------------------------------------------------
static inline bool ring_pop(RingPage* r, uint32_t order, RingSlot* out) {
    const uint64_t mask = (1ull << order) - 1ull;
    const uint64_t tail = __atomic_load_n(&r->tail.v, __ATOMIC_RELAXED);
    const uint64_t head = __atomic_load_n(&r->head.v, __ATOMIC_ACQUIRE);

    if (tail == head) return false;

    RingSlot* src = ring_slots(r) + (tail & mask);
    out->type = src->type;
    out->length = src->length;
    out->seq = src->seq;
    for (uint32_t i = 0; i < 48; ++i) {
        out->payload[i] = src->payload[i];
    }

    __atomic_store_n(&r->tail.v, tail + 1, __ATOMIC_RELEASE);
    return true;
}

// Backward-compatible spelling for older call sites.  The doorbell argument is
// intentionally ignored: this helper is a try-pop now, not a blocking wait.
static inline bool ring_pop(RingPage* r, uint32_t order,
                            RingSlot* out, CapHandle /*caller_doorbell*/) {
    return ring_pop(r, order, out);
}

// ---------------------------------------------------------------------------
// Consumer — blocking pop.
//
// This is the only helper that may call wait_doorbell().  It implements the
// futex-shaped protocol:
//   1. Try to consume.
//   2. Read doorbell counter.
//   3. Publish receiver_about_to_sleep = 1.
//   4. Re-check the ring after publishing the flag.
//   5. If still empty, wait on the doorbell counter.
//   6. Clear the flag and retry.
// ---------------------------------------------------------------------------
static inline bool ring_wait_pop(RingPage* r, uint32_t order,
                                 RingSlot* out, CapHandle caller_doorbell) {
    for (;;) {
        if (ring_pop(r, order, out)) return true;

        const uint64_t counter = sys_doorbell_read(caller_doorbell);
        r->flags.doorbell_counter_snapshot = counter;
        __atomic_store_n(&r->flags.receiver_about_to_sleep, 1u, __ATOMIC_RELEASE);

        // Close the race where a producer publishes after the empty check but
        // before the sleep flag became visible.
        if (ring_pop(r, order, out)) {
            __atomic_store_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_RELEASE);
            return true;
        }

        const int64_t rc = sys_wait_doorbell(caller_doorbell, counter);
        __atomic_store_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_RELEASE);

        if (rc < 0) return false;
        // Wake or immediate return: retry the pop.
    }
}
