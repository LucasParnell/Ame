#include "mochi/boot.h"
extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    auto parent = mochi::boot::require_handle(boot, "parent");
    auto result = mochi::boot::require_handle(boot, "result");
    sys_map_page(result, 0x31000000, RIGHT_READ | RIGHT_WRITE);
    auto arena = static_cast<CapHandle>(sys_arena_create(4096, 0));
    sys_map_page(arena, 0x31010000, RIGHT_READ | RIGHT_WRITE);
    *reinterpret_cast<volatile uint64_t*>(0x31010000) = 0x123456789abcdef0ull;
    auto shared = sys_cap_grant(parent, arena, RIGHT_READ | RIGHT_MAP);
    *reinterpret_cast<volatile uint64_t*>(0x31000000) = static_cast<uint64_t>(shared);
    sys_exit(42);
    while (true) {
        sys_yield();
    }
}
