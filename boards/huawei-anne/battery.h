#pragma once
// Read-only view of the inherited Hi6555v100 fuel gauge. Register addresses,
// ADC scale and factory trims follow the pinned vendor driver in STATUS_BAR.md.
#include "libk/mmio.h"

namespace anne::battery {
inline bool voltage_mv(const libk::Mmio& pmic, unsigned& millivolts) {
    millivolts = 0;
    unsigned char version = 0, trim_low = 0, trim_high = 0;
    if (!pmic.read8(0x004 * 4, &version) || version != 1 ||
        !pmic.read8(0x182 * 4, &trim_low) || !pmic.read8(0x183 * 4, &trim_high)) return false;
    // PMUSSI has one byte per four-byte stride. Retry a torn ADC sample;
    // never issue a word read or write gauge/charger configuration.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        unsigned char high = 0, low = 0, check = 0;
        if (!pmic.read8(0x1f5 * 4, &high) || !pmic.read8(0x1f4 * 4, &low) ||
            !pmic.read8(0x1f5 * 4, &check)) return false;
        if (high != check) continue;
        const unsigned raw = low | (unsigned{high} << 8);
        if (!raw || raw & 0x8000) return false;
        const auto gain = 974400 + (trim_low | ((trim_high & 0x80) << 1)) * 100;
        const auto offset = -31500 + (trim_high & 0x7e) * 500;
        const auto uv = static_cast<long long>(raw) * 2400000 / 10661 * gain / 1000000 + offset;
        if (uv < 2500000 || uv > 5000000) return false;
        millivolts = static_cast<unsigned>((uv + 500) / 1000);
        return true;
    }
    return false;
}
} // namespace anne::battery
