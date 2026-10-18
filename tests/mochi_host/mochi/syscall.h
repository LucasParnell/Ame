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
inline mx_u64_t host_wait_expected = 0, host_wait_deadline = 0;
inline mx_u64_t host_now = 0;
inline mx_status_t (*host_on_notify)(CapHandle) = nullptr;
inline mx_u64_t (*host_read_counter)(CapHandle) = nullptr;
inline void (*host_on_yield)() = nullptr;
inline void (*host_on_read)() = nullptr;
inline mx_i64_t (*host_on_wait)() = nullptr;
inline mx_status_t (*host_on_doorbell_create)(mx_handle_t*) = nullptr;
inline mx_status_t mx_doorbell_create(mx_handle_t* out) {
    if (host_on_doorbell_create) return host_on_doorbell_create(out);
    *out = 98;
    return MX_OK;
}
inline mx_status_t (*host_on_map_page)(CapHandle, mx_vaddr_t, mx_rights_t) = nullptr;
inline mx_status_t (*host_on_cap_info)(CapHandle, MxCapInfo*) = nullptr;
inline mx_status_t (*host_on_map_mmio)(CapHandle, mx_vaddr_t, mx_rights_t) = nullptr;
inline mx_status_t (*host_on_cap_grant)(CapHandle, CapHandle, mx_rights_t, CapHandle*) = nullptr;
inline mx_status_t (*host_on_cap_revoke)(CapHandle) = nullptr;
inline mx_status_t (*host_on_arena_create)(mx_u64_t, mx_u32_t, CapHandle*) = nullptr;
inline mx_i64_t (*host_on_debug_read)(mx_u64_t, mx_u64_t, void*, mx_u64_t) = nullptr;

inline void sys_yield() {
    ++host_yields;
    ++host_now;
    if (host_on_yield) host_on_yield();
}
inline mx_status_t mx_doorbell_notify(CapHandle cap) {
    if (host_on_notify) return host_on_notify(cap);
    ++host_notifies; return MX_OK;
}
inline mx_u64_t mx_clock_ns() { return host_now; }
inline mx_u64_t sys_doorbell_read(CapHandle cap) {
    if (host_read_counter) return host_read_counter(cap);
    if (host_on_read) host_on_read();
    return host_notifies;
}
inline mx_i64_t sys_wait_doorbell(CapHandle, mx_u64_t expected) {
    host_wait_expected = expected;
    return host_on_wait ? host_on_wait() : MX_ERR_DEAD;
}
inline mx_i64_t (*host_on_wait_ring)(CapHandle, CapHandle, mx_u64_t, mx_u32_t) = nullptr;
inline mx_i64_t sys_wait_ring(CapHandle ring, CapHandle db, mx_u64_t cursor, mx_u32_t direction) {
    return host_on_wait_ring ? host_on_wait_ring(ring, db, cursor, direction) : MX_ERR_DEAD;
}
inline mx_i64_t sys_notify_wait(CapHandle notify, CapHandle ring, CapHandle db, mx_u64_t cursor, mx_u32_t = 0) {
    const auto rc = mx_doorbell_notify(notify);
    return rc < 0 ? rc : sys_wait_ring(ring, db, cursor, 0);
}
inline void mx_yield() { sys_yield(); }
inline void mx_exit(mx_i32_t) {}
inline void sys_debug_puts(const char*, mx_u64_t) {}
inline mx_i64_t sys_debug_counter(mx_u64_t) { return 0; }
inline mx_i64_t sys_cap_identify(CapHandle cap) {
    return cap ? mx_i64_t{MX_CAP_PROCESS} : mx_i64_t{MX_ERR_NOT_FOUND};
}
inline mx_i64_t sys_debug_read(mx_u64_t request, mx_u64_t index, void* out, mx_u64_t size) {
    return host_on_debug_read ? host_on_debug_read(request, index, out, size) : MX_ERR_NOT_IMPLEMENTED;
}
inline mx_status_t mx_cap_identify(CapHandle, mx_i64_t*) { return MX_ERR_INVALID; }
inline mx_status_t mx_map_page(CapHandle cap, mx_vaddr_t va, mx_rights_t rights) {
    return host_on_map_page ? host_on_map_page(cap, va, rights) : MX_ERR_INVALID;
}
inline mx_status_t sys_unmap_page(mx_vaddr_t) { return MX_OK; }
inline mx_status_t sys_notify(CapHandle cap) { return mx_doorbell_notify(cap); }
inline mx_i64_t sys_arena_register(mx_vaddr_t, mx_u64_t, mx_u32_t) { return 1; }
// Cold service mapping helpers fail in host probes; packet tests supply views.
inline mx_status_t mx_cap_info(CapHandle cap, MxCapInfo* info) {
    return host_on_cap_info ? host_on_cap_info(cap, info) : MX_ERR_INVALID;
}
inline mx_status_t mx_map_mmio(CapHandle cap, mx_vaddr_t va, mx_rights_t rights) {
    return host_on_map_mmio ? host_on_map_mmio(cap, va, rights) : MX_ERR_INVALID;
}
inline mx_status_t mx_arena_create(mx_u64_t size, mx_u32_t flags, CapHandle* out) {
    return host_on_arena_create ? host_on_arena_create(size, flags, out) : MX_ERR_INVALID;
}
inline mx_status_t mx_cap_grant(CapHandle process, CapHandle source, mx_rights_t rights, CapHandle* out) {
    return host_on_cap_grant ? host_on_cap_grant(process, source, rights, out) : MX_ERR_INVALID;
}
inline mx_status_t sys_cap_revoke(CapHandle cap) { return host_on_cap_revoke ? host_on_cap_revoke(cap) : MX_OK; }
inline mx_u64_t mx_doorbell_read(CapHandle cap) { return sys_doorbell_read(cap); }
inline mx_status_t mx_doorbell_wait_until(CapHandle cap,mx_u64_t expected,mx_u64_t deadline) {
    host_wait_expected = expected;
    host_wait_deadline = deadline;
    if(host_on_wait) return sys_wait_doorbell(cap,expected);
    host_now=deadline; return MX_OK;
}
