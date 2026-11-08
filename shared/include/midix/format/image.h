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
    mx_u64_t relocation_offset = 0;
    MxImageLayout layout{};
    bool aslr = false;
    SegmentPlan segments[kMaxSegments];
    MxArenaCreate arena{MX_PROCESS_ARENA_DEFAULT, MX_PROCESS_ARENA_DEFAULT,
                       MX_PROCESS_ARENA_BASE, MX_PROCESS_ARENA_LIMIT, 1, 0, 0};
};
// Rebase through a nonnegative image offset, so downward slides remain valid
// while malformed addresses and upward overflow are rejected explicitly.
constexpr bool rebase_image_address(mx_u64_t address, mx_u64_t linked, mx_u64_t loaded, mx_u64_t* out) {
    if (!out || address < linked) return false;
    const auto relative = address - linked;
    if (relative > ~mx_u64_t{0} - loaded) return false;
    *out = loaded + relative; return true;
}
// No allocations or mappings occur until the entire image has been validated.
const char* parse_image(const mx_u8_t* data, mx_size_t size, ImagePlan& out);
} // namespace midix::format
