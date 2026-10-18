#pragma once
// Exact hardware-counter ticks -> nanoseconds. Configure once from the 32-bit
// CNTFRQ value, then read immutable coefficients on every CPU. No floating
// point, division on the read path, approximate clock drift, or 128-bit divide.
#include <cstdint>

namespace libk {
class CounterScale {
    using u64 = std::uint64_t;
    u64 divisor_ = 0, reciprocal_ = 0, numerator_ = 1;
    struct Division { u64 quotient, remainder; };
    Division divide(u64 value) const {
        // reciprocal = floor((2^64 - 1) / divisor). The high product is never
        // above floor(value/divisor) and is at most one below it. Since the
        // estimate cannot overshoot, quotient*divisor cannot overflow.
        u64 quotient = static_cast<u64>((static_cast<unsigned __int128>(value) * reciprocal_) >> 64);
        u64 remainder = value - quotient * divisor_;
        if (remainder >= divisor_) { ++quotient; remainder -= divisor_; }
        return {quotient, remainder};
    }
public:
    void configure(std::uint32_t frequency) {
        if (!frequency) { divisor_ = 0; reciprocal_ = 0; numerator_ = 1; return; }
        u64 a = frequency, b = 1000000000;
        while (b) { const u64 remainder = a % b; a = b; b = remainder; }
        divisor_ = frequency / a;
        numerator_ = 1000000000 / a;
        reciprocal_ = ~u64{0} / divisor_;
    }
    u64 nanoseconds(u64 ticks) const {
        if (!divisor_) return ticks; // early boot, before CNTFRQ is known
        if (divisor_ == 1) return ticks * numerator_;
        const auto whole = divide(ticks);
        // remainder < reduced 32-bit frequency; numerator <= 1e9, so this
        // product fits in 64 bits. Final overflow matches the u64 ns clock.
        return whole.quotient * numerator_ + divide(whole.remainder * numerator_).quotient;
    }
};
} // namespace libk
