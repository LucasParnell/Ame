#pragma once
// Host-only syscall seam. Ring algorithms and ABI layouts remain production code.
#include <cstdint>
#include "midix/abi/types.h"
#include "midix/abi/status.h"
#include "midix/abi/cap.h"

using CapHandle = mx_handle_t;
constexpr CapHandle kNullCap = MX_NULL_HANDLE;

inline unsigned host_yields = 0;
inline unsigned host_notifies = 0;
inline mx_u64_t host_now = 0;
inline void (*host_on_yield)() = nullptr;
inline void (*host_on_read)() = nullptr;
inline mx_i64_t (*host_on_wait)() = nullptr;

inline void sys_yield() {
    ++host_yields;
    ++host_now;
    if (host_on_yield) host_on_yield();
}
inline mx_status_t mx_doorbell_notify(CapHandle) { ++host_notifies; return MX_OK; }
inline mx_u64_t mx_clock_ns() { return host_now; }
inline mx_u64_t sys_doorbell_read(CapHandle) {
    if (host_on_read) host_on_read();
    return host_notifies;
}
inline mx_i64_t sys_wait_doorbell(CapHandle, mx_u64_t) {
    return host_on_wait ? host_on_wait() : MX_ERR_DEAD;
}
inline void mx_yield() { sys_yield(); }
inline void mx_exit(mx_i32_t) {}
inline void sys_debug_puts(const char*, mx_u64_t) {}
inline mx_i64_t sys_debug_counter(mx_u64_t) { return 0; }
inline mx_status_t mx_cap_identify(CapHandle, mx_i64_t*) { return MX_ERR_INVALID; }
inline mx_status_t mx_map_page(CapHandle, mx_vaddr_t, mx_rights_t) { return MX_ERR_INVALID; }
inline mx_status_t sys_unmap_page(mx_vaddr_t) { return MX_OK; }
inline mx_status_t sys_notify(CapHandle cap) { return mx_doorbell_notify(cap); }
inline mx_i64_t sys_arena_register(mx_vaddr_t, mx_u64_t, mx_u32_t) { return 1; }
