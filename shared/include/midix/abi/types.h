#pragma once
// Midix stable userspace ABI scalar types.
// This header is intentionally C-shaped: no allocation, no exceptions, no STL.

#if defined(__cplusplus)
#include <cstdint>
#include <cstddef>
#else
#include <stdint.h>
#include <stddef.h>
#endif

typedef uint8_t mx_u8_t;
typedef uint16_t mx_u16_t;
typedef uint32_t mx_u32_t;
typedef uint64_t mx_u64_t;
typedef int32_t mx_i32_t;
typedef int64_t mx_i64_t;
typedef size_t mx_size_t;
typedef uint64_t mx_handle_t;
typedef int64_t mx_status_t;
typedef uint64_t mx_vaddr_t;
typedef uint64_t mx_paddr_t;
typedef uint32_t mx_rights_t;

#define MX_NULL_HANDLE 0

#if defined(__cplusplus)

constexpr mx_u32_t mx_handle_index(mx_handle_t h) {
    return static_cast<mx_u32_t>(h & 0xFFFFFFFFull);
}

constexpr mx_u32_t mx_handle_generation(mx_handle_t h) {
    return static_cast<mx_u32_t>(h >> 32);
}

constexpr mx_handle_t mx_handle_make(mx_u32_t index, mx_u32_t generation) {
    return (static_cast<mx_handle_t>(generation) << 32) | index;
}

// Useful for current V0 boot layouts. New services should prefer BootInfo names,
// but this keeps fixed-slot bootstrap code readable and centralized.
constexpr mx_handle_t mx_cap_slot(mx_u32_t idx) {
    return mx_handle_make(idx, 1u);
}
#endif // __cplusplus helpers
