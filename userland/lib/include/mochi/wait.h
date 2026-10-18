#pragma once
#include "mochi/syscall.h"

namespace mochi {

// For a service with no work source (for example a placeholder driver). EL0
// must block through the scheduler rather than issue WFI or repeatedly yield.
// Never share this private doorbell: only lifecycle termination can end park.
[[noreturn]] inline void park() {
    mx_handle_t doorbell = MX_NULL_HANDLE;
    if (mx_doorbell_create(&doorbell) != MX_OK || doorbell == MX_NULL_HANDLE) {
        mx_exit(MX_ERR_NOMEM);
        __builtin_trap();
    }
    for (;;) {
        const auto expected = mx_doorbell_read(doorbell);
        if (sys_wait_doorbell(doorbell, expected) < 0) {
            mx_exit(MX_ERR_DEAD);
            __builtin_trap();
        }
    }
}

} // namespace mochi
