#include "display_diagnostics.h"
#include "anko/spinlock.h"
#include "boot_diagnostics.h"
#include "debug_io.h"
#ifdef ANKO_ANNE_FRAMEBUFFER
#include "anko/framebuffer_console.h"
#endif

extern "C" u64 g_boot_el;
namespace anne::native_display {
namespace {
u32 read(u64 base, u32 offset) { return *reinterpret_cast<volatile u32*>(base + offset); }
void text(const char* s) {
    usize size = 0; while (s[size]) ++size;
    boot_log_write(s, size);
}
void value(u64 v) {
    char output[19] = {'0', 'x'};
    for (u32 i = 0; i < 16; ++i) output[i + 2] = "0123456789abcdef"[(v >> ((15 - i) * 4)) & 15];
    output[18] = ' ';
    boot_log_write(output, sizeof(output));
}
template <usize N> void dump(const char* name, u64 base, const u32 (&offsets)[N]) {
    text("DISPLAY "); text(name); text(" base="); value(base); text("\n");
    for (u32 offset : offsets) {
        text(" reg="); value(offset); text("value="); value(read(base, offset)); text("\n");
    }
    boot_log_flush();
}
Pipeline pipeline(Channel c) {
    const u64 dma = kDssBase + c.offset, ldi = kDssBase + 0x7d000;
    Pipeline p{};
    p.index = c.index;
    p.dma = {read(dma, 0xd4), read(dma, 0x1c), read(dma, 0x60), read(dma, 0x64),
             read(dma, 0), read(dma, 4), read(dma, 8), read(dma, 12),
             read(ldi, 0x24), read(ldi, 8), read(ldi, 0x14)};
    p.secure = read(dma, 0xd8); p.mask0 = read(dma, 0x10); p.mask1 = read(dma, 0x14);
    p.stretch_stride = read(dma, 0x68);
    const u64 dsi = kDssBase + 0x1000;
    p.dsi_power = read(dsi, 4); p.dsi_mode = read(dsi, 0x34); p.dsi_phy = read(dsi, 0xb0);
    p.dsi_width = read(dsi, 0x3c); p.dsi_height = read(dsi, 0x60);
    const u64 mctl = kDssBase + 0x10800, ov = kDssBase + 0x60000;
    p.mctl_enable = read(mctl, 0); p.mctl_itf = read(mctl, 0xc);
    p.mctl_dbuf = read(mctl, 0x10); p.mctl_overlay = read(mctl, 0x18);
    p.overlay_size = read(ov, 0); p.overlay_config = read(ov, 0x10);
    p.overlay_start = read(ov, 8); p.overlay_end = read(ov, 0xc);
    p.channel_overlay_enable = read(kDssBase + 0x10000, 0x160 + c.index * 4);
    p.channel_select = read(kDssBase + 0x10000, 0x180);
    for (u32 i = 0; i < 6; ++i) {
        p.layer_position[i] = read(ov, 0x14 + i * 0x3c);
        p.layer_end[i] = read(ov, 0x18 + i * 0x3c);
        p.layer_config[i] = read(ov, 0x34 + i * 0x3c);
    }
    p.dfc_size = read(dma, 0x100); p.dfc_clip_h = read(dma, 0x110);
    p.dfc_clip_v = read(dma, 0x114); p.dfc_padding = read(dma, 0x124);
    return p;
}
void paint_pattern(const minimal::Buffer& fb, u64 el) {
    constexpr u32 left = 64, top = 144, scale = 8;
    const char* label = el == 1 ? "ANKO EL1" : "ANKO EL2";
    for (u32 y = 0; y < fb.height; ++y) {
        for (u32 x = 0; x < fb.width; ++x) {
            u32 rgb = el == 1 ? 0x00ff00 : 0xff00ff;
            if (x >= left - scale && x < left + 49 * scale && y >= top - scale && y < top + 8 * scale) {
                rgb = 0;
                if (x >= left && y >= top) {
                    const u32 cx = (x - left) / scale, cy = (y - top) / scale;
                    if (cy < 7 && cx < 48 && cx % 6 < 5 &&
                        (minimal::glyph(label[cx / 6], cy) & (1u << (4 - cx % 6)))) rgb = 0xffffff;
                }
            }
            const u64 address = fb.address + u64{y} * fb.stride + x * fb.bpp();
            if (fb.bpp() == 2) *reinterpret_cast<volatile u16*>(address) = minimal::pixel(fb.format, rgb);
            else *reinterpret_cast<volatile u32*>(address) = minimal::pixel(fb.format, rgb);
        }
    }
}
#ifdef ANKO_ANNE_FRAMEBUFFER
console::FramebufferConsole screen;
kernel::SpinLock screen_lock{};
bool screen_enabled;
char screen_line[512];
u32 screen_line_length;
bool prefix(const char* value, const char* start) {
    for (u32 i = 0; start[i]; ++i) if (value[i] != start[i]) return false;
    return true;
}
void screen_commit_line() {
    screen_line[screen_line_length] = '\0';
    if (!screen_line_length || prefix(screen_line, "DISPLAY ") || prefix(screen_line, " reg=") ||
        prefix(screen_line, "CTOR ") || prefix(screen_line, "LED ") || prefix(screen_line, "AMEANKO1 ") || prefix(screen_line, "RUN tag=")) {
        screen_line_length = 0;
        return;
    }
    u32 length = screen_line_length;
    if (prefix(screen_line, "BOOT ")) {
        for (u32 i = 0; i + 6 < length; ++i) {
            if (screen_line[i] == ' ' && prefix(screen_line + i, " tick=")) { length = i; break; }
        }
    }
    screen.write(screen_line, length);
    screen.putchar('\n');
    screen_line_length = 0;
}
#endif
}
void snapshot(const char* phase, bool paint) {
    text("\nDISPLAY begin phase="); text(phase); text("\n"); boot_log_flush();
    constexpr u32 crg[] = {0x38, 0x3c, 0x48, 0x4c, 0x58, 0x5c, 0x80, 0x8c, 0x98, 0xa4};
    dump("crg", kCrgBase, crg);
    constexpr u32 glb[] = {0x100, 0x108, 0x10c, 0x110, 0x118, 0x11c, 0x120, 0x12c, 0x130, 0x224, 0x300, 0x304};
    dump("glb", kDssBase, glb);
    constexpr u32 dsi[] = {4, 8, 0x10, 0x34, 0x38, 0x3c, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x94, 0xa0, 0xa4, 0xb0};
    dump("dsi0", kDssBase + 0x1000, dsi);
    constexpr u32 ldi[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x24, 0x28, 0x44, 0x54, 0x58, 0x110, 0x118, 0x248};
    dump("ldi0", kDssBase + 0x7d000, ldi);
    constexpr u32 smmu[] = {0, 0x24, 0x28, 0x204, 0x20c, 0x210, 0x218, 0x300};
    dump("smmu", kDssBase + 0x8000, smmu);
    constexpr u32 mctl[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x50, 0x54, 0x68};
    dump("mctl0", kDssBase + 0x10800, mctl);
    constexpr u32 sys[] = {0x100, 0x104, 0x110, 0x114, 0x118, 0x11c, 0x128, 0x160, 0x164,
                           0x170, 0x174, 0x178, 0x17c, 0x180, 0x190, 0x1b4};
    dump("mctl_sys", kDssBase + 0x10000, sys);
    constexpr u32 overlay[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x34, 0x50, 0x54, 0x70,
                               0x8c, 0x90, 0xac, 0xc8, 0xcc, 0xe8, 0x104, 0x108, 0x124,
                               0x140, 0x144, 0x160, 0x188, 0x1a0};
    dump("ovl0", kDssBase + 0x60000, overlay);
    constexpr u32 dbuf[] = {0, 4, 8, 0x3c, 0x44, 0x60};
    dump("dbuf0", kDssBase + 0x6d000, dbuf);
    bool painted = false;
    for (Channel c : kChannels) {
        text("DISPLAY channel="); value(c.index); text("\n");
        constexpr u32 dma[] = {0, 4, 8, 12, 0x10, 0x14, 0x18, 0x1c, 0x60, 0x64, 0x68, 0x80,
                               0xd0, 0xd4, 0xd8, 0xe4, 0x100, 0x10c, 0x110, 0x114, 0x124, 0x850, 0x900, 0x914};
        dump("rdma_dfc", kDssBase + c.offset, dma);
        const Pipeline p = pipeline(c);
        const char* why = rejection(p);
        text("DISPLAY candidate channel="); value(c.index); text("reject="); text(why ? why : "none"); text("\n");
        if (paint && !painted && !why) {
            const minimal::Buffer fb{p.dma.address, 1080, 2280, p.dma.stride * 16, (p.dma.control >> 3) & 31};
            text("DISPLAY paint.begin address="); value(fb.address); text("\n"); boot_log_flush();
            paint_pattern(fb, g_boot_el);
            debug_clean(fb.address, fb.stride * fb.height);
            text("DISPLAY paint.done label=ANKO hold_seconds=5 inset=64,144\n"); boot_log_flush();
#ifndef ANKO_ANNE_DIAG_TEST
            minimal::wait_ticks(5 * minimal::kCounterHz);
#endif
            painted = true;
        }
    }
    text(paint && !painted ? "DISPLAY paint=skip no_verified_scanout\n" : "DISPLAY snapshot.done\n");
    boot_log_flush();
}
#ifdef ANKO_ANNE_FRAMEBUFFER
bool bind_console() {
    screen_enabled = false; screen_line_length = 0;
    for (Channel channel : kChannels) {
        const Pipeline p = pipeline(channel);
        if (rejection(p) || ((p.dma.control >> 3) & 31) < 5) continue;
        const libk::Framebuffer32 fb{p.dma.address, 1080, 2280};
        if (!libk::framebuffer_fits(fb, kLogoBase, kLogoBytes)) continue;
        libk::framebuffer_fill(fb, console::FramebufferConsole::kBackground);
        // Row 0 of the former viewport is kept as a fixed status line: the
        // log viewport wraps, so without it the newest stage can land mid-screen.
        if (!screen.bind(fb, kLogoBase, kLogoBytes, {64, 256, 952, 1928, 2},
                         console::FramebufferConsole::Overflow::Wrap)) continue;
        screen.draw_text(64, 144, 3, g_boot_el == 1 ? "ANKO EL1" : "ANKO EL2", 0xff7ee6a8);
        debug_clean(fb.address, fb.width * fb.height * 4);
        screen_enabled = true;
        text("DISPLAY console.ready channel="); value(channel.index); text("address="); value(fb.address);
        text("cols=59 rows=60 inset=64,256 scale=2\n"); boot_log_flush();
        console_status("stage: console.ready");
        return true;
    }
    text("DISPLAY console=skip no_verified_32bit_scanout\n"); boot_log_flush();
    return false;
}
bool framebuffer(u64& address, u32& width, u32& height) {
    for (Channel channel : kChannels) {
        const Pipeline p = pipeline(channel);
        if (!rejection(p) && ((p.dma.control >> 3) & 31) >= 5) {
            address = p.dma.address; width = 1080; height = 2280; return true;
        }
    }
    return false;
}
void handoff_console() {
    console_flush();
    kernel::LockGuard guard(screen_lock);
    __atomic_store_n(&screen_enabled, false, __ATOMIC_RELEASE);
}
void console_status(const char* text) {
    kernel::LockGuard guard(screen_lock);
    if (!screen_enabled || !text) return;
    char line[60];
    u32 n = 0;
    while (text[n] && n < 59) { line[n] = text[n]; ++n; }
    while (n < 59) line[n++] = ' ';
    line[59] = '\0';
    // Same cell as the viewport's first row, so it never shifts with the ring.
    screen.draw_text(64, 224, 2, line, 0xffffd070);
}
void console_putchar(char c) {
    kernel::LockGuard guard(screen_lock);
    if (!screen_enabled) return;
    if (c == '\n') screen_commit_line();
    else if (screen_line_length + 1 < sizeof(screen_line)) screen_line[screen_line_length++] = c;
}
void console_flush() {
    kernel::LockGuard guard(screen_lock);
    u64 address; u32 bytes;
    if (screen_enabled && screen.take_dirty(address, bytes)) debug_clean(address, bytes);
}
bool console_active() { return screen_enabled; }
u64 console_characters() { return screen.characters(); }
#endif
}
