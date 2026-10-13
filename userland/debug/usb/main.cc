// Development authority: opaque bulk bytes -> validated key events on its OWN
// producer ring. ui.input remains the single layout/lock/terminal policy.
//
// The bulk function carries the framed keycode protocol (scripts/anne/
// usb-input.py --bulk). The ACM stream belongs to dev.serial, which serves
// the shell as a raw serial console.
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
    mochi::SpscEndpoint link;
    mochi::input::EventSink input;
    Source source;
};
void event(Source& s, mx_u32_t type, mx_u32_t key = 0, mx_u32_t modifiers = 0) {
    s.events[s.count++] = {type,         key,          modifiers, 0, 0xf000000000000001ull,
                           s.generation, ++s.sequence, 7,         0};
}
void frame(Source& s) {
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
            event(s, INPUT_SOURCE_RESET);
        }
        s.generation = mx_clock_ns();
        s.sequence = 0;
        event(s, INPUT_SOURCE_ATTACHED);
        s.attached = true;
    } else if (type == kRelease && !length) {
        if (s.attached) {
            event(s, INPUT_SOURCE_RESET);
        }
        s.attached = false;
    } else if (type == kKey && length == 4 && s.attached) {
        const auto key = libk::load_le<mx_u16_t>(bytes + kHeader);
        const auto modifiers = bytes[kHeader + 2], pressed = bytes[kHeader + 3];
        if (key < 4 || key > 0xe7 || pressed > 1) {
            status = MX_ERR_INVALID;
        } else {
            if (s.wire_sequence && sequence != s.wire_sequence + 1) {
                event(s, INPUT_SOURCE_RESET);
            }
            event(s, pressed ? INPUT_KEY_DOWN : INPUT_KEY_UP, key, modifiers);
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
bool service(Runtime& r) {
    auto& s = r.source;
    bool progress = false;
    while (s.next < s.count) {
        if (!r.input.try_send(s.events[s.next])) {
            return progress;
        }
        ++s.next;
        progress = true;
    }
    s.count = s.next = 0;
    if (s.reply_pending) {
        if (!r.link.tx().try_push_msg(MX_FUNCTION_TX, s.reply)) {
            return progress;
        }
        s.reply_pending = false;
        progress = true;
    }
    if (!s.held) {
        MxRingSlot slot{};
        if (!r.link.try_recv(&slot)) {
            return progress;
        }
        progress = true;
        if (slot.type == MX_FUNCTION_RESET) {
            if (s.attached) {
                event(s, INPUT_SOURCE_RESET);
            }
            s.attached = false;
            s.wire_sequence = 0;
            s.parser.reset();
            return progress;
        }
        if (!mochi::message_get(slot, &s.chunk, MX_FUNCTION_RX) || s.chunk.length > 32) {
            return progress;
        }
        s.position = 0;
        s.held = true;
    }
    while (s.position < s.chunk.length) {
        progress = true;
        if (s.parser.push(s.chunk.data[s.position++])) {
            frame(s);
            return progress;
        }
    }
    s.held = false;
    return progress;
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
    r->link = mochi::driver::link(boot, "stream", mochi::va::native_driver::kLinks);
    r->input = {mochi::boot::map_ring(
        boot, {"test.input.ring",
               (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride),
               "test.input.doorbell"})};
    if (!r->link.valid() || !r->input.valid()) {
        mochi::boot::invalid_boot();
    }
    r->source.generation = mx_clock_ns();
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return service(*r); })) {
            mx_yield();
            continue;
        }
        const auto& source = r->source;
        const bool retained = source.held || source.reply_pending || source.next < source.count;
        mochi::driver::idle(&r->link, 1, r->link.rx_doorbell,
                            retained ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0,
                            retained ? 0u : 1u);
    }
}
