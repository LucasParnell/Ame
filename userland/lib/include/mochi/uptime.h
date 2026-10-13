// mochi/uptime.h — kernel-published uptime for userland services.
//
// The kernel records the system counter once at kernel entry and exposes the
// elapsed time as centiseconds through SYS_DEBUG_COUNTERS id 70 (DBG_BOOT_CS).
// The msh boot banner prints this value as the boot time.
#pragma once
#include "mochi/syscall.h"

namespace mochi {
// Centiseconds from kernel entry to now. Returns 0 when the running kernel
// does not provide the counter (older builds), so callers can fall back.
static inline uint64_t uptime_centiseconds() {
    const int64_t value = sys_debug_counter(70);
    return value > 0 ? static_cast<uint64_t>(value) : 0;
}
} // namespace mochi
