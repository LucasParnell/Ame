#pragma once
#include <cstdint>
#include "libk/cpu.h"
#include "libk/wait.h"

namespace libk {

// Hint only; publication and ownership come from the atomics, never the hint.
inline void spin_pause() {
    cpu::relax();
}

// Caller owns interrupt/preemption policy. Poll a contended lock with ordinary
// shared reads: repeated exchanges would invalidate the owner's cache line.
inline void spin_lock(std::uint32_t& word) {
    while (__atomic_exchange_n(&word, 1u, __ATOMIC_ACQUIRE)) {
        wait_until([&word] { return __atomic_load_n(&word, __ATOMIC_RELAXED) == 0u; });
    }
}
inline void spin_unlock(std::uint32_t& word) {
    __atomic_store_n(&word, 0u, __ATOMIC_RELEASE);
    // Wake cores parked in WFE; the store above is already published.
    signal_event();
}

} // namespace libk
