#pragma once
// Copied, bounded messages over the existing SPSC ring ABI. These flags reserve
// the top two opcode bits; both endpoints must opt into this framing protocol.
// One writer/reader object owns one logical message, including partial progress.
// No allocation or wait policy: callers choose blocking, yielding or polling.
#include "mochi/ring.h"

namespace mochi {
constexpr uint32_t kFrameFirst = 1u << 30;
constexpr uint32_t kFrameLast = 1u << 31;
constexpr uint32_t kFramePacked = 1u << 29;
constexpr uint32_t kFrameFlags = kFrameFirst | kFrameLast | kFramePacked;
enum class FrameProgress { Blocked, Progress, Complete, Invalid };

// Packed records reuse the same cursor units and protection boundary. Only the
// first slot has metadata; the remaining bytes contain payload, including wrap.
inline uint64_t frame_slots(uint32_t bytes) { return (uint64_t{bytes} + 16 + 63) / 64; }
inline void frame_copy_into(RingPage* page, uint32_t order, uint64_t cursor,
                            const void* data, uint32_t bytes) {
    auto* base = reinterpret_cast<uint8_t*>(ring_slots(page));
    const uint64_t capacity = uint64_t{mx_ring_capacity(order)} * 64;
    const uint64_t begin = (cursor & mx_ring_mask(order)) * 64 + 16;
    const auto first = static_cast<uint32_t>(bytes < capacity - begin ? bytes : capacity - begin);
    (void)libk::copy_n(libk::as_bytes(base + begin, first), libk::as_bytes(data, bytes), first);
    if (bytes > first)
        (void)libk::copy_n(libk::as_bytes(base, bytes - first),
                          libk::as_bytes(static_cast<const uint8_t*>(data) + first, bytes - first), bytes - first);
}
inline void frame_copy_out(const RingPage* page, uint32_t order, uint64_t cursor,
                           void* data, uint32_t bytes) {
    const auto* base = reinterpret_cast<const uint8_t*>(mx_ring_slots(page));
    const uint64_t capacity = uint64_t{mx_ring_capacity(order)} * 64;
    const uint64_t begin = (cursor & mx_ring_mask(order)) * 64 + 16;
    const auto first = static_cast<uint32_t>(bytes < capacity - begin ? bytes : capacity - begin);
    (void)libk::copy_n(libk::as_bytes(data, bytes), libk::as_bytes(base + begin, first), first);
    if (bytes > first)
        (void)libk::copy_n(libk::as_bytes(static_cast<uint8_t*>(data) + first, bytes - first),
                          libk::as_bytes(base, bytes - first), bytes - first);
}

class FrameWriter {
    const uint8_t* data_;
    uint32_t length_, offset_ = 0, type_, batch_slots_;
    uint64_t sequence_;
    bool complete_ = false;
public:
    FrameWriter(uint32_t type, uint64_t sequence, const void* data, uint32_t length, uint32_t batch_slots = 16)
        : data_(static_cast<const uint8_t*>(data)), length_(length), type_(type), batch_slots_(batch_slots), sequence_(sequence) {}

    uint32_t required_space(const Ring& ring) const {
        return !offset_ && length_ > MX_RING_INLINE_PAYLOAD && ring.valid() &&
               (ring.raw()->space.features & MX_RING_PACKED_FRAMES) && frame_slots(length_) <= ring.capacity()
                   ? static_cast<uint32_t>(frame_slots(length_)) : 1;
    }

    [[gnu::always_inline]] FrameProgress push(const Ring& ring) {
        if (!ring.valid() || !batch_slots_ || (type_ & kFrameFlags) || (!data_ && length_))
            return FrameProgress::Invalid;
        if (complete_) return FrameProgress::Complete;
        if (length_ > MX_RING_INLINE_PAYLOAD) return push_large(ring);
        auto* page = ring.raw();
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > ring.capacity()) return FrameProgress::Invalid;
        if (pending == ring.capacity()) return FrameProgress::Blocked;
        ring_write_slot(ring_slots(page)[head & ring.mask()], type_ | kFrameFirst | kFrameLast,
                        data_, length_, sequence_);
        complete_ = true;
        ring_publish(page, head + 1, ring.doorbell, ring.deferred_notify);
        return FrameProgress::Complete;
    }

private:
    // Keep bulk copying and partial-fragment state out of the inline-record
    // register allocation and instruction footprint.
    [[gnu::noinline]] FrameProgress push_large(const Ring& ring) {
        if (!ring.valid() || !batch_slots_ || (type_ & kFrameFlags) || (!data_ && length_)) return FrameProgress::Invalid;
        if (complete_) return FrameProgress::Complete;
        auto* page = ring.raw();
        const uint64_t capacity = ring.capacity(), mask = capacity - 1;
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > capacity) return FrameProgress::Invalid;
        if (!offset_ && length_ > MX_RING_INLINE_PAYLOAD &&
            (page->space.features & MX_RING_PACKED_FRAMES) && frame_slots(length_) <= capacity) {
            const uint64_t needed = frame_slots(length_);
            if (needed > capacity - pending) return FrameProgress::Blocked;
            auto& slot = ring_slots(page)[head & mask];
            slot.type = type_ | kFrameFirst | kFrameLast | kFramePacked;
            slot.length = length_;
            slot.seq = sequence_;
            frame_copy_into(page, ring.order, head, data_, length_);
            ring_publish(page, head + needed, ring.doorbell, ring.deferred_notify);
            complete_ = true;
            return FrameProgress::Complete;
        }
        if (pending == capacity) return FrameProgress::Blocked;
        const uint32_t room = static_cast<uint32_t>(capacity - pending);
        const uint32_t available = room < batch_slots_ ? room : batch_slots_;
        auto* slots = ring_slots(page);
        uint32_t written = 0;
        do {
            const uint32_t remaining = length_ - offset_;
            const uint32_t chunk = remaining < MX_RING_INLINE_PAYLOAD ? remaining : MX_RING_INLINE_PAYLOAD;
            const bool last = chunk == remaining;
            ring_write_slot(slots[(head + written) & mask],
                            type_ | (offset_ == 0 ? kFrameFirst : 0) | (last ? kFrameLast : 0),
                            data_ ? data_ + offset_ : nullptr, chunk, sequence_);
            offset_ += chunk;
            ++written;
            complete_ = last;
        } while (!complete_ && written < available);
        // Publish one bounded available run once. Retain the established atomic
        // sleep-flag claim; batching must not weaken the wait/wake handshake.
        ring_publish(page, head + written, ring.doorbell, ring.deferred_notify);
        return complete_ ? FrameProgress::Complete : FrameProgress::Progress;
    }
};

class FrameReader {
    uint8_t* data_;
    uint32_t capacity_, size_ = 0, type_ = 0;
    uint64_t sequence_ = 0;
    bool started_ = false, complete_ = false, invalid_ = false;
public:
    FrameReader(void* data, uint32_t capacity) : data_(static_cast<uint8_t*>(data)), capacity_(capacity) {}
    uint32_t size() const { return size_; }
    uint32_t type() const { return type_; }
    uint64_t sequence() const { return sequence_; }

    // notify_credits=false is for an explicitly negotiated polling producer;
    // it must never block waiting for a space notification in that policy.
    [[gnu::always_inline]] FrameProgress pop(const Ring& ring, uint32_t max_slots = 16,
                                            bool notify_credits = true) {
        if (invalid_ || !ring.valid() || !max_slots || (!data_ && capacity_)) return FrameProgress::Invalid;
        if (complete_) return FrameProgress::Complete;
        auto* page = ring.raw();
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_RELAXED);
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > ring.capacity()) { invalid_ = true; return FrameProgress::Invalid; }
        if (!pending) return FrameProgress::Blocked;
        const auto& slot = ring_slots(page)[tail & ring.mask()];
        const uint32_t length = slot.length, flags = slot.type & kFrameFlags;
        if (!started_ && flags == (kFrameFirst | kFrameLast) &&
            length <= MX_RING_INLINE_PAYLOAD && length <= capacity_) {
            type_ = slot.type & ~kFrameFlags;
            sequence_ = slot.seq;
            if (length) (void)libk::copy_n(libk::as_bytes(data_, capacity_),
                                          libk::as_bytes(slot.payload), length);
            size_ = length;
            started_ = complete_ = true;
            ring_release(page, tail + 1, notify_credits ? ring.space_doorbell : kNullCap);
            return FrameProgress::Complete;
        }
        return pop_slow(ring, max_slots, notify_credits);
    }

private:
    [[gnu::noinline]] FrameProgress pop_slow(const Ring& ring, uint32_t max_slots, bool notify_credits) {
        if (invalid_ || !ring.valid() || !max_slots || (!data_ && capacity_)) return FrameProgress::Invalid;
        if (complete_) return FrameProgress::Complete;
        auto* page = ring.raw();
        const uint64_t capacity = ring.capacity(), mask = capacity - 1;
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_RELAXED);
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > capacity) { invalid_ = true; return FrameProgress::Invalid; }
        if (!pending) return FrameProgress::Blocked;
        const auto& first_slot = ring_slots(page)[tail & mask];
        if (first_slot.type & kFramePacked) {
            const uint32_t length = first_slot.length;
            const uint64_t needed = frame_slots(length);
            if (!(page->space.features & MX_RING_PACKED_FRAMES) || started_ ||
                (first_slot.type & kFrameFlags) != kFrameFlags || length <= MX_RING_INLINE_PAYLOAD ||
                length > capacity_ || needed > capacity || needed > pending) {
                invalid_ = true;
                return FrameProgress::Invalid;
            }
            type_ = first_slot.type & ~kFrameFlags;
            sequence_ = first_slot.seq;
            frame_copy_out(page, ring.order, tail, data_, length);
            size_ = length;
            started_ = complete_ = true;
            ring_release(page, tail + needed, notify_credits ? ring.space_doorbell : kNullCap);
            return FrameProgress::Complete;
        }
        // Release a bounded copy run so a producer on another CPU can refill
        // while this reader continues a large message.
        const uint32_t limit = pending < max_slots ? static_cast<uint32_t>(pending) : max_slots;
        const auto* slots = ring_slots(page);
        uint32_t taken = 0;
        do {
            const auto& slot = slots[(tail + taken) & mask];
            const uint32_t length = slot.length, kind = slot.type & ~kFrameFlags;
            const bool first = (slot.type & kFrameFirst) != 0, last = (slot.type & kFrameLast) != 0;
            if ((slot.type & kFramePacked) || length > MX_RING_INLINE_PAYLOAD || length > capacity_ - size_ || first == started_ ||
                (started_ && (kind != type_ || slot.seq != sequence_)) || (!length && (started_ || !last))) {
                invalid_ = true;
                break;
            }
            if (!started_) { type_ = kind; sequence_ = slot.seq; started_ = true; }
            // Copy directly while this consumer still owns the slots. Release
            // them only after the copy; no shared-storage pointer escapes.
            if (length) (void)libk::copy_n(libk::as_bytes(data_ + size_, capacity_ - size_),
                                          libk::as_bytes(slot.payload), length);
            size_ += length;
            ++taken;
            complete_ = last;
        } while (!complete_ && taken < limit);
        if (taken) ring_release(page, tail + taken, notify_credits ? ring.space_doorbell : kNullCap);
        // Invalid framing poisons this reader. Callers must abandon the stream
        // rather than guessing a new message boundary or silently dropping it.
        if (invalid_) return FrameProgress::Invalid;
        return complete_ ? FrameProgress::Complete : FrameProgress::Progress;
    }
};
} // namespace mochi
