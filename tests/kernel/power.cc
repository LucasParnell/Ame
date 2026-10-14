// Isolated QEMU image. Exercise real capability checks and PSCI transitions;
// never include this process in the phone's normal signed service profile.
#include "mochi/boot.h"
#include "midix/abi/platform.h"
#include "libk/str.h"

#ifndef MOCHI_TEST_POWER_OPERATION
#define MOCHI_TEST_POWER_OPERATION MX_PLATFORM_SYSTEM_OFF
#endif

static void print(const char* text) { sys_debug_puts(text, libk::str::length(text)); }
[[noreturn]] static void fail() {
    print("POWER_TEST FAILED\n");
    for (;;) mx_yield();
}
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    const auto cap = mochi::boot::require_handle(boot, "system.control");
    if (mx_platform_control(cap, MX_PLATFORM_SYSTEM_CHECK, MOCHI_TEST_POWER_OPERATION) != MX_ERR_PERM ||
        mx_platform_control(cap, MX_PLATFORM_CLAIM) != MX_OK ||
        mx_platform_control(cap, MX_PLATFORM_SYSTEM_CHECK, MOCHI_TEST_POWER_OPERATION) != MX_OK ||
        mx_platform_control(cap, MX_PLATFORM_SYSTEM_CHECK, MX_PLATFORM_SYSTEM_FASTBOOT) != MX_ERR_NOT_IMPLEMENTED ||
        mx_platform_control(cap, MX_PLATFORM_SYSTEM_FASTBOOT) != MX_ERR_NOT_IMPLEMENTED ||
        mx_platform_control(cap, MX_PLATFORM_SYSTEM_RESET, 1) != MX_ERR_INVALID ||
        mx_platform_control(cap, MX_PLATFORM_SYSTEM_CHECK, ~uint64_t{0}) != MX_ERR_INVALID)
        fail();
    print("POWER_TEST checks passed; transition requested\n");
    (void)mx_platform_control(cap, MOCHI_TEST_POWER_OPERATION);
    fail();
}
