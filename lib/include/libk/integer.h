#pragma once
// libk/integer.h — freestanding constexpr integer helpers.
//
// Small alignment / power-of-two arithmetic shared by the kernel and libk so
// page- and aperture-rounding has one definition instead of a copy per file.
// Freestanding: only <cstdint>, no libc.

#include <cstdint>

namespace libk {

// Round `value` up to the next multiple of `align` (align must be a power of two).
constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t align) {
    return (value + (align - 1)) & ~(align - 1);
}

// Ceiling division: (value + divisor - 1) / divisor.
constexpr std::uint64_t div_round_up(std::uint64_t value, std::uint64_t divisor) {
    return (value + (divisor - 1)) / divisor;
}

// Overflow-checked arithmetic: false on wrap and *out is left untouched, so a
// caller can never mistake a wrapped result for a real one.
constexpr bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    constexpr std::uint64_t kMax = ~static_cast<std::uint64_t>(0);
    if (a > kMax - b) return false;
    if (out) *out = a + b;
    return true;
}
constexpr bool checked_sub(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (a < b) return false;
    if (out) *out = a - b;
    return true;
}
constexpr bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    constexpr std::uint64_t kMax = ~static_cast<std::uint64_t>(0);
    if (a != 0 && b > kMax / a) return false;
    if (out) *out = a * b;
    return true;
}

} // namespace libk
