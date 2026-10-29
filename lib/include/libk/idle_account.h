#pragma once
#include <cstdint>

namespace libk {
// Single CPU writer with interrupts excluded. Every shared field is atomic:
// retrying a sequence alone does not make ordinary C++ data races legal.
// One line per CPU; bounded readers also work in IRQ context on the writer CPU.
struct alignas(64) IdleAccount {
    std::uint64_t sequence = 0, total = 0, entered = 0, entries = 0;
    std::uint32_t waiting = 0;
    void reset() {
        sequence = total = entered = entries = waiting = 0;
    } // before use
    void enter(std::uint64_t now) {
        __atomic_add_fetch(&sequence, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&entered, now, __ATOMIC_RELAXED);
        __atomic_store_n(&waiting, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&entries, __atomic_load_n(&entries, __ATOMIC_RELAXED) + 1,
                         __ATOMIC_RELAXED);
        __atomic_add_fetch(&sequence, 1, __ATOMIC_SEQ_CST);
    }
    void leave(std::uint64_t now) {
        __atomic_add_fetch(&sequence, 1, __ATOMIC_SEQ_CST);
        const auto start = __atomic_load_n(&entered, __ATOMIC_RELAXED);
        const auto elapsed = now >= start ? now - start : 0;
        __atomic_store_n(&total, __atomic_load_n(&total, __ATOMIC_RELAXED) + elapsed,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&waiting, 0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&sequence, 1, __ATOMIC_SEQ_CST);
    }
    bool read(std::uint64_t now, std::uint64_t& idle, std::uint64_t& count) const {
        for (unsigned retry = 0; retry < 4; ++retry) {
            const auto before = __atomic_load_n(&sequence, __ATOMIC_SEQ_CST);
            if (before & 1)
                continue;
            const auto sum = __atomic_load_n(&total, __ATOMIC_RELAXED);
            const auto start = __atomic_load_n(&entered, __ATOMIC_RELAXED);
            const auto active = __atomic_load_n(&waiting, __ATOMIC_RELAXED);
            const auto n = __atomic_load_n(&entries, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (before != __atomic_load_n(&sequence, __ATOMIC_SEQ_CST))
                continue;
            // A reader's timestamp can precede a newer writer publication.
            idle = sum + (active && now >= start ? now - start : 0);
            count = n;
            return true;
        }
        return false;
    }
};
static_assert(sizeof(IdleAccount) == 64);
} // namespace libk
