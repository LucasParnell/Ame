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
constexpr uint64_t frame_slots(uint32_t bytes) {
    return (uint64_t{bytes} + 16 + 63) / 64;
}
// Choose capacity from the application's maximum record and desired number of
// queued records, rather than requiring callers to reason about cursor units.
// An invalid request returns order 0; nothing is silently truncated.
struct FrameGeometry {
    uint32_t order = 0;
    uint32_t max_payload = 0;
    uint32_t queued_records = 0;
    constexpr bool valid() const {
        return order >= MX_RING_ORDER_MIN && order <= MX_RING_ORDER_MAX;
    }
    static constexpr FrameGeometry for_records(uint32_t bytes, uint32_t queued = 3) {
        const uint64_t units = frame_slots(bytes);
        const uint64_t required = units * queued;
        if (!queued || required > (uint64_t{1} << MX_RING_ORDER_MAX))
            return {};
        uint32_t order = MX_RING_ORDER_MIN;
        while ((uint64_t{1} << order) < required)
            ++order;
        return {order, bytes, queued};
    }
};
inline void frame_copy_into(RingPage* page, uint32_t order, uint64_t cursor, const void* data,
                            uint32_t bytes) {
    auto* base = reinterpret_cast<uint8_t*>(ring_slots(page));
    const uint64_t capacity = uint64_t{mx_ring_capacity(order)} * 64;
    const uint64_t begin = (cursor & mx_ring_mask(order)) * 64 + 16;
    const auto first = static_cast<uint32_t>(bytes < capacity - begin ? bytes : capacity - begin);
    (void)libk::copy_n(libk::as_bytes(base + begin, first), libk::as_bytes(data, bytes), first);
    if (bytes > first)
        (void)libk::copy_n(libk::as_bytes(base, bytes - first),
                           libk::as_bytes(static_cast<const uint8_t*>(data) + first, bytes - first),
                           bytes - first);
}
inline void frame_copy_out(const RingPage* page, uint32_t order, uint64_t cursor, void* data,
                           uint32_t bytes) {
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
    FrameWriter(uint32_t type, uint64_t sequence, const void* data, uint32_t length,
                uint32_t batch_slots = 16)
        : data_(static_cast<const uint8_t*>(data)), length_(length), type_(type),
          batch_slots_(batch_slots), sequence_(sequence) {
    }

    uint32_t required_space(const Ring& ring) const {
        return !offset_ && length_ > MX_RING_INLINE_PAYLOAD && ring.valid() &&
                       (ring.raw()->space.features & MX_RING_PACKED_FRAMES) &&
                       frame_slots(length_) <= ring.capacity()
                   ? static_cast<uint32_t>(frame_slots(length_))
                   : 1;
    }

    [[gnu::always_inline]] FrameProgress push(const Ring& ring) {
        if (!ring.valid() || !batch_slots_ || (type_ & kFrameFlags) || (!data_ && length_))
            return FrameProgress::Invalid;
        if (complete_)
            return FrameProgress::Complete;
        if (length_ > MX_RING_INLINE_PAYLOAD)
            return push_large(ring);
        auto* page = ring.raw();
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > ring.capacity())
            return FrameProgress::Invalid;
        if (pending == ring.capacity())
            return FrameProgress::Blocked;
        ring_write_slot(ring_slots(page)[head & ring.mask()], type_ | kFrameFirst | kFrameLast,
                        data_, length_, sequence_);
        complete_ = true;
        ring_publish(page, head + 1, ring.doorbell, ring.deferred_notify, ring.peer_polls);
        return FrameProgress::Complete;
    }

  private:
    // Keep bulk copying and partial-fragment state out of the inline-record
    // register allocation and instruction footprint.
    [[gnu::noinline]] FrameProgress push_large(const Ring& ring) {
        if (!ring.valid() || !batch_slots_ || (type_ & kFrameFlags) || (!data_ && length_))
            return FrameProgress::Invalid;
        if (complete_)
            return FrameProgress::Complete;
        auto* page = ring.raw();
        const uint64_t capacity = ring.capacity(), mask = capacity - 1;
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > capacity)
            return FrameProgress::Invalid;
        if (!offset_ && length_ > MX_RING_INLINE_PAYLOAD &&
            (page->space.features & MX_RING_PACKED_FRAMES) && frame_slots(length_) <= capacity) {
            const uint64_t needed = frame_slots(length_);
            if (needed > capacity - pending)
                return FrameProgress::Blocked;
            auto& slot = ring_slots(page)[head & mask];
            slot.type = type_ | kFrameFirst | kFrameLast | kFramePacked;
            slot.length = length_;
            slot.seq = sequence_;
            frame_copy_into(page, ring.order, head, data_, length_);
            ring_publish(page, head + needed, ring.doorbell, ring.deferred_notify, ring.peer_polls);
            complete_ = true;
            return FrameProgress::Complete;
        }
        if (pending == capacity)
            return FrameProgress::Blocked;
        const uint32_t room = static_cast<uint32_t>(capacity - pending);
        const uint32_t available = room < batch_slots_ ? room : batch_slots_;
        auto* slots = ring_slots(page);
        uint32_t written = 0;
        do {
            const uint32_t remaining = length_ - offset_;
            const uint32_t chunk =
                remaining < MX_RING_INLINE_PAYLOAD ? remaining : MX_RING_INLINE_PAYLOAD;
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
        ring_publish(page, head + written, ring.doorbell, ring.deferred_notify, ring.peer_polls);
        return complete_ ? FrameProgress::Complete : FrameProgress::Progress;
    }
};

class FrameReader {
    uint8_t* data_;
    uint32_t capacity_, size_ = 0, type_ = 0;
    uint64_t sequence_ = 0;
    bool started_ = false, complete_ = false, invalid_ = false;

  public:
    FrameReader(void* data, uint32_t capacity)
        : data_(static_cast<uint8_t*>(data)), capacity_(capacity) {
    }
    uint32_t size() const {
        return size_;
    }
    uint32_t type() const {
        return type_;
    }
    uint64_t sequence() const {
        return sequence_;
    }

    // notify_credits=false is for an explicitly negotiated polling producer;
    // it must never block waiting for a space notification in that policy.
    [[gnu::always_inline]] FrameProgress pop(const Ring& ring, uint32_t max_slots = 16,
                                             bool notify_credits = true) {
        if (invalid_ || !ring.valid() || !max_slots || (!data_ && capacity_))
            return FrameProgress::Invalid;
        if (complete_)
            return FrameProgress::Complete;
        auto* page = ring.raw();
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_RELAXED);
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > ring.capacity()) {
            invalid_ = true;
            return FrameProgress::Invalid;
        }
        if (!pending)
            return FrameProgress::Blocked;
        const auto& slot = ring_slots(page)[tail & ring.mask()];
        const uint32_t length = slot.length, flags = slot.type & kFrameFlags;
        if (!started_ && flags == (kFrameFirst | kFrameLast) && length <= MX_RING_INLINE_PAYLOAD &&
            length <= capacity_) {
            type_ = slot.type & ~kFrameFlags;
            sequence_ = slot.seq;
            if (length)
                (void)libk::copy_n(libk::as_bytes(data_, capacity_), libk::as_bytes(slot.payload),
                                   length);
            size_ = length;
            started_ = complete_ = true;
            ring_release(page, tail + 1, notify_credits ? ring.space_doorbell : kNullCap,
                         ring.peer_polls, ring.deferred_space_notify);
            return FrameProgress::Complete;
        }
        return pop_slow(ring, max_slots, notify_credits);
    }

  private:
    [[gnu::noinline]] FrameProgress pop_slow(const Ring& ring, uint32_t max_slots,
                                             bool notify_credits) {
        if (invalid_ || !ring.valid() || !max_slots || (!data_ && capacity_))
            return FrameProgress::Invalid;
        if (complete_)
            return FrameProgress::Complete;
        auto* page = ring.raw();
        const uint64_t capacity = ring.capacity(), mask = capacity - 1;
        const uint64_t tail = __atomic_load_n(&page->tail.v, __ATOMIC_RELAXED);
        const uint64_t head = __atomic_load_n(&page->head.v, __ATOMIC_ACQUIRE);
        const uint64_t pending = head - tail;
        if (pending > capacity) {
            invalid_ = true;
            return FrameProgress::Invalid;
        }
        if (!pending)
            return FrameProgress::Blocked;
        const auto& first_slot = ring_slots(page)[tail & mask];
        if (first_slot.type & kFramePacked) {
            const uint32_t length = first_slot.length;
            const uint64_t needed = frame_slots(length);
            if (!(page->space.features & MX_RING_PACKED_FRAMES) || started_ ||
                (first_slot.type & kFrameFlags) != kFrameFlags ||
                length <= MX_RING_INLINE_PAYLOAD || length > capacity_ || needed > capacity ||
                needed > pending) {
                invalid_ = true;
                return FrameProgress::Invalid;
            }
            type_ = first_slot.type & ~kFrameFlags;
            sequence_ = first_slot.seq;
            frame_copy_out(page, ring.order, tail, data_, length);
            size_ = length;
            started_ = complete_ = true;
            ring_release(page, tail + needed, notify_credits ? ring.space_doorbell : kNullCap,
                         ring.peer_polls, ring.deferred_space_notify);
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
            if ((slot.type & kFramePacked) || length > MX_RING_INLINE_PAYLOAD ||
                length > capacity_ - size_ || first == started_ ||
                (started_ && (kind != type_ || slot.seq != sequence_)) ||
                (!length && (started_ || !last))) {
                invalid_ = true;
                break;
            }
            if (!started_) {
                type_ = kind;
                sequence_ = slot.seq;
                started_ = true;
            }
            // Copy directly while this consumer still owns the slots. Release
            // them only after the copy; no shared-storage pointer escapes.
            if (length)
                (void)libk::copy_n(libk::as_bytes(data_ + size_, capacity_ - size_),
                                   libk::as_bytes(slot.payload), length);
            size_ += length;
            ++taken;
            complete_ = last;
        } while (!complete_ && taken < limit);
        if (taken)
            ring_release(page, tail + taken, notify_credits ? ring.space_doorbell : kNullCap,
                         ring.peer_polls, ring.deferred_space_notify);
        // Invalid framing poisons this reader. Callers must abandon the stream
        // rather than guessing a new message boundary or silently dropping it.
        if (invalid_)
            return FrameProgress::Invalid;
        return complete_ ? FrameProgress::Complete : FrameProgress::Progress;
    }
};

struct FrameInfo {
    uint32_t type = 0, size = 0;
    uint64_t sequence = 0;
};

// Exclusive SPSC producer transaction. Appends copy caller-owned bytes now;
// commit publishes the entire run once. Cache peer progress only within this
// transaction, so ordinary Ring value views cannot inherit a stale cache.
class FrameBatchWriter {
    const Ring& ring_;
    uint64_t head_ = 0, published_ = 0, tail_ = 0;
    bool active_ = false;

  public:
    explicit FrameBatchWriter(const Ring& ring) : ring_(ring) {
    }
    FrameBatchWriter(const FrameBatchWriter&) = delete;
    FrameBatchWriter& operator=(const FrameBatchWriter&) = delete;
    ~FrameBatchWriter() {
        commit();
    }
    FrameProgress push(uint32_t type, uint64_t sequence, const void* data, uint32_t size) {
        if (!ring_.valid() || (type & kFrameFlags) || (!data && size))
            return FrameProgress::Invalid;
        const uint64_t needed = frame_slots(size), capacity = ring_.capacity();
        if (needed > capacity || (size > MX_RING_INLINE_PAYLOAD &&
                                  !(ring_.raw()->space.features & MX_RING_PACKED_FRAMES)))
            return FrameProgress::Invalid;
        auto* page = ring_.raw();
        if (!active_) {
            head_ = published_ = libk::atomic::load_relaxed(&page->head.v);
            tail_ = libk::atomic::load_acquire(&page->tail.v);
            active_ = true;
        }
        if (head_ - tail_ > capacity)
            return FrameProgress::Invalid;
        if (needed > capacity - (head_ - tail_)) {
            tail_ = libk::atomic::load_acquire(&page->tail.v);
            if (head_ - tail_ > capacity)
                return FrameProgress::Invalid;
            if (needed > capacity - (head_ - tail_))
                return FrameProgress::Blocked;
        }
        auto& slot = ring_slots(page)[head_ & ring_.mask()];
        if (size <= MX_RING_INLINE_PAYLOAD)
            ring_write_slot(slot, type | kFrameFirst | kFrameLast, data, size, sequence);
        else {
            slot.type = type | kFrameFlags;
            slot.length = size;
            slot.seq = sequence;
            frame_copy_into(page, ring_.order, head_, data, size);
        }
        head_ += needed;
        return FrameProgress::Complete;
    }
    // Make records visible without ending exclusive cursor ownership. A
    // concurrent consumer can run while the next push reuses our tail cache.
    void publish() {
        if (active_ && head_ != published_) {
            ring_publish(ring_.raw(), head_, ring_.doorbell, ring_.deferred_notify,
                         ring_.peer_polls);
            published_ = head_;
        }
    }
    void commit() {
        publish();
        active_ = false;
    }
};

// Exclusive packed/inline consumer transaction. Bytes are copied before their
// slots are released. commit returns all credits at once; no shared pointer
// escapes. Fragmented records remain the responsibility of FrameReader.
class FrameBatchReader {
    const Ring& ring_;
    uint64_t tail_ = 0, released_ = 0, head_ = 0;
    bool active_ = false, invalid_ = false;

  public:
    explicit FrameBatchReader(const Ring& ring) : ring_(ring) {
    }
    FrameBatchReader(const FrameBatchReader&) = delete;
    FrameBatchReader& operator=(const FrameBatchReader&) = delete;
    ~FrameBatchReader() {
        commit();
    }
    FrameProgress pop(FrameInfo& info, void* data, uint32_t capacity) {
        if (invalid_ || !ring_.valid() || (!data && capacity))
            return FrameProgress::Invalid;
        const auto* page = ring_.raw();
        if (!active_) {
            tail_ = released_ = libk::atomic::load_relaxed(&page->tail.v);
            head_ = libk::atomic::load_acquire(&page->head.v);
            active_ = true;
        }
        if (head_ == tail_)
            head_ = libk::atomic::load_acquire(&page->head.v);
        if (head_ == tail_)
            return FrameProgress::Blocked;
        if (head_ - tail_ > ring_.capacity()) {
            invalid_ = true;
            return FrameProgress::Invalid;
        }
        const auto& slot = mx_ring_slots(page)[tail_ & ring_.mask()];
        const uint32_t size = slot.length, flags = slot.type & kFrameFlags;
        const bool packed = size > MX_RING_INLINE_PAYLOAD;
        const uint64_t needed = frame_slots(size);
        if (size > capacity || needed > head_ - tail_ ||
            flags != (kFrameFirst | kFrameLast | (packed ? kFramePacked : 0)) ||
            (packed && !(page->space.features & MX_RING_PACKED_FRAMES))) {
            invalid_ = true;
            return FrameProgress::Invalid;
        }
        info = {slot.type & ~kFrameFlags, size, slot.seq};
        if (packed)
            frame_copy_out(page, ring_.order, tail_, data, size);
        else if (size)
            (void)libk::copy_n(libk::as_bytes(data, capacity), libk::as_bytes(slot.payload), size);
        tail_ += needed;
        return FrameProgress::Complete;
    }
    uint64_t pending_slots() const {
        return active_ ? tail_ - released_ : 0;
    }
    // Return copied storage while retaining exclusive ownership and the cached
    // producer cursor. Only commit permits another view to use this consumer.
    void release() {
        if (active_ && tail_ != released_) {
            ring_release(ring_.raw(), tail_, ring_.space_doorbell, ring_.peer_polls,
                         ring_.deferred_space_notify);
            released_ = tail_;
        }
    }
    void commit() {
        release();
        active_ = false;
    }
};
} // namespace mochi
