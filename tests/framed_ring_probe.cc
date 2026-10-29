#include "mochi/framed_ring.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>

using mochi::FrameProgress;
template <unsigned Order> struct Storage {
    RingPage page{};
    RingSlot slots[1u << Order]{};
    mochi::Ring view() { return {&page, Order, 1}; }
};

static void boundaries() {
    Storage<2> storage;
    auto ring = storage.view();
    storage.page.head.v = storage.page.tail.v = ~uint64_t{0} - 2;
    for (unsigned bytes : {0u, 1u, 48u, 49u, 192u, 193u, 4096u}) {
        std::vector<uint8_t> source(bytes), destination(bytes + 2, 0xa5);
        for (unsigned i = 0; i < bytes; ++i) source[i] = static_cast<uint8_t>(i * 37 + bytes);
        mochi::FrameWriter writer(7, bytes + 99, source.data(), bytes);
        mochi::FrameReader reader(destination.data() + 1, bytes);
        // Each publication fills the available ring; no frame is lost at wrap.
        for (;;) {
            const auto sent = writer.push(ring);
            assert(sent == FrameProgress::Progress || sent == FrameProgress::Complete);
            const auto read = reader.pop(ring);
            assert(read == sent);
            if (read == FrameProgress::Complete) break;
        }
        assert(reader.size() == bytes && reader.type() == 7 && reader.sequence() == bytes + 99);
        assert(!bytes || !std::memcmp(source.data(), destination.data() + 1, bytes));
        assert(destination.front() == 0xa5 && destination.back() == 0xa5);
        const auto head = storage.page.head.v, tail = storage.page.tail.v;
        assert(writer.push(ring) == FrameProgress::Complete && storage.page.head.v == head);
        assert(reader.pop(ring) == FrameProgress::Complete && storage.page.tail.v == tail);
    }
    // Coalesce a whole run into exactly one wake; retain zeroed payload tails.
    storage = {}; host_notifies = 0;
    storage.page.flags.receiver_about_to_sleep = 1;
    uint8_t payload[100]{};
    mochi::FrameWriter writer(7, 3, payload, sizeof(payload));
    assert(writer.push(ring) == FrameProgress::Complete && host_notifies == 1);
    assert(storage.page.head.v == 3 && !storage.page.flags.receiver_about_to_sleep);
    for (unsigned i = 4; i < 48; ++i) assert(storage.slots[2].payload[i] == 0);
    // A second reader must leave the next complete message queued.
    mochi::FrameWriter empty(9, 4, nullptr, 0);
    assert(empty.push(ring) == FrameProgress::Complete);
    mochi::FrameReader reader(payload, sizeof(payload));
    assert(reader.pop(ring) == FrameProgress::Complete && storage.page.tail.v == 3);
    mochi::FrameReader control(nullptr, 0);
    assert(control.pop(ring) == FrameProgress::Complete && control.type() == 9 && control.size() == 0);
}

static void invalid_frames() {
    for (unsigned fault = 0; fault < 7; ++fault) {
        Storage<2> storage;
        uint8_t output[64]; std::memset(output, 0xa5, sizeof(output));
        auto& slot = storage.slots[0];
        slot.type = 7 | mochi::kFrameFirst | mochi::kFrameLast;
        slot.length = 3; slot.seq = 9;
        if (fault == 0) slot.type &= ~mochi::kFrameFirst;
        if (fault == 1) slot.length = 49;
        if (fault == 2) slot.length = 33;
        if (fault == 3) { slot.length = 0; slot.type &= ~mochi::kFrameLast; }
        storage.page.head.v = 1;
        if (fault >= 4) {
            slot.type &= ~mochi::kFrameLast;
            storage.slots[1] = slot;
            storage.slots[1].type = 7 | mochi::kFrameLast;
            if (fault == 4) storage.slots[1].type |= mochi::kFrameFirst;
            if (fault == 5) ++storage.slots[1].seq;
            if (fault == 6) storage.slots[1].type ^= 1;
            storage.page.head.v = 2;
        }
        mochi::FrameReader reader(output + 16, 32);
        assert(reader.pop(storage.view()) == FrameProgress::Invalid);
        assert(reader.pop(storage.view()) == FrameProgress::Invalid); // poisoned
        for (unsigned i = 0; i < 16; ++i) assert(output[i] == 0xa5 && output[48 + i] == 0xa5);
    }
    Storage<2> storage;
    uint8_t data[256]{};
    auto ring = storage.view();
    mochi::FrameWriter writer(7, 1, data, sizeof(data));
    assert(writer.push(ring) == FrameProgress::Progress);
    const auto head = storage.page.head.v;
    assert(writer.push(ring) == FrameProgress::Blocked && storage.page.head.v == head);
    storage.page.head.v = 5; // impossible occupancy
    assert(writer.push(ring) == FrameProgress::Invalid);
    mochi::FrameReader reader(data, sizeof(data));
    assert(reader.pop(ring) == FrameProgress::Invalid);
    mochi::FrameWriter bad_type(mochi::kFrameFirst, 1, data, 1);
    assert(bad_type.push(ring) == FrameProgress::Invalid);
    mochi::FrameWriter bad_pointer(7, 1, nullptr, 1);
    assert(bad_pointer.push(ring) == FrameProgress::Invalid);
}

// Batched and previous slot-at-a-time transports must share a wire format.
static void interoperability() {
    for (unsigned length : {0u, 1u, 48u, 49u, 256u, 4096u}) {
        uint8_t source[4096], destination[4096]{};
        for (unsigned i = 0; i < length; ++i) source[i] = static_cast<uint8_t>(i * 31 + length);
        Storage<2> storage;
        auto ring = storage.view();
        mochi::FrameWriter writer(11, 919, source, length);
        unsigned copied = 0;
        bool first = true, done = false;
        while (!done) {
            const auto result = writer.push(ring);
            assert(result != FrameProgress::Invalid);
            RingSlot slot;
            while (ring.try_pop(&slot)) {
                assert(slot.seq == 919 && (slot.type & ~mochi::kFrameFlags) == 11);
                assert(bool(slot.type & mochi::kFrameFirst) == first);
                std::memcpy(destination + copied, slot.payload, slot.length);
                copied += slot.length; first = false;
                if (slot.type & mochi::kFrameLast) { done = true; break; }
            }
        }
        assert(copied == length && !std::memcmp(source, destination, length));
        mochi::FrameReader reader(destination, sizeof(destination));
        unsigned sent = 0;
        do {
            const unsigned chunk = length - sent < 48 ? length - sent : 48;
            const uint32_t type = 11 | (sent == 0 ? mochi::kFrameFirst : 0) |
                                  (sent + chunk == length ? mochi::kFrameLast : 0);
            const uint64_t sequence = 919;
            assert(ring_try_push(ring.raw(), 2, type, source + sent, chunk, 1, &sequence));
            sent += chunk;
            assert(reader.pop(ring) == (sent == length ? FrameProgress::Complete : FrameProgress::Progress));
        } while (sent < length);
        assert(reader.size() == length && reader.type() == 11 && reader.sequence() == 919);
        assert(!std::memcmp(source, destination, length));
    }
}

static void packed_records() {
    Storage<7> storage;
    storage.page.space.features = MX_RING_PACKED_FRAMES;
    auto ring = storage.view();
    uint8_t source[4096], destination[4098];
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = static_cast<uint8_t>(i * 37);
    for (uint64_t cursor : {uint64_t{0}, uint64_t{127}, ~uint64_t{0} - 17}) {
        for (unsigned bytes : {0u, 8u, 48u, 49u, 256u, 4096u}) {
            storage.page.head.v = storage.page.tail.v = cursor;
            std::memset(destination, 0xa5, sizeof(destination));
            mochi::FrameWriter writer(3, 119, source, bytes);
            assert(writer.push(ring) == FrameProgress::Complete);
            const uint64_t count = bytes > 48 ? mochi::frame_slots(bytes) : 1;
            assert(storage.page.head.v - cursor == count);
            mochi::FrameReader reader(destination + 1, bytes);
            assert(reader.pop(ring) == FrameProgress::Complete);
            assert(reader.type() == 3 && reader.sequence() == 119 && reader.size() == bytes);
            assert(!std::memcmp(source, destination + 1, bytes));
            assert(destination[0] == 0xa5 && destination[bytes + 1] == 0xa5);
            assert(storage.page.head.v == storage.page.tail.v);
        }
    }
    storage.page.tail.v = 0; storage.page.head.v = 64;
    mochi::FrameWriter writer(3, 0, source, sizeof(source));
    assert(writer.required_space(ring) == 65);
    assert(writer.push(ring) == FrameProgress::Blocked && storage.page.head.v == 64);
    storage.page.tail.v = 1;
    assert(writer.push(ring) == FrameProgress::Complete);
    storage.page.tail.v = 64;
    storage.slots[64].length = 4097;
    mochi::FrameReader invalid(destination, 4096);
    assert(invalid.pop(ring) == FrameProgress::Invalid && storage.page.tail.v == 64);
}

template <unsigned Order> static void concurrent(bool packed = false) {
    Storage<Order> storage;
    storage.page.space.features = packed ? MX_RING_PACKED_FRAMES : 0;
    storage.page.head.v = storage.page.tail.v = ~uint64_t{0} - 27;
    auto ring = storage.view();
    constexpr unsigned messages = 3000;
    std::thread producer([&] {
        uint8_t bytes[4096];
        for (unsigned n = 0; n < messages; ++n) {
            const unsigned size = n % 2 ? 4096 : n % 257;
            for (unsigned i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(n * 17 + i * 37);
            mochi::FrameWriter writer(n % 19, n, bytes, size);
            for (;;) {
                auto result = writer.push(ring);
                assert(result != FrameProgress::Invalid);
                if (result == FrameProgress::Complete) break;
                if (result == FrameProgress::Blocked) std::this_thread::yield();
            }
        }
    });
    uint8_t bytes[4096];
    for (unsigned n = 0; n < messages; ++n) {
        mochi::FrameReader reader(bytes, sizeof(bytes));
        for (;;) {
            auto result = reader.pop(ring);
            assert(result != FrameProgress::Invalid);
            if (result == FrameProgress::Complete) break;
            if (result == FrameProgress::Blocked) std::this_thread::yield();
        }
        const unsigned size = n % 2 ? 4096 : n % 257;
        assert(reader.sequence() == n && reader.type() == n % 19 && reader.size() == size);
        for (unsigned i = 0; i < size; ++i) assert(bytes[i] == static_cast<uint8_t>(n * 17 + i * 37));
    }
    producer.join();
    assert(storage.page.head.v == storage.page.tail.v);
}

int main() {
    boundaries(); invalid_frames(); interoperability(); packed_records();
    concurrent<6>(); concurrent<7>(true);
}
