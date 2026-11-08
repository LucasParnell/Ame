#pragma once
#include "ame/ui.h"
#include "midix/font/vga8x16.h"

namespace ame::ui {
// Host reference and counted CPU fallback. Pixels use straight ARGB8 input
// colors and opaque RGB output. This blends encoded channels; it does not claim
// linear-light or full Unicode text support. GPU rendering is a separate sink.
class RasterSink {
    uint32_t* pixels_;
    int32_t width_, height_;
    uint32_t stride_;
    bool valid_;
    void pixel(int32_t x, int32_t y, uint32_t color) {
        auto& target = pixels_[uint64_t(y) * stride_ + x];
        const auto alpha = color >> 24;
        uint32_t output = 0xff000000;
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const auto a = (color >> shift) & 255, b = (target >> shift) & 255;
            output |= ((a * alpha + b * (255 - alpha) + 127) / 255) << shift;
        }
        target = output;
    }
  public:
    RasterSink(uint32_t* pixels, uint64_t capacity_pixels, int32_t width, int32_t height,
               uint32_t stride) : pixels_(pixels), width_(width), height_(height), stride_(stride),
        valid_(pixels && width > 0 && width <= kMaxExtent && height > 0 && height <= kMaxExtent &&
               stride >= uint32_t(width) && uint64_t(height - 1) * stride + width <= capacity_pixels) {}
    bool clear(uint32_t color = 0xff000000) {
        if (!valid_) return false;
        for (int32_t y = 0; y < height_; ++y)
            for (int32_t x = 0; x < width_; ++x)
                pixels_[uint64_t(y) * stride_ + x] = color | 0xff000000;
        return true;
    }
    bool rectangle(Rect box, Rect clip, uint32_t color) {
        if (!valid_) return false;
        const auto area = intersect(intersect(box, clip), {0, 0, width_, height_});
        for (int32_t y = area.y; y < add(area.y, area.height); ++y)
            for (int32_t x = area.x; x < add(area.x, area.width); ++x) pixel(x, y, color);
        return true;
    }
    bool text(Rect box, Rect clip, const Text& label, uint32_t color) {
        if (!valid_) return false;
        clip = intersect(intersect(box, clip), {0, 0, width_, height_});
        for (uint32_t i = 0; i < label.size; ++i) {
            const auto ch = uint8_t(label.bytes[i]);
            const auto* glyph = midix::font::vga8x16_glyph(ch < 128 ? ch : '?');
            const auto gx = add(box.x, i * 8);
            for (int32_t y = 0; y < 16; ++y)
                for (int32_t x = 0; x < 8; ++x)
                    if ((glyph[y] & (0x80 >> x)) && clip.contains(add(gx, x), add(box.y, y)))
                        pixel(add(gx, x), add(box.y, y), color);
        }
        return true;
    }
};
} // namespace ame::ui
