// Development authority: opaque CDC/bulk bytes -> validated key events on its
// OWN producer ring. ui.input remains the single layout/lock/terminal policy.
#include "mochi/driver.h"
#include "mochi/input.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/usb_function.h"
#include "midix/debug/usb_input.h"
namespace {
struct Source {
    midix::debug::Parser parser;
    MxFunctionChunk chunk, reply;
    mx_u64_t generation, sequence;
    mx_u32_t position, wire_sequence;
    InputEvent events[3];
    mx_u32_t count, next;
    bool attached, held, reply_pending;
};
struct Runtime {
    mochi::SpscEndpoint links[2];
    mochi::input::EventSink input;
    Source sources[2];
};
void event(Source& s, mx_u32_t index, mx_u32_t type, mx_u32_t key = 0, mx_u32_t modifiers = 0) {
    s.events[s.count++] = {type,         key,          modifiers, 0, 0xf000000000000001ull + index,
                           s.generation, ++s.sequence, 7,         0};
}
void frame(Source& s, mx_u32_t index) {
    using namespace midix::debug;
    const auto* bytes = s.parser.bytes;
    const auto sequence = libk::load_le<mx_u32_t>(bytes + 4);
    const auto length = libk::load_le<mx_u16_t>(bytes + 10);
    const auto type = libk::load_le<mx_u16_t>(bytes + 8);
    mx_status_t status = MX_OK;
    if (!sequence || (type != kHello && sequence <= s.wire_sequence)) {
        status = MX_ERR_INVALID;
    } else if (type == kHello && !length) {
        if (s.attached) {
            event(s, index, INPUT_SOURCE_RESET);
        }
        s.generation = mx_clock_ns();
        s.sequence = 0;
        event(s, index, INPUT_SOURCE_ATTACHED);
        s.attached = true;
    } else if (type == kRelease && !length) {
        if (s.attached) {
            event(s, index, INPUT_SOURCE_RESET);
        }
        s.attached = false;
    } else if (type == kKey && length == 4 && s.attached) {
        const auto key = libk::load_le<mx_u16_t>(bytes + kHeader);
        const auto modifiers = bytes[kHeader + 2], pressed = bytes[kHeader + 3];
        if (key < 4 || key > 0xe7 || pressed > 1) {
            status = MX_ERR_INVALID;
        } else {
            if (s.wire_sequence && sequence != s.wire_sequence + 1) {
                event(s, index, INPUT_SOURCE_RESET);
            }
            event(s, index, pressed ? INPUT_KEY_DOWN : INPUT_KEY_UP, key, modifiers);
        }
    } else {
        status = MX_ERR_BAD_PROTOCOL;
    }
    if (sequence > s.wire_sequence) {
        s.wire_sequence = sequence;
    }
    s.reply = {s.chunk.generation, sequence, 20, 0, 0, {}};
    libk::write_le<mx_u32_t>(libk::as_bytes(s.reply.data, sizeof(mx_u32_t)), kInputMagic);
    libk::write_le<mx_u32_t>(libk::as_bytes(s.reply.data + 4, sizeof(mx_u32_t)), sequence);
    libk::write_le<mx_u16_t>(libk::as_bytes(s.reply.data + 8, sizeof(mx_u16_t)), type | kAck);
    libk::write_le<mx_u16_t>(libk::as_bytes(s.reply.data + 10, sizeof(mx_u16_t)), 4);
    libk::write_le<mx_u32_t>(libk::as_bytes(s.reply.data + 16, sizeof(mx_u32_t)),
                             static_cast<mx_u32_t>(status));
    libk::write_le<mx_u32_t>(libk::as_bytes(s.reply.data + 12, sizeof(mx_u32_t)),
                             checksum(libk::as_bytes(s.reply.data, 20)));
    s.reply_pending = true;
    s.parser.reset();
}
void service(Runtime& r, mx_u32_t i) {
    auto& s = r.sources[i];
    while (s.next < s.count) {
        if (!r.input.try_send(s.events[s.next])) {
            return;
        }
        ++s.next;
    }
    s.count = s.next = 0;
    if (s.reply_pending) {
        if (!r.links[i].tx().try_push_msg(MX_FUNCTION_TX, s.reply)) {
            return;
        }
        s.reply_pending = false;
    }
    if (!s.held) {
        MxRingSlot slot{};
        if (!r.links[i].try_recv(&slot)) {
            return;
        }
        if (slot.type == MX_FUNCTION_RESET) {
            if (s.attached) {
                event(s, i, INPUT_SOURCE_RESET);
            }
            s.attached = false;
            s.wire_sequence = 0;
            s.parser.reset();
            return;
        }
        if (!mochi::message_get(slot, &s.chunk, MX_FUNCTION_RX) || s.chunk.length > 32) {
            return;
        }
        s.position = 0;
        s.held = true;
    }
    while (s.position < s.chunk.length) {
        if (s.parser.push(s.chunk.data[s.position++])) {
            frame(s, i);
            return;
        }
    }
    s.held = false;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "dev.usb-input");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->links[0] = mochi::driver::link(boot, "stream.0", mochi::va::native_driver::kLinks);
    r->links[1] = mochi::driver::link(
        boot, "stream.1",
        (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    r->input = {mochi::boot::map_ring(
        boot, {"test.input.ring",
               (mochi::va::native_driver::kLinks + 2 * mochi::va::native_driver::kLinkStride),
               "test.input.doorbell"})};
    if (!r->links[0].valid() || !r->links[1].valid() || !r->input.valid()) {
        mochi::boot::invalid_boot();
    }
    for (auto& s : r->sources) {
        s.generation = mx_clock_ns();
    }
    mochi::boot::ready(boot);
    for (;;) {
        service(*r, 0);
        service(*r, 1);
        mochi::driver::idle(r->links, 2, r->links[0].rx_doorbell, mx_clock_ns() + 10000000);
    }
}
