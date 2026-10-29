#include "mochi/ring.h"
#include <chrono>
#include <cstdio>
#include <cassert>
#include <thread>
#include <atomic>

static uint64_t checksum_slot(const RingSlot& slot) {
    uint64_t sum=slot.seq;
    for (uint32_t i=0;i<48;++i) sum+=slot.payload[i];
    return sum;
}

int main() {
    constexpr uint32_t order=6, rounds=100000;
    struct Storage { RingPage page{}; RingSlot slots[1u<<order]{}; } storage;
    uint8_t data[48*16];
    for (uint32_t i=0;i<sizeof(data);++i) data[i]=static_cast<uint8_t>(i);
    RingPage* pages[]={&storage.page};
    mochi::RingReaderSet reader{pages,1,order,1};
    uint64_t checksum=0;
    const auto begin=std::chrono::steady_clock::now();
    for (uint32_t i=0;i<rounds;++i) {
        assert(ring_push_bytes(&storage.page,order,17,data,sizeof(data),1)==sizeof(data));
        assert(reader.drain([&](const RingSlot& slot) { checksum+=checksum_slot(slot); }));
    }
    const auto end=std::chrono::steady_clock::now();
    assert(checksum && storage.page.head.v==rounds*16ull && storage.page.head.v==storage.page.tail.v);
    std::printf("case=ring_16_slot_roundtrip ns=%.2f\n",
        std::chrono::duration<double,std::nano>(end-begin).count()/(rounds*16));
    // A singleton control message must retain its low-latency publication path.
    RingSlot slot{};
    const auto single_begin=std::chrono::steady_clock::now();
    for (uint32_t i=0;i<rounds;++i) {
        assert(ring_try_push(&storage.page,order,17,data,48,1));
        assert(ring_pop(&storage.page,order,&slot)); checksum+=slot.seq;
    }
    const auto single_end=std::chrono::steady_clock::now();
    assert(checksum);
    std::printf("case=ring_single_roundtrip ns=%.2f\n",
        std::chrono::duration<double,std::nano>(single_end-single_begin).count()/rounds);

    storage.page.head.v=storage.page.tail.v=0;
    constexpr uint64_t frames=160000;
    std::atomic<bool> go{false}; std::atomic<uint32_t> ready{0};
    std::thread producer([&] {
        ready.fetch_add(1,std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {}
        uint64_t sent=0;
        while (sent<frames*48) {
            const uint64_t left=frames*48-sent;
            const uint32_t size=left<sizeof(data)?static_cast<uint32_t>(left):sizeof(data);
            const auto written=ring_push_bytes(&storage.page,order,17,data,size,1);
            sent+=written;
            if (!written) std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        ready.fetch_add(1,std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {}
        uint64_t received=0;
        while (received<frames) {
            if (!reader.drain([&](const RingSlot& value) {
                assert(value.seq==received++ && value.length==48);
                checksum+=checksum_slot(value);
            })) std::this_thread::yield();
        }
    });
    while (ready.load(std::memory_order_acquire)!=2) {}
    const auto parallel_begin=std::chrono::steady_clock::now(); go.store(true,std::memory_order_release);
    producer.join(); consumer.join();
    const auto parallel_end=std::chrono::steady_clock::now();
    assert(checksum && storage.page.head.v==storage.page.tail.v);
    std::printf("case=ring_parallel_roundtrip ns=%.2f\n",
        std::chrono::duration<double,std::nano>(parallel_end-parallel_begin).count()/frames);
}
