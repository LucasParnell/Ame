#include "mochi/boot_log.h"
#include "libk/printf.h"

extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, 0x18000000, "app.stream-metrics");
    mochi::boot::ready(boot);
    char line[112];
    libk::snprintf(line, sizeof(line), "stream metrics: objects=%lld pages=%lld\n",
                   static_cast<long long>(sys_debug_counter(200)),
                   static_cast<long long>(sys_debug_counter(201)));
    mochi::log_raw_marker(line);
    mx_exit(0);
    for (;;) mx_yield();
}
