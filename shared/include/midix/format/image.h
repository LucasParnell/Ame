#pragma once
#include "midix/abi/image.h"
namespace midix::format {
constexpr mx_u32_t kMaxSegments = 32;
struct SegmentPlan {
    mx_u64_t va_start, page_off, map_size, file_offset, file_size;
    mx_u32_t rights;
};
struct ImagePlan {
    mx_u64_t entry;
    mx_u32_t count;
    SegmentPlan segments[kMaxSegments];
};
// No allocations or mappings occur until the entire image has been validated.
const char* parse_image(const mx_u8_t* data, mx_size_t size, ImagePlan& out);
} // namespace midix::format
