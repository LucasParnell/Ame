#pragma once
#include <cstdint>

namespace anne::minimal {
#ifdef ANNE_MINIMAL_TEST
constexpr std::uint64_t kDss = 0x42000000;
constexpr std::uint64_t kLogoBase = 0x41000000;
#else
constexpr std::uint64_t kDss = 0xe8600000;
constexpr std::uint64_t kLogoBase = 0x5d000000;
#endif
constexpr std::uint64_t kLogoSize = 0x03000000;
constexpr unsigned kLdi = 0x7d000;
// Hi6250 module table: boot firmware uses D3; D2 and V1 are also present.
constexpr unsigned kChannels[] = {0x53000, 0x52000, 0x28000};
struct Registers {
    std::uint32_t channel, control, address, stride, x0, y0, x1, y1;
    std::uint32_t ldi, panel_width, panel_height;
};
struct Buffer {
    std::uint64_t address;
    unsigned width, height, stride, format;
    constexpr unsigned bpp() const { return format < 5 ? 2 : 4; }
};
constexpr Buffer validate(const Registers& r) {
    const unsigned format = (r.control >> 3) & 31;
    const unsigned bpp = format < 5 ? 2 : 4;
    const unsigned panel_width = (r.panel_width & 0x1fff) + 1;
    const unsigned panel_height = (r.panel_height & 0x1fff) + 1;
    // OFT_X is measured in 16-byte groups, not pixels. DATA_ADDR already
    // includes the source rectangle's top/left offset in the vendor driver.
    if (!(r.channel & 1) || (r.channel & 4) || !(r.ldi & 1) ||
        (r.control & ((1u << 1) | (1u << 8) | (7u << 9) | (1u << 12) | (1u << 17))) ||
        format > 6 || r.stride >> 16 || (r.address & 15) ||
        r.x0 > r.x1 || r.y0 > r.y1 || r.x1 > 0xffff || r.y1 > 0xffff ||
        panel_width != 1080 || panel_height != 2280) return {};
    const std::uint64_t row_bytes = (std::uint64_t{r.x1} - r.x0 + 1) * 16;
    const unsigned height = r.y1 - r.y0 + 1;
    const unsigned stride = (r.stride & 0xffff) * 16;
    if (!stride || stride > 16384 || row_bytes > stride ||
        row_bytes > ((panel_width * bpp + 15) & ~15u) || height > panel_height ||
        r.address < kLogoBase) return {};
    const std::uint64_t bytes = std::uint64_t{stride} * (height - 1) + row_bytes;
    const std::uint64_t offset = r.address - kLogoBase;
    if (offset > kLogoSize || bytes > kLogoSize - offset) return {};
    return {r.address, static_cast<unsigned>(row_bytes / bpp), height, stride, format};
}
constexpr std::uint32_t pixel(unsigned format, std::uint32_t rgb) {
    const unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (format == 0) return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    if (format < 3) return 0xf000 | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
    if (format < 5) return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    return 0xff000000 | rgb;
}
constexpr unsigned glyph(char c, unsigned row) {
    switch (c) {
    case 'A': { constexpr unsigned v[]{14,17,17,31,17,17,17}; return v[row]; }
    case 'N': { constexpr unsigned v[]{17,25,25,21,19,19,17}; return v[row]; }
    case 'K': { constexpr unsigned v[]{17,18,20,24,20,18,17}; return v[row]; }
    case 'O': { constexpr unsigned v[]{14,17,17,17,17,17,14}; return v[row]; }
    case 'E': { constexpr unsigned v[]{31,16,16,30,16,16,31}; return v[row]; }
    case 'L': { constexpr unsigned v[]{16,16,16,16,16,16,31}; return v[row]; }
    case '1': { constexpr unsigned v[]{4,12,4,4,4,4,14}; return v[row]; }
    case '2': { constexpr unsigned v[]{14,17,1,2,4,8,31}; return v[row]; }
    default: return 0;
    }
}
inline void paint(const Buffer& fb, unsigned el, bool fault = false) {
    const char* label = el == 1 ? "ANKO EL1" : "ANKO EL2";
    const unsigned scale = fb.width >= 384 && fb.height >= 64 ? 8 : 1;
    for (unsigned y = 0; y < fb.height; ++y) {
        for (unsigned x = 0; x < fb.width; ++x) {
            std::uint32_t rgb = fault ? 0xff0000 : (el == 1 ? 0x00ff00 : 0xff00ff);
            // A white label on a black band remains visible with RGB/BGR wiring.
            if (y < 9 * scale) {
                rgb = 0;
                const unsigned cx = x / scale, cy = y / scale;
                if (cy >= 1 && cy < 8 && cx < 48 && cx % 6 < 5 &&
                    (glyph(label[cx / 6], cy - 1) & (1u << (4 - cx % 6)))) rgb = 0xffffff;
            }
            const auto address = fb.address + std::uint64_t{y} * fb.stride + x * fb.bpp();
            if (fb.bpp() == 2) *reinterpret_cast<volatile std::uint16_t*>(address) = pixel(fb.format, rgb);
            else *reinterpret_cast<volatile std::uint32_t*>(address) = pixel(fb.format, rgb);
        }
    }
}
}
