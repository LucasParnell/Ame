#include "../userland/lib/include/mochi/framebuffer_raster.h"
#include <cassert>
#include <vector>
#include <algorithm>

using mochi::framebuffer::Raster;
static constexpr uint32_t width = 93, height = 139, guard = 17;
static std::vector<uint32_t> actual(width * height + 2 * guard), expected;
static Raster raster{actual.data() + guard, width, height};

static void seed() {
    for (unsigned i = 0; i < actual.size(); ++i) actual[i] = 0x12345678 ^ i;
    expected = actual;
}
static void check() { assert(actual == expected); }
static void pixel(uint64_t x, uint64_t y, uint32_t color) {
    if (x < width && y < height) expected[guard + y * width + x] = color | 0xff000000u;
}
// Independent scalar oracle: visit source font pixels and clip each output
// pixel. Deliberately uses wide coordinates for hostile command origins.
static void glyph(uint64_t x, uint64_t y, uint32_t w, uint32_t h,
                   uint32_t sx, uint32_t sy, const uint8_t* rows, uint32_t fg, uint32_t bg) {
    sx = sx ? std::min(sx, 8u) : 1;
    sy = sy ? std::min(sy, 8u) : 1;
    for (unsigned gy = 0; gy < std::min(h, 16u); ++gy)
        for (unsigned gx = 0; gx < std::min(w, 8u); ++gx)
            for (unsigned dy = 0; dy < sy; ++dy)
                for (unsigned dx = 0; dx < sx; ++dx)
                    pixel(x + gx * sx + dx, y + gy * sy + dy,
                          rows[gy] & (0x80u >> gx) ? fg : bg);
}
int main() {
    seed();
    raster.clear(0x112233);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) pixel(x, y, 0x112233);
    check();
    const unsigned positions[] = {0, 1, 17, width - 1, height - 1, height, ~0u};
    const unsigned extents[] = {0, 1, 9, 32, ~0u};
    for (auto x : positions) for (auto y : positions) for (auto w : extents) {
        seed();
        raster.fill_rect(x, y, w, w, 0x654321);
        for (unsigned py = 0; py < height; ++py)
            for (unsigned px = 0; px < width; ++px)
                if (px >= x && py >= y && px - x < w && py - y < w) pixel(px, py, 0x654321);
        check();
    }
    // Copy from a snapshot checks memmove semantics in every overlap direction,
    // including odd widths, unaligned spans and the full-width fast path.
    for (auto sx : positions) for (auto sy : positions)
        for (auto dx : positions) for (auto dy : positions) {
            seed();
            const auto before = actual;
            FbCopyRectCmd cmd{sx, sy, dx, dy, ~0u, ~0u};
            raster.copy_rect(cmd);
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
                const auto src_x = uint64_t{sx} + x, src_y = uint64_t{sy} + y;
                const auto dst_x = uint64_t{dx} + x, dst_y = uint64_t{dy} + y;
                if (src_x < width && src_y < height && dst_x < width && dst_y < height)
                    expected[guard + dst_y * width + dst_x] = before[guard + src_y * width + src_x];
            }
            check();
        }
    for (auto x : positions) for (auto y : positions) for (unsigned scale = 0; scale <= 9; ++scale) {
        seed();
        FbDrawGlyphCmd cmd{};
        cmd.x = x; cmd.y = y; cmd.fg = 0xabcdef; cmd.bg = 0x987654;
        cmd.width = 255; cmd.height = 255; cmd.scale_x = scale; cmd.scale_y = 9 - scale;
        for (unsigned i = 0; i < 16; ++i) cmd.rows[i] = i * 17;
        raster.draw_glyph(cmd);
        glyph(x, y, cmd.width, cmd.height, cmd.scale_x, cmd.scale_y, cmd.rows, cmd.fg, cmd.bg);
        check();
        seed();
        FbDrawTextCmd text{};
        text.x = x; text.y = y; text.fg = cmd.fg; text.bg = cmd.bg;
        text.len = 255; text.scale_x = scale; text.scale_y = 9 - scale;
        for (unsigned i = 0; i < kFbDrawTextMaxChars; ++i) text.text[i] = static_cast<char>(i * 11);
        raster.draw_text(text);
        for (unsigned i = 0; i < kFbDrawTextMaxChars; ++i)
            glyph(uint64_t{x} + i * 8 * (scale ? std::min(scale, 8u) : 1), y, 8, 16,
                  text.scale_x, text.scale_y,
                  midix::font::vga8x16_glyph(static_cast<uint8_t>(text.text[i])), text.fg, text.bg);
        check();
    }
}
