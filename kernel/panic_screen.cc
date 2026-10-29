// kernel/panic_screen.cc — graphical panic screen.
//
// Portrait layout (P20 Lite, 1080x2280):
//   top           "KERNEL PANIC" and the reason, large
//   below that    QR code carrying the panic report
//   under the QR  the readable report, wrapped to the panel width
//   bottom third  the project logo, scaled to the full panel width
//
// Landscape layout (QEMU ramfb, T210):
//   top-left      QR code, report text beneath it
//   right         logo, scaled down to the space that is left
//   bottom-left   title and reason
//
// The logo is a data asset compiled in from kernel/assets/panic_art.txt, the QR
// spec tables come from kernel/assets/qr_tables.json, and the report text is
// rendered by panic_report::format_qr — nothing here hardcodes content. This
// file has no kernel-only dependencies: tests/panic_screen_probe.cc exercises it
// against a synthetic framebuffer.
//
// Painting order is intentional: background, header, QR, report, logo. A reset
// that lands mid-render (an inherited watchdog, a failing panel) leaves the most
// useful information already on the glass.

#include "anko/panic_screen.h"

#include "anko/qr.h"
#include "midix/font/vga8x16.h"
#include "panic_art.h" // generated from kernel/assets/panic_art.txt

namespace anko::panic_screen {
namespace {

constexpr u32 kCellW = midix::font::kVga8x16Width;
constexpr u32 kCellH = midix::font::kVga8x16Height;
constexpr u32 kQuietModules = 4; // QR quiet zone, in modules
// Report block: 6x12 glyphs (bigger than the 4x8 first cut, still compact).
constexpr u32 kCapCellW = 6;
constexpr u32 kCapCellH = 12;
constexpr u32 kCaptionGap = 2; // leading between caption lines
constexpr u32 kMargin = 8;
constexpr u32 kTitleScale = 2;
constexpr u32 kCaptionScale = 1;
constexpr u32 kMaxCaptionLines = 64;

// XRGB8888 palette.
constexpr u32 kBg = 0x00060E; // near-black blue
constexpr u32 kArtDim = 0x8FA6C4;
constexpr u32 kArtMid = 0xB9CCE4;
constexpr u32 kArtBright = 0xEAF1FF;
constexpr u32 kHeadFg = 0xFF6B6B;   // panic red
constexpr u32 kReasonFg = 0xFFD9A0; // warm amber
constexpr u32 kCaptionFg = 0xC8D6E8;
constexpr u32 kQrModule = 0x08111C; // QR module: dark ink on a light panel
constexpr u32 kQrPanel = 0xF4F7FF;  // QR panel + quiet zone

volatile u32* g_pixels = nullptr;
u64 g_base = 0;
u32 g_w = 0;
u32 g_h = 0;
u32 g_stride = 0;       // pixels per row, >= g_w
bool g_wide_ok = false; // 8-byte stores are safe for this surface
void (*g_flush)(u64, u32) = nullptr;
void (*g_progress)() = nullptr;
bool g_painted = false;

u32 smaller(u32 a, u32 b) {
    return a < b ? a : b;
}
u32 clampu(u32 v, u32 lo, u32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

u32 slen(const char* s) {
    u32 n = 0;
    while (s && s[n])
        ++n;
    return n;
}

void progress() {
    if (g_progress)
        g_progress();
}

void put(u32 x, u32 y, u32 color) {
    if (x >= g_w || y >= g_h)
        return;
    g_pixels[static_cast<usize>(y) * g_stride + x] = color;
}

// Defined below; draw_art_at needs it and the report block uses the same routine.
void draw_glyph_sized(u32 x, u32 y, u32 cw, u32 ch, u32 fg, char glyph);

void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    const u32 x1 = smaller(x + w, g_w);
    const u32 y1 = smaller(y + h, g_h);
    for (u32 py = y; py < y1; ++py) {
        for (u32 px = x; px < x1; ++px) {
            g_pixels[static_cast<usize>(py) * g_stride + px] = color;
        }
    }
}

void fill_background() {
    const u64 pair = static_cast<u64>(kBg) | (static_cast<u64>(kBg) << 32);
    for (u32 y = 0; y < g_h; ++y) {
        volatile u32* row = g_pixels + static_cast<usize>(y) * g_stride;
        if (g_wide_ok) {
            volatile u64* wide = reinterpret_cast<volatile u64*>(const_cast<u32*>(row));
            for (u32 i = 0; i < g_w / 2u; ++i)
                wide[i] = pair;
            if (g_w & 1u)
                row[g_w - 1u] = kBg;
        } else {
            for (u32 i = 0; i < g_w; ++i)
                row[i] = kBg;
        }
        if ((y & 63u) == 0u)
            progress();
    }
}

void draw_glyph(u32 x, u32 y, u32 scale, u32 fg, char ch) {
    const u8* rows = midix::font::vga8x16_glyph(static_cast<u8>(ch));
    for (u32 gy = 0; gy < kCellH; ++gy) {
        const u8 bits = rows[gy];
        if (!bits)
            continue;
        for (u32 gx = 0; gx < kCellW; ++gx) {
            if ((bits & (0x80u >> gx)) == 0)
                continue;
            for (u32 sy = 0; sy < scale; ++sy) {
                const u32 py = y + gy * scale + sy;
                for (u32 sx = 0; sx < scale; ++sx)
                    put(x + gx * scale + sx, py, fg);
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

// Three tones by ink density. The source art is a shading ramp (. : - = + * # % @),
// and collapsing it to two colours flattened the picture into a blob; the middle
// tone is what makes the form readable on a 1080-wide panel.
u32 art_color(char ch) {
    switch (ch) {
    case '@':
    case '%':
    case '#':
        return kArtBright;
    case '=':
    case '+':
    case '*':
        return kArtMid;
    default:
        return kArtDim;
    }
}

// Draw a glyph scaled into a cw x ch cell by box-filtering the 8x16 source (a
// lit pixel anywhere in the source block lights the destination pixel), so
// strokes survive down to a few pixels per glyph.
void draw_glyph_sized(u32 x, u32 y, u32 cw, u32 ch, u32 fg, char glyph) {
    const u8* rows = midix::font::vga8x16_glyph(static_cast<u8>(glyph));
    for (u32 dy = 0; dy < ch; ++dy) {
        u32 sy0 = dy * kCellH / ch;
        u32 sy1 = (dy + 1u) * kCellH / ch;
        if (sy1 <= sy0)
            sy1 = sy0 + 1u;
        if (sy1 > kCellH)
            sy1 = kCellH;
        for (u32 dx = 0; dx < cw; ++dx) {
            u32 sx0 = dx * kCellW / cw;
            u32 sx1 = (dx + 1u) * kCellW / cw;
            if (sx1 <= sx0)
                sx1 = sx0 + 1u;
            if (sx1 > kCellW)
                sx1 = kCellW;
            bool on = false;
            for (u32 sy = sy0; sy < sy1 && !on; ++sy) {
                for (u32 sx = sx0; sx < sx1; ++sx) {
                    if (((rows[sy] >> (7u - sx)) & 1u) != 0u) {
                        on = true;
                        break;
                    }
                }
            }
            if (on)
                put(x + dx, y + dy, fg);
        }
    }
}

// Draw the logo at `cell` size with its top-left at (ox, oy).
void draw_art_at(u32 ox, u32 oy, u32 cw, u32 ch) {
    if (cw == 0u || ch == 0u)
        return;
    u32 row = 0;
    for (u32 ry = 0; ry < anko::art::kPanicArtRows; ++ry) {
        const char* src = anko::art::kPanicArt[ry];
        for (u32 rx = 0; rx < anko::art::kPanicArtCols; ++rx) {
            const char glyph = src[rx];
            if (glyph == ' ')
                continue;
            draw_glyph_sized(ox + rx * cw, oy + ry * ch, cw, ch, art_color(glyph), glyph);
        }
        if ((++row & 7u) == 0u)
            progress();
    }
}

// Draw the logo centred in the box, at the largest cell that fits.
void draw_art_box(u32 x0, u32 y0, u32 x1, u32 y1) {
    if (x1 <= x0 || y1 <= y0)
        return;
    u32 cw = 0;
    u32 ch = 0;
    art_cell(x1 - x0, y1 - y0, &cw, &ch);
    if (cw == 0u || ch == 0u)
        return;

    const u32 art_w = anko::art::kPanicArtCols * cw;
    const u32 art_h = anko::art::kPanicArtRows * ch;
    const u32 ox = x0 + ((x1 - x0) > art_w ? ((x1 - x0) - art_w) / 2u : 0u);
    const u32 oy = y0 + ((y1 - y0) > art_h ? ((y1 - y0) - art_h) / 2u : 0u);
    draw_art_at(ox, oy, cw, ch);
}

// Draw a QR code whose modules are `module` pixels, including the quiet zone.
// Returns the drawn width in pixels (0 when nothing was drawn).
u32 draw_qr(const anko::qr::Code& code, u32 x0, u32 y0, u32 module) {
    if (module == 0)
        return 0;
    const u32 side = (code.size + 2u * kQuietModules) * module;
    fill_rect(x0, y0, side, side, kQrPanel);
    for (u32 my = 0; my < code.size; ++my) {
        for (u32 mx = 0; mx < code.size; ++mx) {
            if (!anko::qr::dark(code, mx, my))
                continue;
            fill_rect(x0 + (mx + kQuietModules) * module, y0 + (my + kQuietModules) * module,
                      module, module, kQrModule);
        }
    }
    return side;
}

// Draw text wrapped to `cols` characters per line, honouring explicit newlines
// (which also give the blank separator lines), with `line_px` between baselines.
void draw_wrapped(u32 x, u32 y, u32 max_lines, u32 cols, u32 line_px, u32 fg, const char* text) {
    if (!text || cols == 0u)
        return;
    u32 line = 0;
    const char* p = text;
    while (*p && line < max_lines) {
        if (*p == '\n') {
            ++line;
            y += line_px;
            ++p;
            continue;
        }
        u32 run = 0;
        while (p[run] && p[run] != '\n' && run < cols)
            ++run;
        if (p[run] && p[run] != '\n') {
            // Prefer breaking at a space so values are not split mid-field.
            u32 brk = run;
            while (brk > 0u && p[brk - 1u] != ' ')
                --brk;
            if (brk > 0u)
                run = brk;
        }
        for (u32 i = 0; i < run; ++i) {
            const char ch = (p[i] >= 32 && p[i] <= 126) ? p[i] : '?';
            draw_glyph_sized(x + i * kCapCellW, y, kCapCellW, kCapCellH, fg, ch);
        }
        p += run;
        if (run != 0u) {
            ++line;
            y += line_px;
        }
        if (*p == ' ')
            ++p;
        // Consume the line terminator: the run above already advanced a line.
        if (*p == '\n')
            ++p;
    }
}

[[maybe_unused]] anko::qr::Code g_code; // static: never on a faulting stack

// Encode the payload; ECC M first, then L, so a denser payload does not silently
// lose the code.
bool encode_qr(const char* qr_payload) {
    if (!qr_payload || !qr_payload[0])
        return false;
    const u8* bytes = reinterpret_cast<const u8*>(qr_payload);
    const u32 len = slen(qr_payload);
    return anko::qr::encode(bytes, len, anko::qr::Level::M, g_code) ||
           anko::qr::encode(bytes, len, anko::qr::Level::L, g_code);
}

// Portrait: header, QR, report, logo across the bottom. See the file comment.
bool render_portrait(const char* head, const char* why, const char* caption,
                     const char* qr_payload) {
    const u32 m = clampu(g_w / 45u, 8u, 32u);
    const u32 title_scale = g_w >= 640u ? 3u : kTitleScale;
    const u32 reason_scale = g_w >= 640u ? 2u : kCaptionScale;

    draw_text(m, m, title_scale, kHeadFg, head, g_w / (kCellW * title_scale));
    const u32 reason_y = m + kCellH * title_scale + 6u;
    draw_text(m, reason_y, reason_scale, kReasonFg, why, g_w / (kCellW * reason_scale));
    progress();

    const u32 header_bottom = reason_y + kCellH * reason_scale + m;

    // Reserve the bottom third for the logo, sized from the panel width so it
    // spans the screen instead of sitting in a corner.
    const u32 band_h = g_h / 3u;
    u32 cw = 0;
    u32 ch = 0;
    art_cell(g_w - 2u * m, band_h > m ? band_h - m : 0u, &cw, &ch);
    const u32 art_h = cw ? anko::art::kPanicArtRows * ch : 0u;
    const u32 art_y0 = art_h && g_h > m + art_h ? g_h - m - art_h : g_h - m;
    const u32 info_bottom = art_y0 > m ? art_y0 - m : g_h - m;
    const u32 info_h = info_bottom > header_bottom ? info_bottom - header_bottom : 0u;

    // QR: at most 3/5 of the info band, so the report always keeps 2/5 and the
    // code stays large enough to scan off a 1080-wide panel.
    u32 qr_side = 0;
    if (info_h > 0u) {
        const u32 cap = smaller(g_w - 2u * m, info_h * 3u / 5u);
        if (encode_qr(qr_payload)) {
            const u32 modules = g_code.size + 2u * kQuietModules;
            const u32 module = cap / modules;
            if (module >= 1u)
                qr_side = draw_qr(g_code, m, header_bottom, module);
        }
    }
    progress();

    const u32 caption_y = header_bottom + qr_side + (qr_side ? m : 0u);
    const u32 line_px = kCapCellH + kCaptionGap;
    const u32 cols = (g_w - 2u * m) / kCapCellW;
    const u32 lines = info_bottom > caption_y ? (info_bottom - caption_y) / line_px : 0u;
    draw_wrapped(m, caption_y, smaller(lines, kMaxCaptionLines), cols, line_px, kCaptionFg,
                 caption);
    progress();

    if (art_h)
        draw_art_box(m, art_y0, g_w - m, g_h - m);
    return qr_side != 0u;
}

// Landscape: the original corner layout, unchanged for QEMU/T210.
bool render_landscape(const char* head, const char* why, const char* caption,
                      const char* qr_payload) {
    u32 qr_width = 0;
    bool have_qr = false;
    const u32 box = smaller(g_w / 3u, g_h / 2u);
    if (encode_qr(qr_payload)) {
        const u32 modules = g_code.size + 2u * kQuietModules;
        const u32 module = box / modules;
        if (module >= 1u) {
            qr_width = draw_qr(g_code, kMargin, kMargin, module);
            have_qr = qr_width != 0u;
        }
    }

    const u32 caption_x = kMargin;
    u32 caption_y = kMargin + qr_width + (qr_width ? kMargin : 0u);
    const u32 band_h = kCellH * kTitleScale + kCellH + 2u * kMargin;
    const u32 caption_limit = g_h > band_h + 4u ? g_h - band_h - 4u : caption_y;
    const u32 caption_line_px = kCapCellH + kCaptionGap;
    const u32 caption_cols = (qr_width ? qr_width : g_w - 2u * kMargin) / kCapCellW;
    const u32 caption_lines =
        caption_limit > caption_y ? (caption_limit - caption_y) / caption_line_px : 0u;
    draw_wrapped(caption_x, caption_y, smaller(caption_lines, kMaxCaptionLines), caption_cols,
                 caption_line_px, kCaptionFg, caption);
    progress();

    const u32 art_x0 = kMargin + qr_width + (qr_width ? kMargin : 0u);
    const u32 band_top = g_h > band_h ? g_h - band_h : 0u;
    draw_art_box(art_x0, kMargin, g_w - kMargin, band_top);

    u32 title_y = band_top + kMargin;
    draw_text(kMargin, title_y, kTitleScale, kHeadFg, head, g_w / (kCellW * kTitleScale));
    title_y += kCellH * kTitleScale;
    draw_text(kMargin, title_y, kCaptionScale, kReasonFg, why, g_w / kCellW);
    return have_qr;
}

} // namespace

void art_cell(u32 avail_w, u32 avail_h, u32* out_cw, u32* out_ch) {
    *out_cw = 0;
    *out_ch = 0;
    // The source cell is 8 wide by 16 tall, so keep ch == 2*cw to preserve the
    // art's proportions while shrinking the glyphs.
    const u32 by_width = avail_w / anko::art::kPanicArtCols;
    const u32 by_height = avail_h / (2u * anko::art::kPanicArtRows);
    const u32 cell = smaller(by_width, by_height);
    if (cell == 0u)
        return;
    *out_cw = cell;
    *out_ch = cell * 2u;
}

void bind(volatile u32* pixels, u32 width, u32 height) {
    bind(pixels, width, height, width);
}

void bind(volatile u32* pixels, u32 width, u32 height, u32 stride) {
    // Reject a framebuffer too small to show even one glyph row, or a stride that
    // does not cover the visible row (which would make every write overlap the
    // next row); an invalid bind leaves nothing bound rather than a half-usable
    // screen.
    if (!pixels || width < kCellW * 8u || height < kCellH * 8u || stride < width) {
        g_pixels = nullptr;
        g_base = 0;
        g_w = 0;
        g_h = 0;
        g_stride = 0;
        g_wide_ok = false;
        g_painted = false;
        return;
    }
    g_pixels = pixels;
    g_base = reinterpret_cast<u64>(const_cast<u32*>(pixels));
    g_w = width;
    g_h = height;
    g_stride = stride;
    g_wide_ok = ((g_base & 7u) == 0u) && ((stride & 1u) == 0u);
    g_painted = false;
}

void set_flush_hook(void (*fn)(u64 base, u32 bytes)) {
    g_flush = fn;
}

void set_progress_hook(void (*fn)()) {
    g_progress = fn;
}

bool bound() {
    return g_pixels != nullptr;
}

bool painted() {
    return g_painted;
}

bool show(const char* title, const char* reason, const char* caption, const char* qr_payload) {
    if (!g_pixels)
        return false;

    const char* head = (title && title[0]) ? title : "KERNEL PANIC";
    const char* why = (reason && reason[0]) ? reason : "unknown";

    fill_background();
    progress();

    const bool portrait = g_h >= (g_w * 3u) / 2u && g_w >= 200u;
    const bool have_qr = portrait ? render_portrait(head, why, caption, qr_payload)
                                  : render_landscape(head, why, caption, qr_payload);

    // Publish before the clean: from here on the console must not write over it.
    g_painted = true;
    if (g_flush)
        g_flush(g_base, g_stride * g_h * 4u);
#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#endif
    return have_qr;
}

} // namespace anko::panic_screen
