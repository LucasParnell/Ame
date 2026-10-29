#pragma once
#include <cstdint>

namespace anne::clocks {
// Hi6250 PPLL0/PPLL3 register layout, from Huawei's SoC interface headers.
// PPLL0 has a separate lock-status register; the caller checks lock and gates.
constexpr std::uint64_t pll_rate(std::uint32_t control0, std::uint32_t control1) {
    constexpr std::uint64_t reference = 19200000;
    if (!(control0 & 1u))
        return 0;
    if (control0 & 2u)
        return reference;

    const auto reference_divisor = (control0 >> 2) & 63u;
    const auto feedback = (control0 >> 8) & 4095u;
    const auto post1 = (control0 >> 20) & 7u;
    const auto post2 = (control0 >> 23) & 7u;
    if (!reference_divisor || !feedback || !post1 || !post2)
        return 0;

    const std::uint64_t fraction = control1 & (1u << 24) ? 0 : control1 & 0xffffffu;
    const auto multiplier = (std::uint64_t{feedback} << 24) + fraction;
    const auto divisor = std::uint64_t{reference_divisor} * post1 * post2 * (1u << 24);
    return reference * multiplier / divisor;
}
} // namespace anne::clocks
