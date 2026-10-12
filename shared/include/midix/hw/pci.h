#pragma once
#include "midix/abi/types.h"

namespace midix::hw::pci {

constexpr mx_u64_t memory_bar_size(mx_u32_t low_mask, mx_u32_t high_mask, bool wide) {
    const auto low = low_mask & ~mx_u32_t{15};
    if (wide) {
        const auto mask = (static_cast<mx_u64_t>(high_mask) << 32) | low;
        return mask ? ~mask + 1 : 0;
    }
    return low ? static_cast<mx_u64_t>(static_cast<mx_u32_t>(~low + 1)) : 0;
}

} // namespace midix::hw::pci
