#include "mochi/ring.h"
#include <cassert>
#include <thread>
#include <vector>

int main() {
    constexpr uint32_t order=6, bytes=1024*1024+17, kType=17;
    struct Storage { RingPage header{}; RingSlot slots[1u<<order]{}; } storage;
    static_assert(__builtin_offsetof(Storage,slots)==MX_RING_HEADER_SIZE);
    storage.header.head.v=storage.header.tail.v=~uint64_t{0}-27;
    std::vector<uint8_t> source(bytes);
    for (uint32_t i=0;i<bytes;++i) source[i]=static_cast<uint8_t>(i*37);
    std::thread producer([&] {
        uint32_t sent=0;
        while (sent<bytes) {
            const auto remaining=bytes-sent;
            const auto run=remaining<777?remaining:777;
            const auto written=ring_push_bytes(&storage.header,order,kType,source.data()+sent,run,1);
            sent+=written;
            if (!written) std::this_thread::yield();
        }
    });
    uint32_t received=0;
    uint64_t sequence=storage.header.tail.v;
    RingSlot slots[9];
    while (received<bytes) {
        const auto count=ring_pop_batch(&storage.header,order,slots,9);
        if (!count) std::this_thread::yield();
        for (uint32_t i=0;i<count;++i) {
            const auto& slot=slots[i];
            assert(slot.type==kType && slot.seq==sequence++ && slot.length<=48);
            for (uint32_t j=0;j<slot.length;++j) assert(slot.payload[j]==source[received++]);
            for (uint32_t j=slot.length;j<48;++j) assert(slot.payload[j]==0);
        }
    }
    producer.join();
    assert(storage.header.head.v==storage.header.tail.v && !host_notifies);
}
