// Exercise the production arm/recheck/wait protocol with actual blocking OS
// threads. The seam models a counter-based kernel doorbell, not ring readiness.
#include "mochi/ring.h"
#include "mochi/inbox.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

static std::atomic<uint64_t> counter{0};
static std::mutex lock;
static std::condition_variable changed;

int main() {
    host_on_notify = [](CapHandle) -> mx_status_t {
        std::lock_guard guard(lock);
        counter.fetch_add(1, std::memory_order_release);
        changed.notify_one();
        return MX_OK;
    };
    host_read_counter = [](CapHandle) -> mx_u64_t { return counter.load(std::memory_order_acquire); };
    host_on_wait = []() -> mx_i64_t {
        std::unique_lock guard(lock);
        const bool woke = changed.wait_for(guard, std::chrono::seconds(2), [] {
            return counter.load(std::memory_order_acquire) != host_wait_expected;
        });
        assert(woke && "publication was missed by the arm/recheck/wait handshake");
        return MX_OK;
    };
    constexpr uint64_t rounds = 40000;
    for (unsigned variant = 0; variant < 3; ++variant) {
        struct Storage { RingPage page{}; RingSlot slots[4]{}; } storage;
        const mochi::Ring ring{&storage.page, 2, 1};
        RingPage* pages[] = {&storage.page};
        const mochi::RingReaderSet set{pages, 1, 2, 1};
        const mochi::InboxWait inbox{pages, 1, 2, 1};
        std::atomic<uint64_t> consumed{0};
        std::thread producer([&] {
            for (uint64_t n = 0; n < rounds; ++n) {
                while (!ring_try_push(ring.raw(), 2, 7, &n, sizeof(n), 1, &n)) std::this_thread::yield();
                // Force the next iteration to race a fresh empty/readiness check.
                while (consumed.load(std::memory_order_acquire) != n + 1)
                    std::this_thread::yield();
            }
        });
        for (uint64_t n = 0; n < rounds; ++n) {
            const auto check = [&](const RingSlot& slot) { assert(slot.seq == n && slot.length == sizeof(n)); };
            if (variant == 0) {
                RingSlot slot{}; assert(ring.wait_pop(&slot)); check(slot);
            } else if (variant == 1) {
                assert(set.wait(check));
            } else {
                RingSlot slot{};
                for (;;) {
                    const auto expected = sys_doorbell_read(1);
                    if (ring.try_pop(&slot)) break;
                    assert(inbox.wait(expected) == MX_OK);
                }
                check(slot);
            }
            consumed.store(n + 1, std::memory_order_release);
        }
        producer.join();
        assert(storage.page.head.v == rounds && storage.page.tail.v == rounds);
    }
}
