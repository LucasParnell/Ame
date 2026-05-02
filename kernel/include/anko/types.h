#pragma once
// anko/types.h — fundamental types used throughout the kernel.
// No kernel headers may be included here; this file has no dependencies.

#include <cstddef>
#include <cstdint>

using u8   = uint8_t;
using u16  = uint16_t;
using u32  = uint32_t;
using u64  = uint64_t;
using i32  = int32_t;
using i64  = int64_t;
using usize = size_t;

using PhysAddr = uint64_t; // physical address
using VirtAddr = uint64_t; // virtual address

// Distinguish physical from virtual at the type level in function signatures.
// Not a strong typedef — just for readability.
