#pragma once
#include "midix/abi/types.h"
// Board mechanisms are capability-scoped. Arguments are masks within one fixed
// reset domain, never register addresses. Sequencing and role policy are userland.
enum MxPlatformOp : mx_u32_t {
    MX_PLATFORM_CLAIM = 1,
    MX_PLATFORM_USB_CLOCK_ON = 2,
    MX_PLATFORM_USB_CLOCK_OFF = 3,
    MX_PLATFORM_RESET_ASSERT = 4,
    MX_PLATFORM_RESET_RELEASE = 5,
    MX_PLATFORM_USB_REFERENCE = 6,
    MX_PLATFORM_I2C_CLOCK_ON = 7,
    MX_PLATFORM_DISPLAY_HANDOFF = 8,
    MX_PLATFORM_I2C_PINS = 9,
};
constexpr mx_u32_t MX_ANNE_USB_RESETS = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |
                                        (1u << 12) | (1u << 13) | (1u << 14) | (1u << 27);
