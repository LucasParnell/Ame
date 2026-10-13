#pragma once
#include "mochi/arena.h"
#include "mochi/syscall.h"
namespace mochi {
// Allocate only during service construction. Hot paths use the resulting arena.
inline Arena make_state(mx_u64_t va, mx_u64_t bytes) {
    mx_handle_t cap = 0;
    if (mx_arena_create(bytes, 0, &cap) != MX_OK ||
        mx_map_page(cap, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
        return {};
    }
    libk::zero(reinterpret_cast<void*>(va), bytes);
    return {reinterpret_cast<void*>(va), bytes};
}
} // namespace mochi
