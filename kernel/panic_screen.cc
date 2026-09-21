// kernel/panic_screen.cc — graphical panic screen.
//
// Layout (any framebuffer size):
//   top-left      QR code carrying the panic report, key data drawn beneath it
//   right / middle the project logo, scaled down to the space that is left
//   bottom-left   "KERNEL PANIC" title and the reason line, left-aligned
//
// The logo is a data asset compiled in from kernel/assets/panic_art.txt, the QR
// spec tables come from kernel/assets/qr_tables.json, and the report text is
// rendered by panic_report::format_qr — nothing here hardcodes content. This
// file has no kernel-only dependencies: tests/panic_screen_probe.cc exercises it
// against a synthetic framebuffer.

#include "anko/panic_screen.h"

#include "anko/qr.h"
#include "midix/font/vga8x16.h"
#include "panic_art.h" // generated from kernel/assets/panic_art.txt

namespace anko::panic_screen {
namespace {

constexpr u32 kCellW = midix::font::kVga8x16Width;
constexpr u32 kCellH = midix::font::kVga8x16Height;
constexpr u32 kMaxScale = 8;
constexpr u32 kQuietModules = 4;   // QR quiet zone, in modules
constexpr u32 kMargin = 8;
constexpr u32 kTitleScale = 2;
constexpr u32 kCaptionScale = 1;
constexpr u32 kMaxCaptionLines = 12;

// XRGB8888 palette.
constexpr u32 kBg = 0x00060E;        // near-black blue
constexpr u32 kArtDim = 0x8FA6C4;
constexpr u32 kArtBright = 0xEAF1FF;
constexpr u32 kHeadFg = 0xFF6B6B;    // panic red
constexpr u32 kReasonFg = 0xFFD9A0;  // warm amber
constexpr u32 kCaptionFg = 0xC8D6E8;
constexpr u32 kQrModule = 0x08111C;  // QR module: dark ink on a light panel
constexpr u32 kQrPanel = 0xF4F7FF;   // QR panel + quiet zone

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

void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    const u32 x1 = smaller(x + w, g_w);
    const u32 y1 = smaller(y + h, g_h);
    for (u32 py = y; py < y1; ++py) {
        for (u32 px = x; px < x1; ++px) g_pixels[static_cast<usize>(py) * g_w + px] = color;
    }
}

void fill_background() {
    const u32 total = g_w * g_h;
    const u64 pair = static_cast<u64>(kBg) | (static_cast<u64>(kBg) << 32);
    volatile u64* wide = reinterpret_cast<volatile u64*>(g_pixels);
    for (u32 i = 0; i < total / 2u; ++i) wide[i] = pair;
    if (total & 1u) g_pixels[total - 1u] = kBg;
}

void draw_glyph(u32 x, u32 y, u32 scale, u32 fg, char ch) {
    const u8* rows = midix::font::vga8x16_glyph(static_cast<u8>(ch));
    for (u32 gy = 0; gy < kCellH; ++gy) {
        const u8 bits = rows[gy];
        if (!bits) continue;
        for (u32 gx = 0; gx < kCellW; ++gx) {
            if ((bits & (0x80u >> gx)) == 0) continue;
            for (u32 sy = 0; sy < scale; ++sy) {
                const u32 py = y + gy * scale + sy;
                for (u32 sx = 0; sx < scale; ++sx) put(x + gx * scale + sx, py, fg);
            }
        }
    }
}

// Draw a NUL-terminated run of printable characters; returns the width used.
u32 draw_text(u32 x, u32 y, u32 scale, u32 fg, const char* s, u32 max_chars) {
    u32 drawn = 0;
    for (; s && *s && drawn < max_chars; ++s, ++drawn) {
        const char ch = (*s >= 32 && *s <= 126) ? *s : '?';
        draw_glyph(x, y, scale, fg, ch);
        x += kCellW * scale;
    }
    return drawn * kCellW * scale;
}

u32 art_color(char ch) {
    switch (ch) {
    case '@': case '%': case '#': return kArtBright;
    default: return kArtDim;
    }
}

// Draw the logo centred in the box on a 1:1 grid (see art_grid).
void draw_art(u32 x0, u32 y0, u32 x1, u32 y1) {
    if (x1 <= x0 || y1 <= y0) return;
    const u32 avail = smaller(x1 - x0, y1 - y0);
    u32 out_cols = 0;
    u32 out_rows = 0;
    art_grid(avail, &out_cols, &out_rows);
    if (out_cols == 0 || out_rows == 0) return;
    const u32 scale = 1;

    const u32 art_w = out_cols * kCellW * scale;
    const u32 art_h = out_rows * kCellH * scale;
    const u32 ox = x0 + ((x1 - x0) > art_w ? ((x1 - x0) - art_w) / 2u : 0u);
    const u32 oy = y0 + ((y1 - y0) > art_h ? ((y1 - y0) - art_h) / 2u : 0u);

    for (u32 dy = 0; dy < out_rows; ++dy) {
        const u32 sy = dy * anko::art::kPanicArtRows / out_rows;
        const char* row = anko::art::kPanicArt[sy];
        for (u32 dx = 0; dx < out_cols; ++dx) {
            const u32 sx = dx * anko::art::kPanicArtCols / out_cols;
            const char ch = row[sx];
            if (ch == ' ') continue;
            draw_glyph(ox + dx * kCellW * scale, oy + dy * kCellH * scale, scale,
                       art_color(ch), ch);
        }
    }
}

// Draw a QR code whose modules are `module` pixels, including the quiet zone.
// Returns the drawn width in pixels (0 when nothing was drawn).
u32 draw_qr(const anko::qr::Code& code, u32 x0, u32 y0, u32 module) {
    if (module == 0) return 0;
    const u32 side = (code.size + 2u * kQuietModules) * module;
    fill_rect(x0, y0, side, side, kQrPanel);
    for (u32 my = 0; my < code.size; ++my) {
        for (u32 mx = 0; mx < code.size; ++mx) {
            if (!anko::qr::dark(code, mx, my)) continue;
            fill_rect(x0 + (mx + kQuietModules) * module, y0 + (my + kQuietModules) * module,
                      module, module, kQrModule);
        }
    }
    return side;
}

// Draw up to `max_lines` newline-separated lines of `text`, left-aligned and
// clipped to `max_chars` characters so a column never runs into its neighbour.
void draw_block(u32 x, u32 y, u32 scale, u32 fg, const char* text, u32 max_lines,
                u32 max_chars) {
    if (!text) return;
    u32 line = 0;
    const char* p = text;
    while (*p && line < max_lines) {
        const char* end = p;
        while (*end && *end != '\n') ++end;
        const u32 width = static_cast<u32>(end - p);
        draw_text(x, y, scale, fg, p, width < max_chars ? width : max_chars);
        ++line;
        y += kCellH * scale;
        if (!*end) break;
        p = end + 1;
    }
}

[[maybe_unused]] anko::qr::Code g_code; // static: never on a faulting stack

} // namespace

void art_grid(u32 avail, u32* out_cols, u32* out_rows) {
    *out_cols = 0;
    *out_rows = 0;
    if (avail == 0) return;
    u32 scale = 1;
    while (scale < kMaxScale &&
           anko::art::kPanicArtCols * (scale + 1u) * kCellW <= avail &&
           anko::art::kPanicArtRows * (scale + 1u) * kCellH <= avail) {
        ++scale;
    }
    u32 cols = smaller(anko::art::kPanicArtCols, avail / (kCellW * scale));
    const u32 row_cap = smaller(anko::art::kPanicArtRows, avail / (kCellH * scale));
    // A glyph cell is 8x16, so two columns and one row cover the same number of
    // pixels: keep the column count even and derive the rows from it, giving an
    // exactly square footprint (no letterboxing, no distortion of the ratio).
    if (cols > row_cap * 2u) cols = row_cap * 2u;
    cols &= ~1u;
    if (cols == 0u) return;
    *out_cols = cols;
    *out_rows = cols / 2u;
}

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

bool show(const char* title, const char* reason, const char* payload) {
    if (!g_pixels) return false;

    const char* head = (title && title[0]) ? title : "KERNEL PANIC";
    const char* why = (reason && reason[0]) ? reason : "unknown";

    fill_background();

    // --- QR block, top-left ------------------------------------------------
    // Try ECC M first, then L: a denser payload must not silently lose the code.
    u32 qr_width = 0;
    u32 qr_height = 0;
    bool have_qr = false;
    // The panel is square by construction (module * modules on both axes); the
    // box only decides how large that square is.
    const u32 box = smaller(g_w / 3u, g_h / 2u);
    if (payload && payload[0]) {
        const u8* bytes = reinterpret_cast<const u8*>(payload);
        const u32 len = slen(payload);
        have_qr = anko::qr::encode(bytes, len, anko::qr::Level::M, g_code) ||
                  anko::qr::encode(bytes, len, anko::qr::Level::L, g_code);
    }
    if (have_qr) {
        const u32 modules = g_code.size + 2u * kQuietModules;
        const u32 module = box / modules;
        if (module >= 1) {
            qr_width = draw_qr(g_code, kMargin, kMargin, module);
            qr_height = qr_width;
        } else {
            have_qr = false;
        }
    }

    // --- key data under the QR, confined to the QR column ------------------
    const u32 caption_x = kMargin;
    u32 caption_y = kMargin + qr_height + (qr_height ? kMargin : 0);
    const u32 band_h = kCellH * kTitleScale + kCellH + 2u * kMargin;
    const u32 caption_limit = g_h > band_h + kCellH ? g_h - band_h - kCellH : caption_y;
    const u32 caption_lines = caption_limit > caption_y ? (caption_limit - caption_y) / kCellH : 0;
    const u32 caption_cols = (qr_width ? qr_width : g_w - 2u * kMargin) / (kCellW * kCaptionScale);
    draw_block(caption_x, caption_y, kCaptionScale, kCaptionFg, payload,
               smaller(caption_lines, kMaxCaptionLines), caption_cols);

    // --- logo, in the space to the right of the QR -------------------------
    const u32 art_x0 = kMargin + qr_width + (qr_width ? kMargin : 0);
    const u32 band_top = g_h > band_h ? g_h - band_h : 0;
    draw_art(art_x0, kMargin, g_w - kMargin, band_top);

    // --- title and reason, bottom-left -------------------------------------
    u32 title_y = band_top + kMargin;
    draw_text(kMargin, title_y, kTitleScale, kHeadFg, head, g_w / (kCellW * kTitleScale));
    title_y += kCellH * kTitleScale;
    draw_text(kMargin, title_y, kCaptionScale, kReasonFg, why, g_w / kCellW);

#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#endif
    return have_qr;
}

} // namespace anko::panic_screen
