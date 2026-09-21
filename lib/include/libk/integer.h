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

} // namespace libk
