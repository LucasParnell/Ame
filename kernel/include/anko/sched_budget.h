#pragma once

#include "anko/types.h"

namespace sched {

// A rollover and a charge must commit together. Two independent atomics can
// lose a charge when another CPU resets the period between those operations.
// Keep both full-width counters in one naturally aligned compare/exchange.
// AArch64 lowers this to LDXP/STXP (including validation of a paired load),
// without requiring LSE or an out-of-line lock. Reject unsupported toolchains.
class alignas(16) BudgetWindow {
    using Word = unsigned __int128;
    Word state_;

    static constexpr Word pack(u64 used, u64 start) {
        return static_cast<Word>(start) << 64 | used;
    }

  public:
    struct Snapshot { u64 used_ns; u64 period_start_ns; };

    void reset(u64 start) { __atomic_store_n(&state_, pack(0, start), __ATOMIC_RELAXED); }

    Snapshot snapshot() const {
        const Word state = __atomic_load_n(&state_, __ATOMIC_RELAXED);
        return {static_cast<u64>(state), static_cast<u64>(state >> 64)};
    }

    // delta == 0 also implements eligibility/refill. Failed CAS means another
    // CPU made progress; recompute against its committed period and usage.
    // A late timestamp charges the current window, matching serial accounting.
    Snapshot charge(u64 delta, u64 now, u64 period) {
        Word observed = __atomic_load_n(&state_, __ATOMIC_RELAXED);
        for (;;) {
            u64 used = static_cast<u64>(observed);
            u64 start = static_cast<u64>(observed >> 64);
            if (period && now >= start && now - start >= period) {
                start += ((now - start) / period) * period;
                used = 0;
            }
            // Saturation preserves exhaustion instead of wrapping into credit.
            used = delta > ~0ull - used ? ~0ull : used + delta;
            const Word desired = pack(used, start);
            if (desired == observed || __atomic_compare_exchange_n(
                    &state_, &observed, desired, false,
                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                return {used, start};
        }
    }
};

static_assert(sizeof(BudgetWindow) == 16 && alignof(BudgetWindow) == 16);
static_assert(__atomic_always_lock_free(16, nullptr),
              "scheduler budgets require native lock-free 128-bit atomics");

} // namespace sched
