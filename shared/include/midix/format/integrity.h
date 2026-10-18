#pragma once

#include "midix/abi/types.h"

namespace integrity {
using u8 = mx_u8_t;
using u32 = mx_u32_t;
using u64 = mx_u64_t;
using usize = mx_size_t;

constexpr usize kSha256Size = 32;

// Optional cold-work hook, called between bounded chunks. It neither receives
// nor changes integrity state; callers may service boot liveness mechanisms.
struct Progress {
    void (*step)(void*) = nullptr;
    void* context = nullptr;
    void poll() const { if (step) step(context); }
};
u32 crc32c(const u8* data, usize size, Progress progress = {});
// Treat the mathematical zero range as clipped to the input, including when
// zero_offset + zero_size would overflow usize. Input storage is never modified.
u32 crc32c_zero_field(const u8* data, usize size, usize zero_offset, usize zero_size,
                     Progress progress = {});
void sha256(const u8* data, usize size, u8 out[kSha256Size], Progress progress = {});
bool bytes_equal(const u8* a, const u8* b, usize size);

} // namespace integrity
