#pragma once
#include "midix/abi/types.h"

// One firmware transition per animation, rather than two per frame. A client
// that disappears releases its boost after the same 5s watchdog as the overlay.
class RenderClockLease {
    mx_u64_t end_{};
    mx_i64_t idle_{};

  public:
    mx_u64_t deadline() const {
        return end_ ? end_ : ~mx_u64_t{0};
    }
    template <class Rate> void begin(mx_u64_t now, Rate rate) {
        if (!end_) {
            idle_ = rate(0);
            if (idle_ > 0 && idle_ < 480) {
                (void)rate(480);
            }
        }
        end_ = now + 5000000000ull;
    }
    template <class Rate> void finish(Rate rate) {
        if (end_ && idle_ > 0 && idle_ < 480) {
            (void)rate(idle_);
        }
        end_ = 0;
        idle_ = 0;
    }
    template <class Rate> void expire(mx_u64_t now, Rate rate) {
        if (end_ && now >= end_) {
            finish(rate);
        }
    }
};
