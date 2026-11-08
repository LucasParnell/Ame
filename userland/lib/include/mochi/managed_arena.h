#pragma once
#include "mochi/syscall.h"
#include "midix/abi/arena.h"
static inline mx_status_t mx_process_arena(MxArenaCreate* contract, mx_handle_t* out) {
    return mx_status_from_value(_svc4(MX_NR_ARENA_CONTROL, MX_ARENA_PROCESS, 0,
        reinterpret_cast<uint64_t>(contract), sizeof(MxArenaCreate)), out);
}

static inline mx_status_t mx_arena_managed_create(const MxArenaCreate& request, mx_handle_t* out) {
    return mx_status_from_value(_svc4(MX_NR_ARENA_CONTROL, MX_ARENA_NEW, 0,
                                      reinterpret_cast<uint64_t>(&request), sizeof(request)), out);
}
static inline mx_status_t mx_arena_resize(mx_handle_t arena, uint64_t size) {
    return _svc3(MX_NR_ARENA_CONTROL, MX_ARENA_RESIZE, arena, size);
}
// Reserve allocates and zeroes additional backing. Subsequent resize/realloc
// within that capacity needs no allocator or payload copy.
static inline mx_status_t mx_arena_reserve(mx_handle_t arena, uint64_t capacity) {
    return _svc3(MX_NR_ARENA_CONTROL, MX_ARENA_RESERVE, arena, capacity);
}
static inline mx_status_t mx_arena_trim(mx_handle_t arena, uint64_t capacity = 0) {
    return _svc3(MX_NR_ARENA_CONTROL, MX_ARENA_TRIM, arena, capacity);
}
static inline mx_status_t mx_arena_unmap_cap(mx_handle_t arena, uint64_t va) {
    return _svc3(MX_NR_ARENA_CONTROL, MX_ARENA_UNMAP, arena, va);
}
static inline mx_status_t mx_arena_realloc(mx_handle_t arena, uint64_t old_va,
                                          uint64_t new_va, uint64_t size) {
    return _svc5(MX_NR_ARENA_CONTROL, MX_ARENA_REALLOC, arena, old_va, new_va, size);
}
static inline mx_status_t mx_arena_reorder(mx_handle_t arena, const mx_u32_t* order, uint64_t count) {
    return _svc4(MX_NR_ARENA_CONTROL, MX_ARENA_REORDER, arena,
                 reinterpret_cast<uint64_t>(order), count);
}
static inline mx_status_t mx_arena_clone(mx_handle_t arena, mx_handle_t* out) {
    return mx_status_from_value(_svc2(MX_NR_ARENA_CONTROL, MX_ARENA_CLONE, arena), out);
}
static inline mx_status_t mx_arena_precopy(mx_handle_t arena) {
    return _svc2(MX_NR_ARENA_CONTROL, MX_ARENA_PRECOPY, arena);
}
static inline mx_status_t mx_arena_commit(mx_handle_t arena) {
    return _svc2(MX_NR_ARENA_CONTROL, MX_ARENA_COMMIT, arena);
}
static inline mx_status_t mx_arena_cancel(mx_handle_t arena) {
    return _svc2(MX_NR_ARENA_CONTROL, MX_ARENA_CANCEL, arena);
}
static inline mx_status_t mx_arena_inspect(mx_handle_t arena, MxArenaInfo* info) {
    return _svc4(MX_NR_ARENA_CONTROL, MX_ARENA_INSPECT, arena,
                 reinterpret_cast<uint64_t>(info), sizeof(*info));
}
static inline mx_status_t mx_arena_pin(mx_handle_t arena, uint64_t generation) {
    return _svc3(MX_NR_ARENA_CONTROL, MX_ARENA_PIN, arena, generation);
}
static inline mx_status_t mx_arena_unpin(mx_handle_t arena, uint64_t generation) {
    return _svc3(MX_NR_ARENA_CONTROL, MX_ARENA_UNPIN, arena, generation);
}
static inline mx_status_t mx_arena_policy(mx_handle_t arena, mx_u32_t flags) {
    return _svc3(MX_NR_ARENA_CONTROL, MX_ARENA_POLICY, arena, flags);
}
// Unmapping a managed arena includes its reserved tail, so no invisible alias
// keeps backing alive after the caller has relinquished the virtual reservation.
static inline mx_status_t mx_arena_unmap(uint64_t va, uint64_t capacity) {
    if (!capacity || ((va | capacity) & 4095) || va > ~uint64_t{0} - capacity) return MX_ERR_INVALID;
    for (uint64_t offset = 0; offset < capacity; offset += 4096) {
        const auto status = sys_unmap_page(va + offset);
        if (status < 0) return status;
    }
    return MX_OK;
}
