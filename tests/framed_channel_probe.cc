#include "mochi/framed_channel.h"
#include <cassert>
#include <cstring>
#include <thread>

template <unsigned Order> struct Storage {
    RingPage page{};
    RingSlot slots[1u << Order]{};
    Storage() {
        page.space.features = MX_RING_PACKED_FRAMES;
    }
    mochi::Ring view() {
        return {&page, Order, 1, 9, 2};
    }
};

static void batches() {
    static_assert(mx_ring_mapping_bytes(6) == 8192);
    static_assert(mx_ring_mapping_bytes(8) == 20480);
    static_assert(mx_ring_mapping_bytes(10) == 69632);
    static_assert(!mx_ring_mapping_bytes(32));
    static_assert(mochi::FrameGeometry::for_records(4096, 3).order == 8);
    static_assert(mochi::FrameGeometry::for_records(4096, 7).order == 9);
    static_assert(!mochi::FrameGeometry::for_records(4096, 16).valid());
    static_assert(!mochi::FrameGeometry::for_records(~uint32_t{0}, ~uint32_t{0}).valid());
    static_assert(!mochi::FrameGeometry::for_records(0, 0).valid());
    Storage<8> storage;
    auto ring = storage.view();
    storage.page.head.v = storage.page.tail.v = ~uint64_t{0} - 64;
    const auto start = storage.page.head.v;
    uint8_t bytes[4096], copy[4098];
    std::memset(bytes, 0x5a, sizeof(bytes));
    std::memset(copy, 0xa5, sizeof(copy));
    host_notifies = 0;
    storage.page.flags.receiver_about_to_sleep = 1;
    mochi::FrameBatchWriter writer(ring);
    for (unsigned i = 0; i < 3; ++i)
        assert(writer.push(7, i, bytes, sizeof(bytes)) == mochi::FrameProgress::Complete);
    assert(writer.push(7, 3, bytes, sizeof(bytes)) == mochi::FrameProgress::Blocked);
    assert(storage.page.head.v == start && host_notifies == 0);
    // Input is reusable immediately, before publication.
    std::memset(bytes, 0, sizeof(bytes));
    writer.commit();
    assert(storage.page.head.v - start == 195 && host_notifies == 1);
    mochi::FrameBatchReader reader(ring);
    mochi::FrameInfo info;
    storage.page.space.producer_about_to_sleep = 1;
    for (unsigned i = 0; i < 3; ++i) {
        assert(reader.pop(info, copy + 1, 4096) == mochi::FrameProgress::Complete);
        assert(info.sequence == i && info.type == 7 && info.size == 4096);
        for (unsigned j = 1; j <= 4096; ++j)
            assert(copy[j] == 0x5a);
        assert(copy[0] == 0xa5 && copy[4097] == 0xa5);
    }
    assert(reader.pop(info, copy, sizeof(copy)) == mochi::FrameProgress::Blocked);
    assert(storage.page.tail.v == start);
    reader.commit();
    assert(storage.page.tail.v == storage.page.head.v && host_notifies == 2);
    // Cached peer cursors cannot persist across another ordinary value view.
    mochi::FrameWriter ordinary(8, 11, nullptr, 0);
    assert(ordinary.push(ring) == mochi::FrameProgress::Complete);
    assert(reader.pop(info, copy, sizeof(copy)) == mochi::FrameProgress::Complete &&
           info.sequence == 11);
    reader.commit();
    assert(writer.push(8, 12, nullptr, 0) == mochi::FrameProgress::Complete);
    writer.commit();
    auto& slot = storage.slots[storage.page.tail.v & ring.mask()];
    slot.length = 4097;
    assert(reader.pop(info, copy + 1, 4096) == mochi::FrameProgress::Invalid);
    assert(reader.pop(info, copy + 1, 4096) == mochi::FrameProgress::Invalid);
    assert(copy[0] == 0xa5 && copy[4097] == 0xa5);
}

static unsigned unmapped;
static void mapping_teardown() {
    host_on_unmap_page = [](mx_vaddr_t address) -> mx_status_t {
        assert(address == 0x10000000 + uint64_t{unmapped++} * 4096);
        return MX_OK;
    };
    assert(mochi::unmap_ring(0x10000000, 6) && unmapped == 2);
    assert(!mochi::unmap_ring(0x10000001, 6));
    assert(!mochi::unmap_ring(~uint64_t{4095}, 6));
    assert(!mochi::unmap_ring(0x10000000, 32));
    assert(unmapped == 2);
    host_on_unmap_page = nullptr;
}

static Storage<7>* tx_storage;
static Storage<7>* rx_storage;
static unsigned space_waits;
static void channel_exchange() {
    Storage<7> tx, rx;
    tx_storage = &tx;
    rx_storage = &rx;
    auto out = tx.view(), in = rx.view();
    out.doorbell = 11;
    in.doorbell = 12;
    in.space_doorbell = 13;
    host_on_notify = [](CapHandle cap) -> mx_status_t {
        if (cap == 1)
            return MX_OK; // peer's notification of our armed data flag
        if (cap == 11) {
            // Peer returns the outgoing record's credits.
            tx_storage->page.tail.v = tx_storage->page.head.v;
        } else {
            assert(cap == 13);
            mochi::FrameWriter reply(9, 99, nullptr, 0);
            assert(reply.push(rx_storage->view()) == mochi::FrameProgress::Complete);
        }
        return MX_OK;
    };
    host_on_wait_ring_space = [](CapHandle, CapHandle, uint64_t, uint32_t direction,
                                 uint32_t needed) -> mx_i64_t {
        if (direction == MX_RING_WAIT_WRITABLE) {
            ++space_waits;
            assert(needed == 65 && tx_storage->page.head.v == tx_storage->page.tail.v);
        } else
            assert(rx_storage->page.head.v != rx_storage->page.tail.v);
        return MX_OK;
    };
    {
        mochi::FramedChannel channel(out, in, {false, 4096, true});
        assert(channel.current_poll_budget() == 0 && channel.polling(true) == MX_ERR_INVALID);
        uint8_t bytes[4096]{};
        tx.page.flags.receiver_about_to_sleep = 1;
        assert(channel.send(7, 1, bytes, sizeof(bytes), mochi::FrameFlush::OnWait) == MX_OK);
        // Only 63 units remain. The next complete record requires 65; carry
        // the pending data wake into this writable wait instead of flushing it.
        assert(channel.send(7, 2, bytes, sizeof(bytes), mochi::FrameFlush::OnWait) == MX_OK);
        assert(space_waits == 1);
        mochi::FrameWriter peer(9, 98, nullptr, 0);
        assert(peer.push(in) == mochi::FrameProgress::Complete);
        rx.page.space.producer_about_to_sleep = 1;
        mochi::FrameInfo info;
        assert(channel.receive(info, bytes, sizeof(bytes)) == MX_OK && info.sequence == 98);
        // The returned credit wake travels with the next receive; its callback
        // publishes the next record before the conditional wait checks it.
        assert(channel.receive(info, bytes, sizeof(bytes)) == MX_OK && info.sequence == 99);
        assert(channel.finish_reply() == MX_OK);
    }
    host_on_notify = nullptr;
    host_on_wait_ring_space = nullptr;
    // Cross-core production must overlap consumption even before the sender
    // reaches its next wait; local batching may defer until a flush.
    for (bool cross_core : {false, true}) {
        Storage<7> requests, replies;
        mochi::FramedChannel channel(requests.view(), replies.view(), {cross_core, 0, false});
        uint8_t source[] = {1, 2, 3}, copied[3]{};
        assert(channel.send(8, 42, source, sizeof(source), mochi::FrameFlush::Buffered) == MX_OK);
        source[0] = 99;
        mochi::FrameReader reader(copied, sizeof(copied));
        const auto progress = reader.pop(requests.view());
        if (cross_core)
            assert(progress == mochi::FrameProgress::Complete);
        else {
            assert(progress == mochi::FrameProgress::Blocked);
            assert(channel.flush() == MX_OK);
            assert(reader.pop(requests.view()) == mochi::FrameProgress::Complete);
        }
        assert(copied[0] == 1 && reader.sequence() == 42);
    }
}

static void concurrent_batches() {
    Storage<8> storage;
    storage.page.head.v = storage.page.tail.v = ~uint64_t{0} - 17;
    auto tx = storage.view(), rx = storage.view();
    tx.peer_polls = rx.peer_polls = true;
    constexpr unsigned count = 5000;
    std::thread producer([&] {
        uint8_t bytes[4096];
        mochi::FrameBatchWriter writer(tx);
        for (unsigned n = 0; n < count; ++n) {
            const auto size = n % 2 ? 4096u : n % 257;
            for (unsigned i = 0; i < size; ++i)
                bytes[i] = static_cast<uint8_t>(n * 17 + i * 31);
            for (;;) {
                const auto progress = writer.push(7, n, bytes, size);
                assert(progress != mochi::FrameProgress::Invalid);
                if (progress == mochi::FrameProgress::Complete)
                    break;
                writer.commit();
                std::this_thread::yield();
            }
            if (n % 7 == 6)
                writer.commit();
        }
        writer.commit();
    });
    uint8_t bytes[4096];
    mochi::FrameBatchReader reader(rx);
    mochi::FrameInfo info;
    for (unsigned n = 0; n < count; ++n) {
        for (;;) {
            const auto progress = reader.pop(info, bytes, sizeof(bytes));
            assert(progress != mochi::FrameProgress::Invalid);
            if (progress == mochi::FrameProgress::Complete)
                break;
            reader.commit();
            std::this_thread::yield();
        }
        assert(info.sequence == n && info.type == 7 && info.size == (n % 2 ? 4096 : n % 257));
        for (unsigned i = 0; i < info.size; ++i)
            assert(bytes[i] == static_cast<uint8_t>(n * 17 + i * 31));
        if (n % 5 == 4)
            reader.commit();
    }
    reader.commit();
    producer.join();
    assert(storage.page.head.v == storage.page.tail.v);
}
int main() {
    batches();
    mapping_teardown();
    channel_exchange();
    concurrent_batches();
}
