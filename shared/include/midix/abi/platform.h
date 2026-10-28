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
    // Machine power. The kernel owns the platform sequence (PSCI on arm64;
    // board fallbacks live in the kernel's platform layer).
    MX_PLATFORM_SYSTEM_OFF = 10,
    MX_PLATFORM_SYSTEM_RESET = 11,
    MX_PLATFORM_SYSTEM_FASTBOOT = 12,
    // Read-only: argument is one of SYSTEM_OFF/RESET/FASTBOOT. Returns status
    // before init stops services; unsupported transitions must leave them live.
    MX_PLATFORM_SYSTEM_CHECK = 13,
    MX_PLATFORM_GPU_RATE = 14,      // arg=0: observed MHz; 120/240/360/480: firmware DVFS request
    MX_PLATFORM_DISPLAY_BIND0 = 15, // arg=owned CAP_ARENA; fixed 512x512 RGB geometry
    MX_PLATFORM_DISPLAY_BIND1 = 16,
    MX_PLATFORM_DISPLAY_FLIP = 17,    // arg=0/1; BUSY until a fresh display timing edge
    MX_PLATFORM_DISPLAY_RESTORE = 18, // arg=0; same completion/retirement rule
    MX_PLATFORM_DISPLAY_STATUS = 19,  // ready/live/fault bits, completed transitions above bit 7
};
constexpr mx_u32_t MX_ANNE_USB_RESETS = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |
                                        (1u << 12) | (1u << 13) | (1u << 14) | (1u << 27);
