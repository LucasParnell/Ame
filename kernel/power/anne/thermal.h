#pragma once
#include <cstdint>
namespace anne::thermal {
// Hi6250 tsens transfer function from the firmware DT and vendor driver.
// Preserve milli-degree precision and negative temperatures; firmware error
// values must fail validation rather than become a fabricated zero degrees.
inline bool millicelsius(std::uint64_t adc, std::uint32_t start, std::uint32_t end,
                         std::int32_t& out) {
    if (!start || start >= end || adc < start || adc > end)
        return false;
    out = static_cast<std::int32_t>((adc - start) * 165000 / (end - start)) - 40000;
    return true;
}
} // namespace anne::thermal
