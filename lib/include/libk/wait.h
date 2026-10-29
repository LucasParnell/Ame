#pragma once
// Event-based waiting for shared state.
//
// WFE parks the core until an event is signalled (SEV) or an exception is
// taken. The event register is set by SEV and by taking an interrupt, so a
// signal that lands between the predicate test and the WFE is not lost: the
// WFE returns immediately. That property is what makes the pattern below
// correct without a second barrier.
//
// Callers must re-test after every wake: WFE also returns for unrelated SEV and
// for interrupts, and it is not a memory barrier.
#include <cstdint>
#include "libk/cpu.h"

namespace libk {

// Wake every core parked in WFE. Cheap and idempotent.
inline void signal_event() {
    cpu::send_event();
}

// Wait until `predicate` holds. The signaller must call signal_event() after
// publishing the state change, or the waiter can sleep through it.
template <typename Predicate> inline void wait_until(Predicate predicate) {
    while (!predicate()) {
        cpu::wait_for_event();
    }
}

// Poll without sleeping. For state that changes with no signaller to SEV on our
// behalf, spinning is correct but wasteful: prefer wait_until or a blocking
// syscall wherever a wake source exists.
template <typename Predicate> inline void poll_until(Predicate predicate) {
    while (!predicate()) {
        cpu::relax();
    }
}

// Bound a wait by a monotonic clock. `now` must return the same units as
// `deadline`. Returns false if the deadline passed first.
template <typename Predicate, typename Clock>
inline bool wait_until_or(Predicate predicate, Clock now, std::uint64_t deadline) {
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (now() >= deadline) {
            return false;
        }
        cpu::wait_for_event();
    }
}

} // namespace libk
