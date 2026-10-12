#pragma once
// libk/integer.h — freestanding constexpr integer helpers.
//
// Small alignment / power-of-two arithmetic shared by the kernel and libk so
// page- and aperture-rounding has one definition instead of a copy per file.
// Freestanding: only <cstdint>, no libc.

#include <cstdint>

namespace libk {

// Round up to a multiple of align. Requires a nonzero power of two and a result
// representable in uint64_t; use checked_align_up for untrusted sizes.
constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t align) {
    return (value + (align - 1)) & ~(align - 1);
}

// Ceiling division without overflowing the numerator. Requires divisor != 0.
constexpr std::uint64_t div_round_up(std::uint64_t value, std::uint64_t divisor) {
    return value / divisor + (value % divisor != 0);
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

constexpr bool checked_align_up(std::uint64_t value, std::uint64_t align,
                                std::uint64_t* out) {
    if (align == 0 || (align & (align - 1)) != 0) return false;
    std::uint64_t padded = 0;
    if (!checked_add(value, align - 1, &padded)) return false;
    if (out) *out = padded & ~(align - 1);
    return true;
}

// Absolute value in unsigned storage, including INT64_MIN.
constexpr std::uint64_t magnitude(std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    return value < 0 ? std::uint64_t{0} - bits : bits;
}

} // namespace libk
