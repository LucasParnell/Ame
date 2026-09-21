// kernel/panic_screen.cc — graphical panic screen.
//
// Paints the project logo and panic information onto the framebuffer the kernel
// owns before init runs. The logo is an ASCII art asset compiled in from
// kernel/assets/panic_art.txt (generated header); nothing about the image is
// hardcoded here. The art is rendered through the shared 8x16 console font,
// scaled to fit the screen and re-sampled when even scale 1 would overflow, so
// the screen adapts to any framebuffer size (QEMU 1024x768, T210 1280x720, ...).
//
// This file has no kernel-only dependencies: it is exercised by a host test
// (tests/test_panic_screen.py) against a synthetic framebuffer as well as by the
// real panic path.

#include "anko/panic_screen.h"

#include "midix/font/vga8x16.h"
#include "panic_art.h" // generated from kernel/assets/panic_art.txt

namespace anko::panic_screen {
namespace {

constexpr u32 kCellW = midix::font::kVga8x16Width;
constexpr u32 kCellH = midix::font::kVga8x16Height;
constexpr u32 kMaxScale = 8;

// XRGB8888 palette.
constexpr u32 kBg = 0x00060E;       // near-black blue
constexpr u32 kArtDim = 0x8FA6C4;   // light glyphs
constexpr u32 kArtBright = 0xEAF1FF;// dense glyphs
constexpr u32 kHeadFg = 0xFF6B6B;   // panic red
constexpr u32 kDetailFg = 0xFFE6B8; // warm white

volatile u32* g_pixels = nullptr;
u32 g_w = 0;
u32 g_h = 0;

u32 smaller(u32 a, u32 b) { return a < b ? a : b; }

u32 slen(const char* s) {
    u32 n = 0;
    while (s && s[n]) ++n;
    return n;
}

void put(u32 x, u32 y, u32 color) {
    if (x >= g_w || y >= g_h) return;
    g_pixels[static_cast<usize>(y) * g_w + x] = color;
}

void fill_background() {
    const u32 total = g_w * g_h;
    const u64 pair = static_cast<u64>(kBg) | (static_cast<u64>(kBg) << 32);
    volatile u64* wide = reinterpret_cast<volatile u64*>(g_pixels);
    const u32 pairs = total / 2u;
    for (u32 i = 0; i < pairs; ++i) wide[i] = pair;
    if (total & 1u) g_pixels[total - 1u] = kBg;
}

// Dense glyphs of the art are drawn brighter than the sparse ones so the logo
// keeps some of the tonal ramp it was authored with.
u32 art_color(char ch) {
    switch (ch) {
    case '@': case '%': case '#': return kArtBright;
    default: return kArtDim;
    }
}

// Draw one glyph with integer `scale`; only lit pixels are written (the
// background was already filled).
void draw_glyph(u32 x, u32 y, u32 scale, u32 fg, char ch) {
    const u8* rows = midix::font::vga8x16_glyph(static_cast<u8>(ch));
    for (u32 gy = 0; gy < kCellH; ++gy) {
        const u8 bits = rows[gy];
        if (!bits) continue;
        for (u32 gx = 0; gx < kCellW; ++gx) {
            if ((bits & (0x80u >> gx)) == 0) continue;
            for (u32 sy = 0; sy < scale; ++sy) {
                const u32 py = y + gy * scale + sy;
                for (u32 sx = 0; sx < scale; ++sx) {
                    put(x + gx * scale + sx, py, fg);
                }
            }
        }
    }
}

// Draw a NUL-terminated string; returns the width consumed.
u32 draw_text(u32 x, u32 y, u32 scale, u32 fg, const char* s) {
    while (s && *s) {
        draw_glyph(x, y, scale, fg, *s);
        x += kCellW * scale;
        ++s;
    }
    return x;
}

u32 text_width(u32 scale, const char* s) { return slen(s) * kCellW * scale; }

void draw_centered(u32 y, u32 scale, u32 fg, const char* s) {
    const u32 width = text_width(scale, s);
    const u32 x = width < g_w ? (g_w - width) / 2u : 0u;
    (void)draw_text(x, y, scale, fg, s);
}

// Render the logo into [top, bottom) pixels, centred, scaled to fit.
void draw_art(u32 top, u32 bottom) {
    if (bottom <= top) return;
    const u32 avail_w = g_w;
    const u32 avail_h = bottom - top;
    const u32 avail_cols = avail_w / kCellW;
    const u32 avail_rows = avail_h / kCellH;
    if (avail_cols == 0 || avail_rows == 0) return;

    // Largest integer font scale that still fits the whole art, then fall back
    // to re-sampling the art grid when even scale 1 overflows.
    u32 scale = 1;
    while (scale < kMaxScale && anko::art::kPanicArtCols * (scale + 1u) <= avail_cols &&
           anko::art::kPanicArtRows * (scale + 1u) <= avail_rows) {
        ++scale;
    }
    const u32 out_cols = smaller(anko::art::kPanicArtCols, avail_cols / scale);
    const u32 out_rows = smaller(anko::art::kPanicArtRows, avail_rows / scale);
    if (out_cols == 0 || out_rows == 0) return;

    const u32 art_w = out_cols * kCellW * scale;
    const u32 art_h = out_rows * kCellH * scale;
    const u32 x0 = art_w < g_w ? (g_w - art_w) / 2u : 0u;
    const u32 y0 = top + (avail_h > art_h ? (avail_h - art_h) / 2u : 0u);

    for (u32 dy = 0; dy < out_rows; ++dy) {
        const u32 sy = dy * anko::art::kPanicArtRows / out_rows;
        const char* row = anko::art::kPanicArt[sy];
        for (u32 dx = 0; dx < out_cols; ++dx) {
            const u32 sx = dx * anko::art::kPanicArtCols / out_cols;
            const char ch = row[sx];
            if (ch == ' ') continue;
            draw_glyph(x0 + dx * kCellW * scale, y0 + dy * kCellH * scale, scale,
                       art_color(ch), ch);
        }
    }
}

} // namespace

void bind(volatile u32* pixels, u32 width, u32 height) {
    // Reject a framebuffer too small to show even one glyph row; an invalid bind
    // leaves nothing bound rather than a half-usable screen.
    if (!pixels || width < kCellW * 8u || height < kCellH * 8u) {
        g_pixels = nullptr;
        g_w = 0;
        g_h = 0;
        return;
    }
    g_pixels = pixels;
    g_w = width;
    g_h = height;
}

bool bound() { return g_pixels != nullptr; }

void show(const char* headline, const char* detail) {
    if (!g_pixels) return;

    const char* head = (headline && headline[0]) ? headline : "KERNEL PANIC";
    const u32 head_scale = 2;
    const u32 head_h = kCellH * head_scale;
    const u32 detail_h = kCellH;

    fill_background();

    // Headline band, reserving symmetric margins for the art.
    const u32 margin = 8;
    draw_centered(margin, head_scale, kHeadFg, head);

    const u32 art_top = margin + head_h + margin;
    const u32 art_bottom = g_h > (detail_h + 2u * margin) ? g_h - (detail_h + 2u * margin) : art_top;
    draw_art(art_top, art_bottom);

    if (detail && detail[0]) {
        draw_centered(g_h - detail_h - margin, 1, kDetailFg, detail);
    }

#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#endif
}

} // namespace anko::panic_screen
