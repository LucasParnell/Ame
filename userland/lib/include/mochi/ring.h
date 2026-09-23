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
#include "midix/abi/ring.h"

// ---------------------------------------------------------------------------
// Ring layout — must match kernel RingHeader / RingSlot exactly.
// ---------------------------------------------------------------------------
using RingHead = MxRingHeadLine;
using RingTail = MxRingTailLine;
using RingFlags = MxRingFlagsLine;
using RingSlot = MxRingSlot;
using RingPage = MxRingPage;

static_assert(sizeof(RingHead) == sizeof(MxRingHeadLine), "userspace RingHead diverged from ABI");
static_assert(sizeof(RingTail) == sizeof(MxRingTailLine), "userspace RingTail diverged from ABI");
static_assert(sizeof(RingFlags) == sizeof(MxRingFlagsLine), "userspace RingFlags diverged from ABI");
static_assert(sizeof(RingSlot) == sizeof(MxRingSlot), "userspace RingSlot diverged from ABI");
static_assert(sizeof(RingPage) == sizeof(MxRingPage), "userspace RingPage diverged from ABI");

static inline RingSlot* ring_slots(RingPage* r) {
    // The header offset lives in the ABI (mx_ring_slots); never restate 0x100.
    return mx_ring_slots(r);
}

// ---------------------------------------------------------------------------
// Producer — push one slot.
// Returns false if ring is full.
//
// Producer never notifies merely because the ring was empty.  The only V1
// reason to notify is that the consumer explicitly published
// receiver_about_to_sleep in the shared flags cache line.
// ---------------------------------------------------------------------------
static inline bool ring_try_push(RingPage* r, uint32_t order,
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
    const uint32_t was_sleeping =
        __atomic_exchange_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL);
    if (was_sleeping != 0) {
        (void)mx_doorbell_notify(doorbell_cap);
    }

    return true;
}

// Backward-compatible spelling. This is a non-blocking try-push; it never waits
// for space and only enters the kernel when the consumer has published a sleep
// request in the shared flags line.
static inline bool ring_push(RingPage* r, uint32_t order,
                             uint32_t type, const void* data, uint32_t len,
                             CapHandle doorbell_cap) {
    return ring_try_push(r, order, type, data, len, doorbell_cap);
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

// Producer convenience helper for services that are allowed to back-pressure
// themselves. It never blocks in the kernel; it yields in userspace until the
// consumer drains space. Do not use from latency-critical paths unless bounded
// by a caller-owned retry budget.
static inline bool ring_send_spinning(RingPage* r, uint32_t order,
                                      uint32_t type, const void* data, uint32_t len,
                                      CapHandle doorbell_cap, uint32_t max_yields) {
    for (uint32_t i = 0; i <= max_yields; ++i) {
        if (ring_try_push(r, order, type, data, len, doorbell_cap)) return true;
        sys_yield();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Byte runs
//
// A byte run longer than the inline payload becomes ordered slots on one ring
// (the terminal contract, midix/abi/protocols/terminal.h). The chunking lives
// here once so every byte-stream producer splits a run identically instead of
// re-deriving the 48-byte boundary in each service.
// ---------------------------------------------------------------------------

// Push `len` bytes of `data` as ordered <=MX_RING_INLINE_PAYLOAD slots. Returns
// the number of bytes accepted. A short return means the ring filled mid-run: the
// caller owns the policy for the tail (never silently treated as success).
static inline uint32_t ring_push_bytes(RingPage* r, uint32_t order, uint32_t type,
                                       const void* data, uint32_t len,
                                       CapHandle doorbell_cap) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t off = 0;
    while (off < len) {
        const uint32_t remaining = len - off;
        const uint32_t chunk = remaining > MX_RING_INLINE_PAYLOAD
                                   ? MX_RING_INLINE_PAYLOAD
                                   : remaining;
        if (!ring_try_push(r, order, type, p + off, chunk, doorbell_cap)) break;
        off += chunk;
    }
    return off;
}

// Spinning variant: each chunk may yield up to `max_yields` times before the run
// is abandoned. Returns the number of bytes accepted.
static inline uint32_t ring_push_bytes_spinning(RingPage* r, uint32_t order, uint32_t type,
                                                const void* data, uint32_t len,
                                                CapHandle doorbell_cap, uint32_t max_yields) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t off = 0;
    while (off < len) {
        const uint32_t remaining = len - off;
        const uint32_t chunk = remaining > MX_RING_INLINE_PAYLOAD
                                   ? MX_RING_INLINE_PAYLOAD
                                   : remaining;
        if (!ring_send_spinning(r, order, type, p + off, chunk, doorbell_cap, max_yields)) break;
        off += chunk;
    }
    return off;
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

// ---------------------------------------------------------------------------
// RingReaderSet — one consumer draining many SPSC rings through a shared
// doorbell.
//
// A service that reads several producer rings can only sleep on one wakeup, so
// the single-ring protocol in ring_wait_pop() has to be fanned out: publish
// receiver_about_to_sleep on every ring, re-check them all, then wait on the
// shared doorbell. That is what drain()/wait() do here, so no service ever
// hand-rolls it again. This is a view — the caller owns the ring array.
// ---------------------------------------------------------------------------
namespace mochi {

// ---------------------------------------------------------------------------
// Ring — one mapped SPSC ring plus the doorbell that wakes its consumer.
//
// A Ring is a value view over shared memory (the caller maps and owns the page).
// It carries EVERY operation the raw ring has: non-blocking / spinning / deadline
// publish; non-blocking / blocking / deadline consume; a byte run split into
// ordered inline slots; typed inline messages; queue depth; and the
// receiver-sleep flag the doorbell protocol reads. Wrapping a ring in a Ring
// therefore costs no functionality — it only names the geometry and the
// doorbell, and raw() returns the RingPage for anything not covered here.
// ---------------------------------------------------------------------------
struct Ring {
    RingPage* ring = nullptr;
    uint32_t order = 0;
    CapHandle doorbell = kNullCap;

    bool valid() const { return ring != nullptr; }
    RingPage* raw() const { return ring; }
    uint32_t capacity() const { return mx_ring_capacity(order); }
    uint32_t mask() const { return mx_ring_mask(order); }

    // --- publish (producer) -------------------------------------------------
    bool try_push(uint32_t type, const void* data, uint32_t len) const {
        return ring != nullptr && ring_try_push(ring, order, type, data, len, doorbell);
    }
    bool push(uint32_t type, const void* data, uint32_t len, uint32_t max_yields = 1024) const {
        return ring != nullptr && ring_send_spinning(ring, order, type, data, len, doorbell, max_yields);
    }
    bool push_bounded(uint32_t type, const void* data, uint32_t len, mx_u64_t deadline_ns) const {
        if (!ring) return false;
        for (;;) {
            if (ring_try_push(ring, order, type, data, len, doorbell)) return true;
            if (mx_clock_ns() >= deadline_ns) return false;
            sys_yield();
        }
    }
    // A byte run as ordered inline slots; returns bytes accepted. max_yields 0 is
    // non-blocking (stop at the first full slot), otherwise each chunk yields.
    uint32_t push_bytes(uint32_t type, const void* data, uint32_t len,
                        uint32_t max_yields = 0) const {
        if (!ring) return 0;
        return max_yields == 0
                   ? ring_push_bytes(ring, order, type, data, len, doorbell)
                   : ring_push_bytes_spinning(ring, order, type, data, len, doorbell, max_yields);
    }
    template <typename T>
    bool try_push_msg(uint32_t type, const T& msg) const {
        static_assert(sizeof(T) <= MX_RING_INLINE_PAYLOAD, "message exceeds one inline payload");
        return ring_try_push(ring, order, type, &msg, static_cast<uint32_t>(sizeof(T)), doorbell);
    }
    template <typename T>
    bool push_msg(uint32_t type, const T& msg, uint32_t max_yields = 1024) const {
        static_assert(sizeof(T) <= MX_RING_INLINE_PAYLOAD, "message exceeds one inline payload");
        return ring != nullptr &&
               ring_send_spinning(ring, order, type, &msg, static_cast<uint32_t>(sizeof(T)),
                                  doorbell, max_yields);
    }

    // --- consume (consumer) -------------------------------------------------
    bool try_pop(RingSlot* out) const { return ring != nullptr && ring_pop(ring, order, out); }
    bool wait_pop(RingSlot* out) const {
        return ring != nullptr && doorbell != kNullCap && ring_wait_pop(ring, order, out, doorbell);
    }
    bool pop_bounded(RingSlot* out, mx_u64_t deadline_ns) const {
        if (!ring) return false;
        for (;;) {
            if (ring_pop(ring, order, out)) return true;
            if (mx_clock_ns() >= deadline_ns) return false;
            sys_yield();
        }
    }

    // Slots queued but not yet consumed, bounded by the capacity.
    uint32_t depth() const {
        if (!ring) return 0;
        const uint64_t tail = __atomic_load_n(&ring->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t head = __atomic_load_n(&ring->head.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        const uint64_t capacity = mx_ring_capacity(order);
        return pending > capacity ? static_cast<uint32_t>(capacity)
                                  : static_cast<uint32_t>(pending);
    }

    // --- doorbell / receiver-sleep flag -------------------------------------
    uint64_t doorbell_counter() const { return sys_doorbell_read(doorbell); }
    void publish_sleep(uint64_t counter) const {
        if (!ring) return;
        ring->flags.doorbell_counter_snapshot = counter;
        __atomic_store_n(&ring->flags.receiver_about_to_sleep, 1u, __ATOMIC_RELEASE);
    }
    void clear_sleep() const {
        if (!ring) return;
        __atomic_store_n(&ring->flags.receiver_about_to_sleep, 0u, __ATOMIC_RELEASE);
    }
};

// ---------------------------------------------------------------------------
// RingReaderSet — one consumer draining many SPSC rings through a shared
// doorbell.
//
// A service reading several producer rings can only sleep on one wakeup, so the
// single-ring protocol in ring_wait_pop() is fanned out: publish
// receiver_about_to_sleep on every ring, re-check them all, then wait on the
// shared doorbell. That is what drain()/wait() do here. This is a view — the
// caller owns the ring array.
// ---------------------------------------------------------------------------
struct RingReaderSet {
    RingPage* const* rings;
    uint32_t count;
    uint32_t order;
    CapHandle doorbell;

    // Consume every slot currently available on every ring, calling on_slot for
    // each. Never blocks. Returns true if any slot was consumed.
    template <typename OnSlot>
    bool drain(OnSlot on_slot) const {
        bool any = false;
        RingSlot slot{};
        for (uint32_t i = 0; i < count; ++i) {
            const Ring ring{rings[i], order, doorbell};
            while (ring.try_pop(&slot)) {
                on_slot(slot);
                any = true;
            }
        }
        return any;
    }

    // Block until at least one slot is consumed, running on_slot for every slot
    // drained. This is the multi-ring form of the ring_wait_pop() protocol: the
    // publish-sleep / re-check / wait ordering closes the wake race against the
    // shared doorbell. Returns false only if the doorbell wait itself fails.
    template <typename OnSlot>
    bool wait(OnSlot on_slot) const {
        for (;;) {
            if (drain(on_slot)) return true;

            const uint64_t counter = sys_doorbell_read(doorbell);
            publish_sleep(counter);

            // Close the race where a producer publishes after the empty check
            // but before the sleep flag became visible.
            if (drain(on_slot)) {
                clear_sleep();
                return true;
            }

            // The flag must stay set across the wait: a producer only notifies
            // the doorbell while it observes the sleep request. Clearing it
            // early would let a publication go unnotified and lose the wake.
            const int64_t rc = sys_wait_doorbell(doorbell, counter);
            clear_sleep();
            if (rc < 0) return false;
        }
    }

private:
    void publish_sleep(uint64_t counter) const {
        for (uint32_t i = 0; i < count; ++i) {
            const Ring ring{rings[i], order, doorbell};
            ring.publish_sleep(counter);
        }
    }
    void clear_sleep() const {
        for (uint32_t i = 0; i < count; ++i) {
            const Ring ring{rings[i], order, doorbell};
            ring.clear_sleep();
        }
    }
};

} // namespace mochi
