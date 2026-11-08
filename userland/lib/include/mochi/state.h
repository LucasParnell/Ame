#pragma once
#include "mochi/arena.h"
#include "mochi/syscall.h"
#include "mochi/managed_arena.h"
namespace mochi {
// Allocate only during service construction. Hot paths use the resulting arena.
inline Arena make_state(mx_u64_t va, mx_u64_t bytes) {
    mx_handle_t cap = 0;
    const MxArenaCreate request{bytes, bytes, 0x10000000, 0x30000000, 0, MX_ARENA_COMPACT_IDLE, 0};
    if (mx_arena_managed_create(request, &cap) != MX_OK) return {};
    mx_status_t status = MX_ERR_BUSY;
    for (unsigned attempt = 0; attempt < 64 && status == MX_ERR_BUSY; ++attempt) {
        status = mx_map_page(cap, va, MX_RIGHT_READ | MX_RIGHT_WRITE);
        if (status == MX_ERR_BUSY) sys_yield();
    }
    if (status != MX_OK) { (void)sys_cap_revoke(cap); return {}; }
    libk::zero(reinterpret_cast<void*>(va), bytes);
    return {reinterpret_cast<void*>(va), bytes};
}
} // namespace mochi
