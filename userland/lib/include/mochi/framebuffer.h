#pragma once
// mochi/framebuffer.h — io.framebuffer client and server over mochi::Ring.
//
// The wire contract (FbInfo, the FB_* opcodes, the command structs) lives in the
// ABI header midix/abi/protocols/framebuffer.h; these are the ergonomic
// endpoints. Each side owns two Rings: the command ring and the reply ring.

#include "midix/abi/protocols/framebuffer.h"
#include "mochi/message.h"
#include "mochi/ring.h"

namespace mochi::framebuffer {

struct Client {
    Ring cmd;
    Ring reply;

    bool valid() const { return cmd.valid(); }

    bool try_send(uint32_t type, const void* data, uint32_t len) const {
        return cmd.try_push(type, data, len);
    }

    bool send_spinning(uint32_t type, const void* data, uint32_t len,
                       uint32_t max_yields = 1024) const {
        return cmd.valid() && cmd.push(type, data, len, max_yields);
    }

    bool clear(uint32_t color) const {
        return send_spinning(FB_CLEAR, &color, sizeof(color));
    }

    bool fill_rect(const FbFillRectCmd& c) const {
        return send_spinning(FB_FILL_RECT, &c, sizeof(c));
    }

    bool copy_rect(const FbCopyRectCmd& c) const {
        return send_spinning(FB_COPY_RECT, &c, sizeof(c));
    }

    bool draw_glyph(const FbDrawGlyphCmd& c) const {
        return send_spinning(FB_DRAW_GLYPH, &c, sizeof(c));
    }

    bool draw_text(const FbDrawTextCmd& c) const {
        return send_spinning(FB_DRAW_TEXT, &c, sizeof(c));
    }

    bool present() const {
        return send_spinning(FB_PRESENT, nullptr, 0);
    }

    bool request_info() const {
        return send_spinning(FB_INFO, nullptr, 0);
    }

    bool try_read_info(FbInfo* out) const {
        return reply.try_pop_msg(FB_INFO, out);
    }

    bool wait_read_info(FbInfo* out) const {
        return reply.wait_pop_msg(FB_INFO, out);
    }
};

struct Server {
    Ring cmd;
    Ring reply;

    bool valid() const { return cmd.valid(); }

    bool try_recv(RingSlot* out) const { return cmd.try_pop(out); }

    bool wait_recv(RingSlot* out) const { return cmd.wait_pop(out); }

    bool send_info(const FbInfo& info) const {
        return reply.valid() && reply.try_push_msg(FB_INFO, info);
    }
};

} // namespace mochi::framebuffer
