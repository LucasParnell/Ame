#include "mochi/boot_log.h"
#include "mochi/state.h"

extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, 0x18000000, "app.stream-test");
    mochi::boot::ready(boot);
    // Use raw arenas so make_state's initialization cannot hide uncleared
    // pages. Seed, release, and reallocate scratch before testing process exit.
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        const auto cap = sys_arena_create(16384, 0);
        if (cap <= 0 || sys_map_page(cap, 0x20000000, RIGHT_READ | RIGHT_WRITE) != 0) {
            mx_exit(MX_ERR_NOMEM);
            for (;;) mx_yield();
        }
        const auto bytes = libk::as_bytes(reinterpret_cast<void*>(0x20000000), 16384);
        if (cycle && !libk::is_zero(bytes)) {
            mochi::log_raw_marker("streamed app: scratch reuse was not zero\n");
            mx_exit(MX_ERR_FAULT);
            for (;;) mx_yield();
        }
        libk::fill(bytes, 0xa5);
        if (cycle != 3) {
            for (mx_u64_t off = 0; off < 16384; off += 4096) sys_unmap_page(0x20000000 + off);
            sys_cap_revoke(cap);
        }
    }
    mochi::log_raw_marker("streamed app: scratch reuse zero verified; exiting for cleanup\n");
    mx_exit(0);
    for (;;) mx_yield();
}
