#pragma once
// Single-owner raster operations on validated, tightly packed Normal RAM/NC.
// Commands clip once; hot loops walk contiguous pixels. Publishing to scanout
// belongs to the service, separately from CPU rendering.
#include "libk/bytes.h"
#include "libk/framebuffer.h"
#include "midix/abi/protocols/framebuffer.h"
#include "midix/font/vga8x16.h"

namespace mochi::framebuffer {
struct Raster {
    uint32_t* pixels;
    uint32_t width, height;

    static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }
    static uint32_t scale(uint32_t value) { return value ? min(value, 8) : 1; }
    uint32_t* row(uint32_t x, uint32_t y) const {
        return pixels + uint64_t{y} * width + x;
    }
    void clear(uint32_t color) const {
        libk::framebuffer_fill_span(pixels, uint64_t{width} * height, color | 0xff000000u);
    }
    void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) const {
        if (x >= width || y >= height) return;
        w = min(w, width - x);
        h = min(h, height - y);
        color |= 0xff000000u;
        for (uint32_t py = 0; py < h; ++py)
            libk::framebuffer_fill_span(row(x, y + py), w, color);
    }
    void copy_rect(const FbCopyRectCmd& cmd) const {
        if (cmd.src_x >= width || cmd.dst_x >= width ||
            cmd.src_y >= height || cmd.dst_y >= height) return;
        const auto w = min(cmd.width, min(width - cmd.src_x, width - cmd.dst_x));
        const auto h = min(cmd.height, min(height - cmd.src_y, height - cmd.dst_y));
        if (!w || !h) return;
        const auto bytes = static_cast<std::size_t>(w) * sizeof(*pixels);
        auto* dst = row(cmd.dst_x, cmd.dst_y);
        const auto* src = row(cmd.src_x, cmd.src_y);
        // Full-width scrolling is one contiguous overlap-safe move.
        if (w == width) {
            const auto total = bytes * h;
            (void)libk::move_n(libk::as_bytes(dst, total), libk::as_bytes(src, total), total);
            return;
        }
        // Vertical overlap determines row order; move_n handles horizontal overlap.
        for (uint32_t i = 0; i < h; ++i) {
            const auto py = cmd.dst_y <= cmd.src_y ? i : h - 1 - i;
            (void)libk::move_n(libk::as_bytes(row(cmd.dst_x, cmd.dst_y + py), bytes),
                               libk::as_bytes(row(cmd.src_x, cmd.src_y + py), bytes), bytes);
        }
    }
    void draw_glyph(const FbDrawGlyphCmd& cmd) const {
        if (cmd.x >= width || cmd.y >= height) return;
        const auto sx = scale(cmd.scale_x), sy = scale(cmd.scale_y);
        const auto w = min(min(cmd.width, 8) * sx, width - cmd.x);
        const auto h = min(min(cmd.height, 16) * sy, height - cmd.y);
        const auto fg = cmd.fg | 0xff000000u, bg = cmd.bg | 0xff000000u;
        for (uint32_t py = 0; py < h; ++py)
            libk::framebuffer_mono_span(row(cmd.x, cmd.y + py), cmd.rows[py / sy], w, sx,
                                        fg, bg);
    }
    void draw_text(const FbDrawTextCmd& cmd) const {
        if (cmd.x >= width || cmd.y >= height) return;
        const auto sx = scale(cmd.scale_x), sy = scale(cmd.scale_y);
        const auto cell_w = 8 * sx;
        const auto available = width - cmd.x;
        const auto count = min(min(cmd.len, kFbDrawTextMaxChars),
                               available / cell_w + (available % cell_w != 0));
        const auto h = min(16 * sy, height - cmd.y);
        const auto fg = cmd.fg | 0xff000000u, bg = cmd.bg | 0xff000000u;
        // Resolve font addresses once per run, then complete each destination
        // scanline before advancing. No per-character command construction or
        // repeated jumps across 16 framebuffer rows.
        const uint8_t* glyphs[kFbDrawTextMaxChars];
        for (uint32_t i = 0; i < count; ++i)
            glyphs[i] = midix::font::vga8x16_glyph(static_cast<uint8_t>(cmd.text[i]));
        for (uint32_t py = 0; py < h; ++py) {
            auto* dst = row(cmd.x, cmd.y + py);
            for (uint32_t i = 0; i < count; ++i) {
                const auto w = min(cell_w, available - i * cell_w);
                libk::framebuffer_mono_span(dst + i * cell_w, glyphs[i][py / sy], w, sx,
                                            fg, bg);
            }
        }
    }
};
} // namespace mochi::framebuffer
