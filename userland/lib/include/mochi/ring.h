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
#include "mochi/message.h"
#include "midix/abi/ring.h"
#include "libk/cpu.h"

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

// Geometry must fit the ABI's 32-bit capacity. The caller still owns the
// mapping and must provide storage for mx_ring_page_bytes(order) bytes.
static inline bool ring_valid(const RingPage* r, uint32_t order) {
    return r != nullptr && order < sizeof(mx_u32_t) * 8;
}

// Fill unpublished, producer-owned storage. Keeping construction separate from
// publication lets a contiguous run amortize head/peer/wakeup traffic.
static inline void ring_write_slot(RingSlot& slot, uint32_t type, const void* data,
                                    uint32_t length, uint64_t sequence) {
    slot.type = type;
    slot.length = length < MX_RING_INLINE_PAYLOAD ? length : MX_RING_INLINE_PAYLOAD;
    slot.seq = sequence;
    const auto payload = libk::as_bytes(slot.payload);
    (void)libk::copy_n(payload, libk::as_bytes(data, length), slot.length);
    (void)libk::zero(libk::slice(payload, slot.length, payload.size - slot.length));
}

static inline void ring_publish(RingPage* r, uint64_t head, CapHandle doorbell,
                                bool* deferred_notify = nullptr, bool peer_polls = false) {
    __atomic_store_n(&r->head.v, head, __ATOMIC_RELEASE);
    // A negotiated polling peer never arms receiver_about_to_sleep, so there is
    // no waiter to claim and the RMW below would only dirty the shared flags
    // line on the far core. The caller must guarantee the peer stays in its
    // poll loop for the whole channel policy, not just around one publish.
    if (peer_polls) return;
    // Keep the existing unconditional RMW handshake. A speculative flag load
    // before this exchange would need an additional wait/wake ordering proof.
    if (__atomic_exchange_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL)) {
        if (deferred_notify) *deferred_notify = true;
        else (void)mx_doorbell_notify(doorbell);
    }
}

static inline void ring_release(RingPage* r, uint64_t tail, CapHandle space_doorbell = kNullCap,
                                bool peer_polls = false) {
    __atomic_store_n(&r->tail.v, tail, __ATOMIC_RELEASE);
    if (!peer_polls && space_doorbell &&
        __atomic_exchange_n(&r->space.producer_about_to_sleep, 0u, __ATOMIC_ACQ_REL))
        (void)mx_doorbell_notify(space_doorbell);
}

// Both ends use an acquire/release RMW on the one-shot sleep flag. If
// publication wins, this acquire observes the producer's release and therefore
// its head store. If arming wins, the producer observes 1 and notifies. A mere
// release-store here permits the following head check to miss publication.
static inline void ring_publish_sleep(RingPage* r, uint64_t counter) {
    if (!r) return;
    r->flags.doorbell_counter_snapshot = counter;
    (void)__atomic_exchange_n(&r->flags.receiver_about_to_sleep, 1u, __ATOMIC_ACQ_REL);
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
                                 CapHandle doorbell_cap, const uint64_t* sequence = nullptr,
                                 bool* deferred_notify = nullptr, bool peer_polls = false) {
    if (!ring_valid(r, order) || (!data && len != 0)) return false;
    const uint64_t mask = mx_ring_mask(order);

    const uint64_t tail = __atomic_load_n(&r->tail.v, __ATOMIC_ACQUIRE);
    const uint64_t head = __atomic_load_n(&r->head.v, __ATOMIC_RELAXED);

    if (head - tail > mask) return false; // full

    RingSlot* slot = ring_slots(r) + (head & mask);
    ring_write_slot(*slot, type, data, len, sequence ? *sequence : head);

    // Publish with release ordering so the consumer's acquire-load of head sees
    // the completed slot contents.

    // The sleep flag is a one-shot wake request.  Claim it by clearing the
    // flag before notifying.  This keeps the hot path out of the kernel and
    // prevents a burst of messages from turning one sleeping receiver into N
    // notify syscalls.
    //
    // Race safety with ring_wait_pop(): if the consumer has set the flag but
    // has not yet entered wait_doorbell(), this exchange still works because
    // sys_notify increments the doorbell counter.  The later wait sees the
    // counter changed and returns immediately instead of sleeping.
    ring_publish(r, head + 1, doorbell_cap, deferred_notify, peer_polls);

    return true;
}

// Preserve a protocol-owned correlation token when forwarding a complete frame.
static inline bool ring_try_push_frame(RingPage* r, uint32_t order,
                                       const RingSlot& frame, CapHandle doorbell,
                                       bool peer_polls = false) {
    return mochi::message_valid(frame) &&
           ring_try_push(r, order, frame.type, frame.payload, frame.length, doorbell, &frame.seq,
                         nullptr, peer_polls);
}

// Publish the valid prefix that fits. Input remains caller-owned; no persistent
// cursor cache can go stale when another value view uses these same helpers.
static inline uint32_t ring_try_push_frames(RingPage* r, uint32_t order,
                                            const RingSlot* frames, uint32_t count,
                                            CapHandle doorbell, bool peer_polls = false) {
    if (!ring_valid(r, order) || !frames || !count) return 0;
    const uint64_t capacity = mx_ring_capacity(order), mask = capacity - 1;
    const uint64_t head = __atomic_load_n(&r->head.v, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&r->tail.v, __ATOMIC_ACQUIRE);
    const uint64_t pending = head - tail;
    if (pending >= capacity) return 0;
    const uint32_t available = static_cast<uint32_t>(capacity - pending);
    const uint32_t limit = count < available ? count : available;
    auto* slots = ring_slots(r);
    uint32_t written = 0;
    while (written < limit && mochi::message_valid(frames[written])) {
        const auto& frame = frames[written];
        ring_write_slot(slots[(head + written) & mask], frame.type, frame.payload,
                        frame.length, frame.seq);
        ++written;
    }
    if (written) ring_publish(r, head + written, doorbell, nullptr, peer_polls);
    return written;
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
static inline bool ring_pop_space(RingPage* r, uint32_t order, RingSlot* out,
                                  CapHandle space_doorbell, bool peer_polls = false) {
    if (!ring_valid(r, order) || !out) return false;
    const uint64_t mask = mx_ring_mask(order);
    const uint64_t tail = __atomic_load_n(&r->tail.v, __ATOMIC_RELAXED);
    const uint64_t head = __atomic_load_n(&r->head.v, __ATOMIC_ACQUIRE);

    // Match the batch reader's contract. A corrupted producer cursor must not
    // make an overwritten slot appear readable or advance consumer ownership.
    const uint64_t pending = head - tail;
    if (!pending || pending > mask + 1) return false;

    RingSlot* src = ring_slots(r) + (tail & mask);
    out->type = src->type;
    out->length = src->length;
    out->seq = src->seq;
    (void)libk::copy_exact(libk::as_bytes(out->payload), libk::as_bytes(src->payload));

    ring_release(r, tail + 1, space_doorbell, peer_polls);
    return true;
}

static inline bool ring_pop(RingPage* r, uint32_t order, RingSlot* out) {
    return ring_pop_space(r, order, out, kNullCap);
}

// Backward-compatible spelling for older call sites.  The doorbell argument is
// intentionally ignored: this helper is a try-pop now, not a blocking wait.
static inline bool ring_pop(RingPage* r, uint32_t order,
                            RingSlot* out, CapHandle /*caller_doorbell*/) {
    return ring_pop(r, order, out);
}

// Copy a bounded snapshot, then release the consumed slots together. No pointer
// into shared storage escapes; producers may reuse it as soon as tail advances.
static inline uint32_t ring_pop_batch(RingPage* r, uint32_t order,
                                      RingSlot* out, uint32_t count,
                                      CapHandle space_doorbell = kNullCap, bool peer_polls = false) {
    if (!ring_valid(r, order) || !out || !count) return 0;
    const uint64_t capacity = mx_ring_capacity(order), mask = capacity - 1;
    const uint64_t tail = __atomic_load_n(&r->tail.v, __ATOMIC_RELAXED);
    const uint64_t head = __atomic_load_n(&r->head.v, __ATOMIC_ACQUIRE);
    const uint64_t pending = head - tail;
    if (!pending || pending > capacity) return 0;
    const uint32_t taken = pending < count ? static_cast<uint32_t>(pending) : count;
    const auto* slots = ring_slots(r);
    for (uint32_t i = 0; i < taken; ++i) out[i] = slots[(tail + i) & mask];
    ring_release(r, tail + taken, space_doorbell, peer_polls);
    return taken;
}

// Producer convenience helper for services that are allowed to back-pressure
// themselves. It never blocks in the kernel; it yields in userspace until the
// consumer drains space. Do not use from latency-critical paths unless bounded
// by a caller-owned retry budget.
static inline bool ring_send_spinning(RingPage* r, uint32_t order,
                                      uint32_t type, const void* data, uint32_t len,
                                      CapHandle doorbell_cap, uint32_t max_yields,
                                      bool peer_polls = false) {
    if (!ring_valid(r, order) || (!data && len != 0)) return false;
    for (;;) {
        if (ring_try_push(r, order, type, data, len, doorbell_cap, nullptr, nullptr, peer_polls)) return true;
        if (max_yields == 0) return false;
        --max_yields;
        sys_yield();
    }
}

// ---------------------------------------------------------------------------
// Byte runs
//
// A byte run longer than the inline payload becomes ordered slots on one ring
// (the terminal contract, midix/abi/protocols/terminal.h). The chunking lives
// here once so every byte-stream producer splits a run identically instead of
// re-deriving the 48-byte boundary in each service.
// ---------------------------------------------------------------------------

// Push a byte run as ordered inline slots. Each chunk may yield at most
// max_yields times. A short return reports the accepted prefix; the caller owns
// the retry/drop policy for the remaining bytes.
static inline uint32_t ring_push_bytes_spinning(RingPage* r, uint32_t order, uint32_t type,
                                                const void* data, uint32_t len,
                                                CapHandle doorbell_cap, uint32_t max_yields,
                                                bool peer_polls = false) {
    if (!data || !ring_valid(r, order)) return 0;
    const auto* p = static_cast<const uint8_t*>(data);
    const uint64_t capacity = mx_ring_capacity(order), mask = capacity - 1;
    auto* slots = ring_slots(r);
    uint32_t off = 0;
    uint32_t retries = max_yields;
    while (off < len) {
        const uint64_t head = __atomic_load_n(&r->head.v, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(&r->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending >= capacity) {
            if (!retries) break;
            --retries;
            sys_yield();
            continue;
        }
        // Bound one publication to 16 cache lines. Logs/terminal byte runs
        // amortize peer loads and wake RMWs without building a staging array.
        const uint64_t available = capacity - pending;
        const uint32_t limit = available < 16 ? static_cast<uint32_t>(available) : 16;
        uint32_t written = 0;
        while (written < limit && off < len) {
            const uint32_t remaining = len - off;
            const uint32_t chunk = remaining < MX_RING_INLINE_PAYLOAD ? remaining : MX_RING_INLINE_PAYLOAD;
            ring_write_slot(slots[(head + written) & mask], type, p + off, chunk, head + written);
            off += chunk;
            ++written;
        }
        ring_publish(r, head + written, doorbell_cap, nullptr, peer_polls);
        retries = max_yields;
    }
    return off;
}

// Non-blocking byte run: stop at the first full slot without yielding.
static inline uint32_t ring_push_bytes(RingPage* r, uint32_t order, uint32_t type,
                                       const void* data, uint32_t len,
                                       CapHandle doorbell_cap) {
    return ring_push_bytes_spinning(r, order, type, data, len, doorbell_cap, 0);
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
// Wait for readable data without consuming it. Transport adapters that separate
// wait() from pop() must use this same re-check after publishing the sleep flag.
static inline bool ring_wait_readable(RingPage* r, uint32_t order, CapHandle doorbell) {
    if (!ring_valid(r, order) || doorbell == kNullCap) return false;
    const auto readable = [&] {
        const auto tail = __atomic_load_n(&r->tail.v, __ATOMIC_RELAXED);
        return __atomic_load_n(&r->head.v, __ATOMIC_ACQUIRE) != tail;
    };
    for (;;) {
        if (readable()) return true;
        const uint64_t counter = sys_doorbell_read(doorbell);
        ring_publish_sleep(r, counter);
        if (readable()) {
            __atomic_store_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_RELEASE);
            return true;
        }
        const int64_t rc = sys_wait_doorbell(doorbell, counter);
        __atomic_store_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_RELEASE);
        if (rc < 0) return false;
    }
}

static inline bool ring_wait_pop(RingPage* r, uint32_t order,
                                 RingSlot* out, CapHandle caller_doorbell) {
    if (!out || !ring_valid(r, order) || caller_doorbell == kNullCap) return false;
    do {
        if (ring_pop(r, order, out)) return true;
    } while (ring_wait_readable(r, order, caller_doorbell));
    return false;
}

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
    CapHandle handle = kNullCap;
    CapHandle space_doorbell = kNullCap;
    // Exclusive sender-owned storage, never placed in shared memory. A caller
    // deferring a wake must flush it before blocking or leaving the operation.
    bool* deferred_notify = nullptr;
    // Set only when the peer has agreed to poll for the whole channel policy.
    // Publishes then skip the one-shot wake handshake; see ring_publish().
    bool peer_polls = false;
    // Relax iterations this ring may spin before arming its sleep flag.
    // refresh_poll_budget() seeds it from the principal policy.
    uint32_t poll_budget = 0;

    bool valid() const { return ring_valid(ring, order); }
    RingPage* raw() const { return ring; }
    uint32_t capacity() const { return valid() ? mx_ring_capacity(order) : 0; }
    uint32_t mask() const { return valid() ? mx_ring_mask(order) : 0; }

    // --- publish (producer) -------------------------------------------------
    bool try_push(uint32_t type, const void* data, uint32_t len) const {
        return ring != nullptr && ring_try_push(ring, order, type, data, len, doorbell, nullptr,
                                                deferred_notify, peer_polls);
    }
    bool try_push_frame(const RingSlot& frame) const {
        return ring != nullptr && ring_try_push_frame(ring, order, frame, doorbell, peer_polls);
    }
    uint32_t try_push_frames(const RingSlot* frames, uint32_t count) const {
        return ring_try_push_frames(ring, order, frames, count, doorbell, peer_polls);
    }
    bool push(uint32_t type, const void* data, uint32_t len, uint32_t max_yields = 1024) const {
        return ring != nullptr &&
               ring_send_spinning(ring, order, type, data, len, doorbell, max_yields, peer_polls);
    }
    bool push_bounded(uint32_t type, const void* data, uint32_t len, mx_u64_t deadline_ns) const {
        if (!valid() || (!data && len != 0)) return false;
        for (;;) {
            if (ring_try_push(ring, order, type, data, len, doorbell, nullptr, nullptr, peer_polls))
                return true;
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
                   ? ring_push_bytes_spinning(ring, order, type, data, len, doorbell, 0, peer_polls)
                   : ring_push_bytes_spinning(ring, order, type, data, len, doorbell, max_yields,
                                              peer_polls);
    }
    template <typename T>
    bool try_push_msg(uint32_t type, const T& msg) const {
        return try_push(type, &msg, message_size<T>());
    }
    template <typename T>
    bool push_msg(uint32_t type, const T& msg, uint32_t max_yields = 1024) const {
        return push(type, &msg, message_size<T>(), max_yields);
    }

    // --- consume (consumer) -------------------------------------------------
    bool try_pop(RingSlot* out) const {
        return ring != nullptr && ring_pop_space(ring, order, out, space_doorbell, peer_polls);
    }
    uint32_t try_pop_batch(RingSlot* out, uint32_t count) const {
        return ring_pop_batch(ring, order, out, count, space_doorbell, peer_polls);
    }
    // Read the principal's adaptive budget once; 0 leaves the caller's own
    // explicit budget in charge.
    uint32_t refresh_poll_budget() {
        const uint32_t policy = mx_ipc_policy_budget();
        if (policy) poll_budget = policy;
        return poll_budget;
    }
    // Wait with the ring's policy budget. Same-core peers should keep it 0:
    // they cannot make progress while this thread spins.
    bool wait_readable_auto() const {
        return handle ? wait_cursor(false, 1, poll_budget)
                      : ring_wait_readable(ring, order, doorbell);
    }
    bool wait_readable(uint32_t poll_budget = 0) const {
        if (!handle) return ring_wait_readable(ring, order, doorbell);
        return wait_cursor(false, 1, poll_budget);
    }
    bool wait_readable_after_notify(CapHandle notify, uint32_t flags = 0) const {
        if (!handle) return mx_doorbell_notify(notify) == MX_OK && wait_readable();
        if (!valid() || !doorbell) return false;
        const uint64_t tail = __atomic_load_n(&ring->tail.v, __ATOMIC_RELAXED);
        (void)__atomic_exchange_n(&ring->flags.receiver_about_to_sleep, 1u, __ATOMIC_ACQ_REL);
        const auto rc = sys_notify_wait(notify, handle, doorbell, tail, flags);
        __atomic_store_n(&ring->flags.receiver_about_to_sleep, 0u, __ATOMIC_RELEASE);
        return rc >= 0;
    }
    bool wait_writable(uint32_t slots = 1, uint32_t poll_budget = 0) const {
        if (!handle || !space_doorbell) return false;
        return wait_cursor(true, slots, poll_budget);
    }
    bool wait_cursor(bool writable, uint32_t slots = 1, uint32_t poll_budget = 0) const {
        if (!valid() || !slots || slots > capacity()) return false;
        auto* flag = writable ? &ring->space.producer_about_to_sleep : &ring->flags.receiver_about_to_sleep;
        for (;;) {
            const uint64_t head = __atomic_load_n(&ring->head.v, __ATOMIC_ACQUIRE);
            const uint64_t tail = __atomic_load_n(&ring->tail.v, __ATOMIC_ACQUIRE);
            if (head - tail > capacity()) return false;
            if (writable ? head - tail <= capacity() - slots : head != tail) return true;
            // Opt-in only when the peer can run concurrently. Exhaustion falls
            // through to the same race-free kernel wait; there is no busy wait
            // while a same-core peer is descheduled.
            if (poll_budget) {
                --poll_budget;
                libk::cpu::relax();
                continue;
            }
            (void)__atomic_exchange_n(flag, 1u, __ATOMIC_ACQ_REL);
            const auto cursor = writable ? tail : head;
            const auto rc = sys_wait_ring(handle, writable ? space_doorbell : doorbell,
                                           cursor, writable ? MX_RING_WAIT_WRITABLE : MX_RING_WAIT_READABLE);
            __atomic_store_n(flag, 0u, __ATOMIC_RELEASE);
            if (rc < 0) return false;
        }
    }
    bool wait_pop(RingSlot* out) const {
        if (!ring || !doorbell || !out) return false;
        do { if (try_pop(out)) return true; } while (wait_readable());
        return false;
    }
    bool pop_bounded(RingSlot* out, mx_u64_t deadline_ns) const {
        if (!valid() || !out) return false;
        for (;;) {
            if (try_pop(out)) return true;
            if (mx_clock_ns() >= deadline_ns) return false;
            sys_yield();
        }
    }

    // Non-blocking typed read consumes one slot, including a malformed or
    // unrelated message. Blocking typed read skips those until a valid match.
    template <typename T>
    bool try_pop_msg(uint32_t type, T* out) const {
        if (!out) return false;
        RingSlot slot{};
        return try_pop(&slot) && message_get(slot, out, type);
    }
    template <typename T>
    bool wait_pop_msg(uint32_t type, T* out) const {
        if (!out) return false;
        RingSlot slot{};
        while (wait_pop(&slot)) {
            if (message_get(slot, out, type)) return true;
        }
        return false;
    }

    // Slots queued but not yet consumed, bounded by the capacity.
    uint32_t depth() const {
        if (!valid()) return 0;
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
        ring_publish_sleep(ring, counter);
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
        RingSlot slots[8];
        for (uint32_t i = 0; i < count; ++i) {
            const Ring ring{rings[i], order, doorbell};
            // Copy before callbacks (which may block on UART/terminal output),
            // returning a batch of space to the producer with one tail store.
            while (const auto taken = ring.try_pop_batch(slots, 8)) {
                for (uint32_t j = 0; j < taken; ++j) on_slot(slots[j]);
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
