#include "anko/framebuffer_console.h"
#include "midix/font/vga8x16.h"
#include "libk/bytes.h"

namespace console {
bool FramebufferConsole::bind(libk::Framebuffer32 fb, u64 owner_base, u64 owner_bytes,
                              Viewport view, Overflow overflow) {
    fb_ = {};
    columns_ = rows_ = x_ = y_ = 0;
    if (!libk::framebuffer_fits(fb, owner_base, owner_bytes) || !view.scale || view.scale > 4 ||
        view.x > fb.width || view.y > fb.height || view.width > fb.width - view.x ||
        view.height > fb.height - view.y) return false;
    const u32 columns = view.width / (8 * view.scale), rows = view.height / (16 * view.scale);
    if (!columns || !rows || columns > kMaxColumns || rows > kMaxRows) return false;
    fb_ = fb; view_ = view; columns_ = columns; rows_ = rows; overflow_ = overflow;
    x_ = y_ = 0; characters_ = 0; dirty_y0_ = ~0u; dirty_y1_ = 0;
    for (u32 row = 0; row < rows_; ++row) clear_row(row);
    return true;
}
void FramebufferConsole::dirty(u32 y, u32 height) {
    if (y < dirty_y0_) dirty_y0_ = y;
    if (y + height > dirty_y1_) dirty_y1_ = y + height;
}
void FramebufferConsole::glyph(u32 x, u32 y, u32 scale, char c, u32 foreground) {
    if (!active() || !scale || scale > 4 || x > fb_.width || y > fb_.height ||
        8 * scale > fb_.width - x || 16 * scale > fb_.height - y) return;
    const auto* bits = midix::font::vga8x16_glyph(static_cast<u8>(c));
    auto* pixels = reinterpret_cast<u32*>(fb_.address);
    for (u32 gy = 0; gy < 16; ++gy) {
        for (u32 sy = 0; sy < scale; ++sy) {
            const u64 row = u64{y + gy * scale + sy} * fb_.width;
            libk::framebuffer_mono_span(pixels + row + x, bits[gy], 8 * scale, scale,
                                        foreground, kBackground);
        }
    }
    dirty(y, 16 * scale);
}
void FramebufferConsole::clear_row(u32 row) {
    for (u32 col = 0; col < columns_; ++col) cells_[row][col] = ' ';
    auto* pixels = reinterpret_cast<u32*>(fb_.address);
    const u32 start_y = view_.y + row * 16 * view_.scale;
    for (u32 py = start_y; py < start_y + 16 * view_.scale; ++py)
        libk::framebuffer_fill_span(pixels + u64{py} * fb_.width + view_.x,
                                    columns_ * 8 * view_.scale, kBackground);
    dirty(start_y, 16 * view_.scale);
}
void FramebufferConsole::newline() {
    x_ = 0;
    if (overflow_ == Overflow::Wrap) {
        // Page flip, not a ring: when the last row is used, clear the whole text
        // area and start again at the top. Overwriting rows one at a time left
        // the screen holding new lines above stale ones, which reads as a freeze.
        // The full clear costs one viewport fill per page, not per line.
        y_ = (y_ + 1) % rows_;
        if (y_ == 0) {
            for (u32 row = 0; row < rows_; ++row) clear_row(row);
            dirty(view_.y, rows_ * 16 * view_.scale);
        } else {
            clear_row(y_);
        }
        return;
    }
    if (++y_ < rows_) return;
    y_ = rows_ - 1;
    for (u32 row = 0; row + 1 < rows_; ++row)
        for (u32 col = 0; col < columns_; ++col) cells_[row][col] = cells_[row + 1][col];
    // Only scroll the text viewport: preserve the banner and outside margins.
    auto* pixels = reinterpret_cast<u32*>(fb_.address);
    const usize width_bytes = usize{columns_} * 8 * view_.scale * 4;
    const u32 cell_height = 16 * view_.scale;
    for (u32 py = view_.y; py < view_.y + (rows_ - 1) * cell_height; ++py) {
        u32* dst = pixels + u64{py} * fb_.width + view_.x;
        const u32* src = dst + u64{cell_height} * fb_.width;
        (void)libk::move_n(libk::as_bytes(dst, width_bytes), libk::as_bytes(src, width_bytes), width_bytes);
    }
    clear_row(y_);
    dirty(view_.y, rows_ * cell_height);
}
void FramebufferConsole::putchar(char c) {
    if (!active()) return;
    ++characters_;
    if (c == '\n') { newline(); return; }
    if (c == '\r') { x_ = 0; return; }
    if (c == '\b') {
        if (x_) { --x_; cells_[y_][x_] = ' '; glyph(view_.x + x_ * 8 * view_.scale, view_.y + y_ * 16 * view_.scale, view_.scale, ' ', kForeground); }
        return;
    }
    if (c == '\t') {
        const u32 spaces = 4 - (x_ % 4);
        for (u32 i = 0; i < spaces; ++i) putchar(' ');
        return;
    }
    if (static_cast<u8>(c) < 32 || static_cast<u8>(c) > 126) return;
    if (x_ == columns_) newline();
    cells_[y_][x_] = c;
    glyph(view_.x + x_ * 8 * view_.scale, view_.y + y_ * 16 * view_.scale, view_.scale, c, kForeground);
    ++x_;
}
void FramebufferConsole::write(const char* text, usize size) {
    if (text) for (usize i = 0; i < size; ++i) putchar(text[i]);
}
void FramebufferConsole::draw_text(u32 x, u32 y, u32 scale, const char* text, u32 foreground) {
    if (!active() || !text || !scale || scale > 4) return;
    while (*text && x <= fb_.width && 8 * scale <= fb_.width - x) {
        glyph(x, y, scale, *text++, foreground);
        x += 8 * scale;
    }
}
bool FramebufferConsole::take_dirty(u64& address, u32& bytes) {
    if (!active() || dirty_y0_ >= dirty_y1_) return false;
    const u64 length = u64{dirty_y1_ - dirty_y0_} * fb_.width * 4;
    if (length > ~0u) return false;
    address = fb_.address + u64{dirty_y0_} * fb_.width * 4;
    bytes = static_cast<u32>(length);
    dirty_y0_ = ~0u; dirty_y1_ = 0;
    return true;
}
}
