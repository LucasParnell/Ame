#pragma once

#include "midix/abi/types.h"

namespace integrity {
using u8 = mx_u8_t;
using u32 = mx_u32_t;
using u64 = mx_u64_t;
using usize = mx_size_t;

constexpr usize kSha256Size = 32;

u32 crc32c(const u8* data, usize size);
u32 crc32c_zero_field(const u8* data, usize size, usize zero_offset, usize zero_size);
void sha256(const u8* data, usize size, u8 out[kSha256Size]);
bool bytes_equal(const u8* a, const u8* b, usize size);

} // namespace integrity
