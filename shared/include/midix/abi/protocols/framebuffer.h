#pragma once
// Midix framebuffer service protocol ABI (io.framebuffer).
//
// io.framebuffer owns the physical framebuffer arena and exposes a ring-based
// blit API; no other service touches framebuffer memory directly. Every command
// payload fits one 48-byte ring-slot payload, so a text run or a scroll is one
// message, not one message per pixel.
//
// Transport envelope: MxRingSlot.

#include "midix/abi/types.h"
#include "midix/abi/ring.h"

constexpr mx_u32_t MX_PROTOCOL_FRAMEBUFFER = 0x46425231u; // 'FBR1'

using MxFbFrame = MxRingSlot;
static_assert(sizeof(MxFbFrame) == 64, "framebuffer frame must be one ring slot");

// Pixel format identifiers.
enum FbFormat : mx_u32_t {
    FB_FORMAT_XRGB8888 = 1,
};

// Framebuffer geometry descriptor.
struct FbInfo {
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t stride;  // bytes per row
    mx_u32_t format;  // FbFormat
    mx_u32_t flags;
    mx_u32_t reserved[3];
};
static_assert(sizeof(FbInfo) <= MX_RING_INLINE_PAYLOAD, "FbInfo must fit one payload");
static_assert(sizeof(FbInfo) == 32, "FbInfo size drift");

// Ring message types for the io.framebuffer endpoint.
enum FbMsg : mx_u32_t {
    FB_INFO       = 1, // no payload -> reply: FbInfo
    FB_CLEAR      = 2, // payload: uint32_t color (XRGB8888)
    FB_FILL_RECT  = 3, // payload: FbFillRectCmd
    FB_DRAW_GLYPH = 4, // payload: FbDrawGlyphCmd
    FB_PRESENT    = 5, // no payload; currently a no-op for ramfb
    FB_COPY_RECT  = 6, // payload: FbCopyRectCmd
    FB_DRAW_TEXT  = 7, // payload: FbDrawTextCmd
};

struct FbFillRectCmd {
    mx_u32_t x;
    mx_u32_t y;
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t color;
};
static_assert(sizeof(FbFillRectCmd) == 20, "fill-rect size drift");

// Copies pixels inside the framebuffer. Overlapping rectangles are supported, so
// tty.fb0 can scroll the terminal with one command instead of a full redraw.
struct FbCopyRectCmd {
    mx_u32_t src_x;
    mx_u32_t src_y;
    mx_u32_t dst_x;
    mx_u32_t dst_y;
    mx_u32_t width;
    mx_u32_t height;
};
static_assert(sizeof(FbCopyRectCmd) == 24, "copy-rect size drift");

// Draws a small monochrome glyph. `rows[n]` uses the high bits first, so an
// 8-pixel-wide glyph tests bit 7 for the leftmost pixel.
struct FbDrawGlyphCmd {
    mx_u32_t x;
    mx_u32_t y;
    mx_u32_t fg;
    mx_u32_t bg;
    mx_u8_t width;
    mx_u8_t height;
    mx_u8_t scale_x;
    mx_u8_t scale_y;
    mx_u8_t rows[16];
    mx_u8_t reserved[8];
};
static_assert(sizeof(FbDrawGlyphCmd) == 44, "draw-glyph size drift");

constexpr mx_u32_t kFbDrawTextMaxChars = 28;

// Draws up to kFbDrawTextMaxChars contiguous 8x16 text cells, sized to fit
// exactly in one 48-byte ring payload.
struct FbDrawTextCmd {
    mx_u32_t x;
    mx_u32_t y;
    mx_u32_t fg;
    mx_u32_t bg;
    mx_u8_t len;
    mx_u8_t scale_x;
    mx_u8_t scale_y;
    mx_u8_t reserved;
    char text[kFbDrawTextMaxChars];
};
static_assert(sizeof(FbDrawTextCmd) == 48, "draw-text size drift");
