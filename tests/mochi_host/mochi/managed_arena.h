#pragma once
// Service probes substitute the syscall boundary; production arena mechanics
// are covered by amm_probe and the QEMU lifecycle matrix.
#include "mochi/syscall.h"
#include "midix/abi/arena.h"
inline mx_status_t mx_arena_managed_create(const MxArenaCreate& request, mx_handle_t* out) {
    if (!request.size || request.size != request.capacity || request.reserved) return MX_ERR_INVALID;
    return mx_arena_create(request.size, 0, out);
}
inline mx_status_t mx_arena_unmap_cap(mx_handle_t, mx_u64_t va) { return sys_unmap_page(va); }
