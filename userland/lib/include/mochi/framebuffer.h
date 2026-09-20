#pragma once
// mochi/framebuffer.h — io.framebuffer protocol.
//
// io.framebuffer owns the physical framebuffer arena and exposes a ring-based
// blit API. No other service touches framebuffer memory directly.

#include <cstdint>
#include "mochi/ring.h"

// Pixel format identifiers.
enum FbFormat : uint32_t {
    FB_FORMAT_XRGB8888 = 1,
};

// Framebuffer geometry descriptor.
struct FbInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride;  // bytes per row
    uint32_t format;  // FbFormat
    uint32_t flags;
    uint32_t reserved[3];
};
static_assert(sizeof(FbInfo) == 32);

// Ring message types for io.framebuffer endpoint.  Every command payload fits
// in one 48-byte ring slot; larger pixel transfers are intentionally not part
// of the v0 terminal path.
enum FbMsg : uint32_t {
    FB_INFO       = 1,  // no payload -> reply: FbInfo
    FB_CLEAR      = 2,  // payload: uint32_t color (XRGB8888)
    FB_FILL_RECT  = 3,  // payload: FbFillRectCmd
    FB_DRAW_GLYPH = 4,  // payload: FbDrawGlyphCmd
    FB_PRESENT    = 5,  // no payload; currently a no-op for ramfb
    FB_COPY_RECT  = 6,  // payload: FbCopyRectCmd
    FB_DRAW_TEXT  = 7,  // payload: FbDrawTextCmd
};

struct FbFillRectCmd {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
};
static_assert(sizeof(FbFillRectCmd) == 20);

// Copies pixels inside the framebuffer. Overlapping rectangles are supported,
// so tty.fb0 can scroll the terminal with one command instead of a full redraw.
struct FbCopyRectCmd {
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(FbCopyRectCmd) == 24);

// Draws a small monochrome glyph. `rows[n]` uses the high bits first, so an
// 8-pixel-wide glyph tests bit 7 for the leftmost pixel.
struct FbDrawGlyphCmd {
    uint32_t x;
    uint32_t y;
    uint32_t fg;
    uint32_t bg;
    uint8_t width;
    uint8_t height;
    uint8_t scale_x;
    uint8_t scale_y;
    uint8_t rows[16];
    uint8_t reserved[8];
};
static_assert(sizeof(FbDrawGlyphCmd) == 44);

constexpr uint32_t kFbDrawTextMaxChars = 28;

// Draws up to 28 contiguous 8x16 text cells. This is intentionally sized to
// fit exactly in one 48-byte ring payload and prevents boot log rendering from
// consuming one framebuffer command per character.
struct FbDrawTextCmd {
    uint32_t x;
    uint32_t y;
    uint32_t fg;
    uint32_t bg;
    uint8_t len;
    uint8_t scale_x;
    uint8_t scale_y;
    uint8_t reserved;
    char text[kFbDrawTextMaxChars];
};
static_assert(sizeof(FbDrawTextCmd) == 48);

namespace mochi::framebuffer {

struct Client {
    RingPage* cmd_ring;
    RingPage* reply_ring;
    uint32_t cmd_order;
    uint32_t reply_order;
    CapHandle cmd_doorbell;
    CapHandle reply_doorbell;

    bool valid() const {
        return cmd_ring && cmd_doorbell != MX_NULL_HANDLE;
    }

    bool try_send(uint32_t type, const void* data, uint32_t len) const {
        return valid() && ring_try_push(cmd_ring, cmd_order, type, data, len, cmd_doorbell);
    }

    bool send_spinning(uint32_t type, const void* data, uint32_t len,
                       uint32_t max_yields = 1024) const {
        if (!valid()) return false;
        while (!ring_try_push(cmd_ring, cmd_order, type, data, len, cmd_doorbell)) {
            if (max_yields == 0) return false;
            --max_yields;
            mx_yield();
        }
        return true;
    }

    bool clear(uint32_t color) const {
        return send_spinning(FB_CLEAR, &color, sizeof(color));
    }

    bool fill_rect(const FbFillRectCmd& cmd) const {
        return send_spinning(FB_FILL_RECT, &cmd, sizeof(cmd));
    }

    bool copy_rect(const FbCopyRectCmd& cmd) const {
        return send_spinning(FB_COPY_RECT, &cmd, sizeof(cmd));
    }

    bool draw_glyph(const FbDrawGlyphCmd& cmd) const {
        return send_spinning(FB_DRAW_GLYPH, &cmd, sizeof(cmd));
    }

    bool draw_text(const FbDrawTextCmd& cmd) const {
        return send_spinning(FB_DRAW_TEXT, &cmd, sizeof(cmd));
    }

    bool present() const {
        return send_spinning(FB_PRESENT, nullptr, 0);
    }

    bool request_info() const {
        return send_spinning(FB_INFO, nullptr, 0);
    }

    bool try_read_info(FbInfo* out) const {
        if (!out || !reply_ring) return false;
        RingSlot slot{};
        if (!ring_pop(reply_ring, reply_order, &slot)) return false;
        if (slot.type != FB_INFO || slot.length < sizeof(FbInfo)) return false;
        const auto& info = *reinterpret_cast<const FbInfo*>(slot.payload);
        *out = info;
        return true;
    }

    bool wait_read_info(FbInfo* out) const {
        if (!out || !reply_ring || reply_doorbell == MX_NULL_HANDLE) return false;
        for (;;) {
            RingSlot slot{};
            if (!ring_wait_pop(reply_ring, reply_order, &slot, reply_doorbell)) return false;
            if (slot.type == FB_INFO && slot.length >= sizeof(FbInfo)) {
                const auto& info = *reinterpret_cast<const FbInfo*>(slot.payload);
                *out = info;
                return true;
            }
        }
    }
};

struct Server {
    RingPage* cmd_ring;
    RingPage* reply_ring;
    uint32_t cmd_order;
    uint32_t reply_order;
    CapHandle cmd_doorbell;
    CapHandle reply_doorbell;

    bool valid() const {
        return cmd_ring && cmd_doorbell != MX_NULL_HANDLE;
    }

    bool try_recv(RingSlot* out) const {
        return cmd_ring && ring_pop(cmd_ring, cmd_order, out);
    }

    bool wait_recv(RingSlot* out) const {
        return cmd_ring && cmd_doorbell != MX_NULL_HANDLE &&
               ring_wait_pop(cmd_ring, cmd_order, out, cmd_doorbell);
    }

    bool send_info(const FbInfo& info) const {
        return reply_ring && reply_doorbell != MX_NULL_HANDLE &&
               ring_try_push(reply_ring, reply_order, FB_INFO, &info, sizeof(info), reply_doorbell);
    }
};

} // namespace mochi::framebuffer
