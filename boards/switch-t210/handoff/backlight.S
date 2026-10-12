#include "midix/hw/tegra_backlight.h"

.section .text, "ax"
.global tegra_backlight_off
.type tegra_backlight_off, %function
// Returns original PWM0 in w0. No stack, waits, clocks or pinmux changes.
// Clobbers only x0/x9/x10; callable at EL3 reset or the EL2 BL33 entry.
tegra_backlight_off:
    ldr x9, =MIDIX_T210_PWM0
    ldr w0, [x9]
    bic w10, w0, #MIDIX_T210_PWM_DUTY_MASK
    str w10, [x9]
    dsb sy
    ret
.size tegra_backlight_off, . - tegra_backlight_off

.global tegra_backlight_restore
.type tegra_backlight_restore, %function
tegra_backlight_restore:
    ldr x9, =MIDIX_T210_PWM0
    str w0, [x9]
    dsb sy
    ret
.size tegra_backlight_restore, . - tegra_backlight_restore
