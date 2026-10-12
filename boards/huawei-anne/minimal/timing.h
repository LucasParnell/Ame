#pragma once
#include <cstdint>

namespace anne::minimal {
// ANE-LX1: measured against recovery CLOCK_MONOTONIC and confirmed by the DTB.
// CNTFRQ_EL0 says 16384 even though CNTPCT/CNTVCT advance at 1.92 MHz.
constexpr std::uint64_t kCounterHz = 1920000;
constexpr std::uint64_t kLedHoldTicks = 3 * kCounterHz;
constexpr std::uint64_t kBlinkTicks = kCounterHz / 4;
constexpr bool elapsed(std::uint64_t start, std::uint64_t now, std::uint64_t ticks) {
    return now - start >= ticks;
}
#if defined(__aarch64__)
inline std::uint64_t counter() {
    std::uint64_t value;
    asm volatile("isb\n mrs %0, cntpct_el0" : "=r"(value) :: "memory");
    return value;
}
inline void wait_ticks(std::uint64_t ticks) {
    const auto start = counter();
    while (!elapsed(start, counter(), ticks)) asm volatile("nop");
}
#endif
}
