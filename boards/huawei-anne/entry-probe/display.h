#pragma once
#include <cstdint>

namespace anne::display {
// Captured DTB: hisifb DSS 0xe8600000; reserved boot-logo pool 0x5d000000+48MiB.
// Register definitions: vendor soc_dss_interface.h and hisi_overlay_utils.c.
constexpr std::uint64_t kDss = 0xe8600000;
constexpr std::uint64_t kLogoBase = 0x5d000000;
constexpr std::uint64_t kLogoSize = 0x03000000;
constexpr unsigned kRdmaD3 = 0x53000;
constexpr unsigned kLdi0 = 0x7d000;

struct Registers {
    std::uint32_t channel_control, dma_control, address, stride;
    std::uint32_t x0, y0, mask0, mask1;
    std::uint32_t ldi_control, width, height;
};
struct Buffer {
    std::uint64_t address;
    std::uint32_t width, height, stride, bytes_per_pixel;
};
constexpr Buffer validate(const Registers& r) {
    const auto format = (r.dma_control >> 3) & 31;
    const auto width = (r.width & 0x1fff) + 1;
    const auto height = (r.height & 0x1fff) + 1;
    const auto stride = (r.stride & 0xffff) * 16;
    const unsigned bpp = format == 0 ? 2 : 4;
    // Only enabled, linear physical RGB565 or ARGB/XRGB8888 with the observed
    // portrait geometry. Reject IOVAs, compressed/tiled/cropped/transformed data.
    if (!(r.channel_control & 1) || (r.channel_control & 4) || !(r.ldi_control & 1) ||
        (r.dma_control & ((1u << 1) | (1u << 8) | (7u << 9) | (1u << 12) | (1u << 17))) ||
        (format != 0 && format != 5 && format != 6) || r.stride >> 16 || r.x0 || r.y0 || r.mask0 || r.mask1 ||
        width != 1080 || height != 2280 || stride < width * bpp || stride > 16384 ||
        (r.address & 3) || r.address < kLogoBase) return {};
    const std::uint64_t bytes = std::uint64_t{stride} * (height - 1) + width * bpp;
    const std::uint64_t offset = r.address - kLogoBase;
    if (offset > kLogoSize || bytes > kLogoSize - offset) return {};
    return {r.address, width, height, stride, bpp};
}
}
