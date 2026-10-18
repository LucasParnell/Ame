#pragma once
// mochi/ipc_budget.h - bounded poll-budget controller.
//
// The kernel stores one budget per principal; this controller decides what that
// budget should be from observed waits. It is deliberately tiny and
// deterministic: one window of waits, two thresholds, no clocks, no allocation,
// no floating point. A service embeds it, calls note_wait() after every wait,
// and applies a non-zero return through mx_ipc_policy_set_budget().
//
// Shape is AIMD on purpose: grow fast while the peer keeps answering just after
// we slept, and give budget back only after a long quiet stretch. Decaying on
// every quiet window made the budget collapse between bursts and then climb
// again, which cost more latency than it saved.

#include <cstdint>

namespace mochi {

struct BudgetController {
    // Floor stays non-zero: a scalar bulk stream still parks rarely at 64, and
    // the dead band then holds it there instead of flip-flopping with 0.
    static constexpr uint32_t kMin = 64;
    static constexpr uint32_t kMax = 4096;
    static constexpr uint32_t kWindow = 64;
    static constexpr uint32_t kGrowParks = kWindow / 2;
    // Consecutive empty windows before a single 1/8 step down.
    static constexpr uint32_t kQuietWindows = 8;

    uint32_t budget = 0;
    uint32_t waits = 0;
    uint32_t parks = 0;
    uint32_t quiet = 0;

    // Seed from the kernel policy. Values above the cap are not trusted.
    void seed(uint32_t policy) { budget = policy > kMax ? kMax : policy; }

    // Record one wait. Returns the new budget when it changed, else 0.
    uint32_t note_wait(bool parked) {
        ++waits;
        if (parked) ++parks;
        if (waits < kWindow) return 0;

        uint32_t next = budget;
        if (parks >= kGrowParks) {
            next = budget < kMin ? kMin : (budget > kMax / 2 ? kMax : budget * 2);
            quiet = 0;
        } else if (parks == 0) {
            if (++quiet >= kQuietWindows && budget > kMin) {
                const uint32_t decayed = budget - budget / 8;
                next = decayed > kMin ? decayed : kMin;
                quiet = 0;
            }
        } else {
            quiet = 0;   // some parks: hold, this is the dead band
        }
        waits = 0;
        parks = 0;
        if (next == budget) return 0;
        budget = next;
        return next;
    }
};

} // namespace mochi
