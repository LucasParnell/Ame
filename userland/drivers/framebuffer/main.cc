// userland/drivers/framebuffer/main.cc — io.framebuffer service (M2).
//
// Owns the physical framebuffer MMIO range granted by the kernel as "fb.memory"
// in BootInfo. Maps it, clears it to a solid color, and serves ring-based
// blit requests from tty.fb0.
//
// Cap policy: holds "fb.memory" CAP_MMIO only. No UART, no log rings direct.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/framebuffer.h"
#include "midix/font/vga8x16.h"
#include "midix/abi/ring.h"

// Virtual address layout for io.framebuffer.
constexpr uint64_t kVaBootInfo = 0x1000'0000ull;
constexpr uint64_t kVaLogRing  = 0x1800'0000ull;
constexpr uint64_t kVaFbPixels = 0x2000'0000ull; // framebuffer pixel arena
constexpr uint64_t kVaCmdRing  = 0x2100'0000ull;
constexpr uint64_t kVaReplyRing = 0x2102'0000ull; // keep clear of 0x11000-byte order-10 cmd ring

// Framebuffer dimensions — set per board by CMake (ANKO_FB_WIDTH/HEIGHT).
// QEMU virt: 1024×768.  Switch T210: 1280×720.
#ifndef ANKO_FB_WIDTH
#define ANKO_FB_WIDTH  1024
#endif
#ifndef ANKO_FB_HEIGHT
#define ANKO_FB_HEIGHT 768
#endif
constexpr uint32_t kFbWidth  = ANKO_FB_WIDTH;
constexpr uint32_t kFbHeight = ANKO_FB_HEIGHT;
[[maybe_unused]] constexpr uint32_t kFbStride = kFbWidth * 4; // XRGB8888
constexpr uint32_t kRingOrder = MX_DEFAULT_RING_ORDER;

// Clear color on startup: dark blue.
constexpr uint32_t kClearColor = 0x001F3F;

static void fb_clear(volatile uint32_t* pixels, uint32_t color) {
    const uint32_t total = kFbHeight * kFbWidth;
    for (uint32_t i = 0; i < total; ++i) {
        pixels[i] = color;
    }
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static bool both_8byte_aligned(const void* a, const void* b) {
    return ((reinterpret_cast<uint64_t>(a) | reinterpret_cast<uint64_t>(b)) & 7ull) == 0;
}

static void copy_words_forward(uint32_t* dst, const uint32_t* src, uint32_t words) {
    if (both_8byte_aligned(dst, src) && ((words & 1u) == 0)) {
        auto* d64 = reinterpret_cast<uint64_t*>(dst);
        const auto* s64 = reinterpret_cast<const uint64_t*>(src);
        for (uint32_t i = 0; i < words / 2u; ++i) d64[i] = s64[i];
        return;
    }
    for (uint32_t i = 0; i < words; ++i) dst[i] = src[i];
}

static void copy_words_backward(uint32_t* dst, const uint32_t* src, uint32_t words) {
    if (both_8byte_aligned(dst, src) && ((words & 1u) == 0)) {
        auto* d64 = reinterpret_cast<uint64_t*>(dst);
        const auto* s64 = reinterpret_cast<const uint64_t*>(src);
        for (uint32_t i = words / 2u; i > 0; --i) d64[i - 1u] = s64[i - 1u];
        return;
    }
    for (uint32_t i = words; i > 0; --i) dst[i - 1u] = src[i - 1u];
}

static void fb_fill_rect(volatile uint32_t* pixels, uint32_t x, uint32_t y,
                         uint32_t width, uint32_t height, uint32_t color) {
    if (x >= kFbWidth || y >= kFbHeight || width == 0 || height == 0) return;
    const uint32_t clipped_w = min_u32(width, kFbWidth - x);
    const uint32_t clipped_h = min_u32(height, kFbHeight - y);
    for (uint32_t row = 0; row < clipped_h; ++row) {
        volatile uint32_t* dst = pixels + ((y + row) * kFbWidth) + x;
        for (uint32_t col = 0; col < clipped_w; ++col) {
            dst[col] = color;
        }
    }
}

static void fb_copy_rect(volatile uint32_t* pixels, const FbCopyRectCmd& cmd) {
    if (cmd.src_x >= kFbWidth || cmd.dst_x >= kFbWidth ||
        cmd.src_y >= kFbHeight || cmd.dst_y >= kFbHeight ||
        cmd.width == 0 || cmd.height == 0) {
        return;
    }

    const uint32_t clipped_w = min_u32(cmd.width,
                                       min_u32(kFbWidth - cmd.src_x,
                                               kFbWidth - cmd.dst_x));
    const uint32_t clipped_h = min_u32(cmd.height,
                                       min_u32(kFbHeight - cmd.src_y,
                                               kFbHeight - cmd.dst_y));
    if (clipped_w == 0 || clipped_h == 0) return;

    const bool copy_top_down =
        (cmd.dst_y < cmd.src_y) ||
        (cmd.dst_y == cmd.src_y && cmd.dst_x <= cmd.src_x);

    // The ramfb backing store is normal RAM. Cast away volatile for bulk copies
    // so scrolling does not degrade into hundreds of thousands of strongly
    // ordered 32-bit accesses under QEMU TCG. FB_PRESENT provides the barrier.
    uint32_t* base = const_cast<uint32_t*>(pixels);

    for (uint32_t row_i = 0; row_i < clipped_h; ++row_i) {
        const uint32_t row = copy_top_down ? row_i : (clipped_h - 1u - row_i);
        const uint32_t src_off = ((cmd.src_y + row) * kFbWidth) + cmd.src_x;
        const uint32_t dst_off = ((cmd.dst_y + row) * kFbWidth) + cmd.dst_x;
        const uint32_t* src = base + src_off;
        uint32_t* dst = base + dst_off;

        const bool copy_left_to_right =
            (cmd.dst_y + row != cmd.src_y + row) || cmd.dst_x <= cmd.src_x;
        if (copy_left_to_right) {
            copy_words_forward(dst, src, clipped_w);
        } else {
            copy_words_backward(dst, src, clipped_w);
        }
    }
}

static void fb_draw_glyph(volatile uint32_t* pixels, const FbDrawGlyphCmd& cmd) {
    const uint32_t glyph_w = min_u32(cmd.width, 8);
    const uint32_t glyph_h = min_u32(cmd.height, 16);
    const uint32_t scale_x = cmd.scale_x ? min_u32(cmd.scale_x, 8) : 1;
    const uint32_t scale_y = cmd.scale_y ? min_u32(cmd.scale_y, 8) : 1;
    if (glyph_w == 0 || glyph_h == 0) return;

    for (uint32_t gy = 0; gy < glyph_h; ++gy) {
        const uint8_t bits = cmd.rows[gy];
        for (uint32_t sy = 0; sy < scale_y; ++sy) {
            const uint32_t py = cmd.y + gy * scale_y + sy;
            if (py >= kFbHeight) continue;
            for (uint32_t gx = 0; gx < glyph_w; ++gx) {
                const bool on = (bits & (0x80u >> gx)) != 0;
                const uint32_t color = on ? cmd.fg : cmd.bg;
                for (uint32_t sx = 0; sx < scale_x; ++sx) {
                    const uint32_t px = cmd.x + gx * scale_x + sx;
                    if (px < kFbWidth) pixels[py * kFbWidth + px] = color;
                }
            }
        }
    }
}

static void fb_draw_text(volatile uint32_t* pixels, const FbDrawTextCmd& cmd) {
    const uint32_t count = min_u32(cmd.len, static_cast<uint32_t>(sizeof(cmd.text)));
    const uint32_t scale_x = cmd.scale_x ? min_u32(cmd.scale_x, 8) : 1;
    const uint32_t scale_y = cmd.scale_y ? min_u32(cmd.scale_y, 8) : 1;

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t code = static_cast<uint8_t>(cmd.text[i]);
        const uint8_t* rows = midix::font::vga8x16_glyph(code);

        FbDrawGlyphCmd glyph{};
        glyph.x = cmd.x + (i * midix::font::kVga8x16Width * scale_x);
        glyph.y = cmd.y;
        glyph.fg = cmd.fg;
        glyph.bg = cmd.bg;
        glyph.width = static_cast<uint8_t>(midix::font::kVga8x16Width);
        glyph.height = static_cast<uint8_t>(midix::font::kVga8x16Height);
        glyph.scale_x = static_cast<uint8_t>(scale_x);
        glyph.scale_y = static_cast<uint8_t>(scale_y);
        for (uint32_t r = 0; r < midix::font::kVga8x16Height; ++r) {
            glyph.rows[r] = rows[r];
        }
        fb_draw_glyph(pixels, glyph);
    }
}

static void send_info(const mochi::framebuffer::Server& endpoint) {
    const FbInfo info{
        kFbWidth,
        kFbHeight,
        kFbStride,
        FB_FORMAT_XRGB8888,
        0,
        {0, 0, 0},
    };
    (void)endpoint.send_info(info);
}

static bool handle_fb_command(volatile uint32_t* pixels,
                              const mochi::framebuffer::Server& endpoint,
                              const RingSlot& slot) {
    switch (slot.type) {
    case FB_INFO:
        send_info(endpoint);
        return true;
    case FB_CLEAR:
        if (slot.length >= sizeof(uint32_t)) {
            const uint32_t color = *reinterpret_cast<const uint32_t*>(slot.payload);
            fb_clear(pixels, color);
        }
        return true;
    case FB_FILL_RECT:
        if (slot.length >= sizeof(FbFillRectCmd)) {
            const auto& cmd = *reinterpret_cast<const FbFillRectCmd*>(slot.payload);
            fb_fill_rect(pixels, cmd.x, cmd.y, cmd.width, cmd.height, cmd.color);
        }
        return true;
    case FB_DRAW_GLYPH:
        if (slot.length >= sizeof(FbDrawGlyphCmd)) {
            const auto& cmd = *reinterpret_cast<const FbDrawGlyphCmd*>(slot.payload);
            fb_draw_glyph(pixels, cmd);
        }
        return true;
    case FB_DRAW_TEXT:
        if (slot.length >= sizeof(FbDrawTextCmd)) {
            const auto& cmd = *reinterpret_cast<const FbDrawTextCmd*>(slot.payload);
            fb_draw_text(pixels, cmd);
        }
        return true;
    case FB_COPY_RECT:
        if (slot.length >= sizeof(FbCopyRectCmd)) {
            const auto& cmd = *reinterpret_cast<const FbCopyRectCmd*>(slot.payload);
            fb_copy_rect(pixels, cmd);
        }
        return true;
    case FB_PRESENT:
        asm volatile("dsb sy" ::: "memory");
        return true;
    default:
        return false;
    }
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);

    (void)mochi::log_map_from_boot(boot, kVaLogRing, "io.framebuffer");

    // Locate the framebuffer memory arena from BootInfo.
    mx_handle_t fb_arena = mochi::boot::handle(boot, "fb.memory");
    if (fb_arena == MX_NULL_HANDLE) {
        mochi::log_raw_marker("io.framebuffer: no fb.memory in BootInfo\n");
        while (true) { mx_yield(); }
    }

    // Map the framebuffer pixel arena (CAP_MMIO, NORMAL_NC).
    const mx_status_t map_rc = mx_map_mmio(fb_arena, kVaFbPixels,
                                           MX_RIGHT_READ | MX_RIGHT_WRITE);
    if (map_rc != MX_OK) {
        mochi::log_raw_marker("io.framebuffer: fb.memory map failed\n");
        while (true) { mx_yield(); }
    }

    volatile uint32_t* pixels = reinterpret_cast<volatile uint32_t*>(kVaFbPixels);
    fb_clear(pixels, kClearColor);

    mochi::log_raw_marker("io.framebuffer: ready\n");

    RingPage* cmd_ring = nullptr;
    RingPage* reply_ring = nullptr;
    uint32_t cmd_ring_order = kRingOrder;
    uint32_t reply_ring_order = kRingOrder;
    CapHandle cmd_db = mochi::boot::handle(boot, "fb.cmd.doorbell");
    CapHandle reply_db = mochi::boot::handle(boot, "fb.reply.doorbell");
    const MxBootEntry* cmd_ring_entry =
        mochi::boot::find_type(boot, "fb.cmd.ring", MX_BOOT_ENTRY_RING);
    const MxBootEntry* reply_ring_entry =
        mochi::boot::find_type(boot, "fb.reply.ring", MX_BOOT_ENTRY_RING);
    if (cmd_ring_entry && cmd_ring_entry->handle != MX_NULL_HANDLE &&
        mx_map_page(cmd_ring_entry->handle, kVaCmdRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        cmd_ring = reinterpret_cast<RingPage*>(kVaCmdRing);
        cmd_ring_order = cmd_ring_entry->aux0
            ? static_cast<uint32_t>(cmd_ring_entry->aux0)
            : kRingOrder;
    }
    if (reply_ring_entry && reply_ring_entry->handle != MX_NULL_HANDLE &&
        mx_map_page(reply_ring_entry->handle, kVaReplyRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        reply_ring = reinterpret_cast<RingPage*>(kVaReplyRing);
        reply_ring_order = reply_ring_entry->aux0
            ? static_cast<uint32_t>(reply_ring_entry->aux0)
            : kRingOrder;
    }
    const mochi::framebuffer::Server endpoint{
        cmd_ring,
        reply_ring,
        cmd_ring_order,
        reply_ring_order,
        cmd_db,
        reply_db,
    };
    if (!cmd_ring || cmd_db == MX_NULL_HANDLE) {
        mochi::log_raw_marker("io.framebuffer: no command endpoint\n");
    }

    mochi::boot::ready(boot);
    while (true) {
        if (!cmd_ring || cmd_db == MX_NULL_HANDLE) {
            mx_yield();
            continue;
        }

        RingSlot slot{};
        while (endpoint.try_recv(&slot)) {
            (void)handle_fb_command(pixels, endpoint, slot);
        }
        if (endpoint.wait_recv(&slot)) {
            (void)handle_fb_command(pixels, endpoint, slot);
        } else {
            mx_yield();
        }
    }
}
