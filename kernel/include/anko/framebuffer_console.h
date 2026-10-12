#pragma once
#include "anko/types.h"
#include "libk/framebuffer.h"

namespace console {
struct Viewport { u32 x, y, width, height, scale; };

// A bounded kernel text surface. The caller serializes writes and publishes
// dirty rows for DMA; this class only accesses validated normal pixel memory.
class FramebufferConsole {
public:
    static constexpr u32 kMaxColumns = 160, kMaxRows = 128;
    bool bind(libk::Framebuffer32 fb, u64 owner_base, u64 owner_bytes, Viewport view);
    void putchar(char c);
    void write(const char* text, usize size);
    void draw_text(u32 x, u32 y, u32 scale, const char* text, u32 foreground);
    bool take_dirty(u64& address, u32& bytes);
    bool active() const { return fb_.address != 0; }
    u32 columns() const { return columns_; }
    u32 rows() const { return rows_; }
    char cell(u32 row, u32 col) const { return row < rows_ && col < columns_ ? cells_[row][col] : '\0'; }
    u64 characters() const { return characters_; }
    static constexpr u32 kBackground = 0xff07111f, kForeground = 0xffe6f1ff;
private:
    libk::Framebuffer32 fb_{};
    Viewport view_{};
    u32 columns_ = 0, rows_ = 0, x_ = 0, y_ = 0;
    u32 dirty_y0_ = ~0u, dirty_y1_ = 0;
    u64 characters_ = 0;
    char cells_[kMaxRows][kMaxColumns]{};
    void glyph(u32 x, u32 y, u32 scale, char c, u32 foreground);
    void dirty(u32 y, u32 height);
    void newline();
    void clear_row(u32 row);
};
}
