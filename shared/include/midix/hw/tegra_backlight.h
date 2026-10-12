#pragma once

// Tegra210 PWM0, used by Hekate for the Erista LCD backlight. Kept usable by
// assembly so diagnostics can signal entry before stack or C++ initialization.
#define MIDIX_T210_PWM0 0x7000a000
#define MIDIX_T210_PWM_DUTY_MASK 0x00ff0000
