// Exercise both sides of the production credit/record protocol with real
// sleeping host threads. The syscall seam checks cursors under its notify lock.
#include "mochi/framed_ring.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

struct Storage { RingPage page{}; RingSlot slots[128]{}; } storage;
static std::mutex lock;
static std::condition_variable progress;

int main() {
    storage.page.space.features = MX_RING_PACKED_FRAMES;
    storage.page.head.v = storage.page.tail.v = ~uint64_t{0} - 47;
    mochi::Ring ring{&storage.page, 7, 1, 9, 2};
    host_on_notify = [](CapHandle) -> mx_status_t {
        std::lock_guard guard(lock);
        progress.notify_all();
        return MX_OK;
    };
    host_on_wait_ring = [](CapHandle handle, CapHandle db, uint64_t cursor, uint32_t direction) -> mx_i64_t {
        assert(handle == 9 && db == (direction ? 2 : 1));
        std::unique_lock guard(lock);
        const bool ready = progress.wait_for(guard, std::chrono::seconds(3), [&] {
            return __atomic_load_n(direction ? &storage.page.tail.v : &storage.page.head.v,
                                   __ATOMIC_ACQUIRE) != cursor;
        });
        assert(ready && "missed data/credit publication");
        return MX_OK;
    };
    constexpr unsigned count = 6000;
    std::thread producer([&] {
        alignas(16) uint8_t bytes[4096];
        for (unsigned n = 0; n < count; ++n) {
            const unsigned length = n % 3 == 0 ? 4096 : n % 257;
            for (unsigned i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(n + i * 31);
            mochi::FrameWriter writer(17, n, bytes, length);
            for (;;) {
                const auto result = writer.push(ring);
                assert(result != mochi::FrameProgress::Invalid);
                if (result == mochi::FrameProgress::Complete) break;
                if (result == mochi::FrameProgress::Blocked)
                    assert(ring.wait_writable(writer.required_space(ring), 64));
            }
        }
    });
    alignas(16) uint8_t bytes[4096];
    for (unsigned n = 0; n < count; ++n) {
        mochi::FrameReader reader(bytes, sizeof(bytes));
        for (;;) {
            const auto result = reader.pop(ring);
            assert(result != mochi::FrameProgress::Invalid);
            if (result == mochi::FrameProgress::Complete) break;
            if (result == mochi::FrameProgress::Blocked) assert(ring.wait_readable(64));
        }
        const unsigned length = n % 3 == 0 ? 4096 : n % 257;
        assert(reader.sequence() == n && reader.type() == 17 && reader.size() == length);
        for (unsigned i = 0; i < length; ++i) assert(bytes[i] == static_cast<uint8_t>(n + i * 31));
    }
    producer.join();
    assert(storage.page.head.v == storage.page.tail.v && host_yields == 0);
}
