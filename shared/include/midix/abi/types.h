#pragma once
// Midix stable userspace ABI scalar types.
// This header is intentionally C-shaped: no allocation, no exceptions, no STL.

#include <cstdint>
#include <cstddef>

using mx_u8_t = uint8_t;
using mx_u16_t = uint16_t;
using mx_u32_t = uint32_t;
using mx_u64_t = uint64_t;
using mx_i32_t = int32_t;
using mx_i64_t = int64_t;
using mx_size_t = size_t;
using mx_handle_t = uint64_t;
using mx_status_t = int64_t;
using mx_vaddr_t = uint64_t;
using mx_paddr_t = uint64_t;
using mx_rights_t = uint32_t;

constexpr mx_handle_t MX_NULL_HANDLE = 0;

constexpr mx_u32_t mx_handle_index(mx_handle_t h) {
    return static_cast<mx_u32_t>(h & 0xFFFF'FFFFull);
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
