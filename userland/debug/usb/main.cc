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
#include "midix/abi/protocols/app_stream.h"
namespace {
struct Source {
    midix::debug::Parser parser;
    MxFunctionChunk chunk, reply;
    mx_u64_t generation, sequence;
    mx_u32_t position, wire_sequence;
    InputEvent events[3];
    mx_u32_t count, next;
    bool attached, held, reply_pending;
    mx_u32_t app_offset;
    bool app_pending, app_reset;
    mx_u64_t usb_generation;
    bool configured;
};
struct Runtime {
    mochi::SpscEndpoint link;
    mochi::input::EventSink input;
    Source source;
    mochi::SpscEndpoint apps;
    MxRingSlot app_reply;
    bool app_reply_pending;
};
void event(Source& s, mx_u32_t type, mx_u32_t key = 0, mx_u32_t modifiers = 0) {
    s.events[s.count++] = {type,         key,          modifiers, 0, 0xf000000000000001ull,
                           s.generation, ++s.sequence, 7,         0};
}
void frame(Runtime& r) {
    auto& s = r.source;
    using namespace midix::debug;
    const auto* bytes = s.parser.bytes;
    const auto sequence = libk::load_le<mx_u32_t>(bytes + 4);
    const auto length = libk::load_le<mx_u16_t>(bytes + 10);
    const auto type = libk::load_le<mx_u16_t>(bytes + 8);
    if (midix::apps::wire_type(type) && r.apps.valid()) {
        s.app_pending = true;
        s.app_offset = 0;
        return;
    }
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
    if (r.apps.valid()) {
        if (!r.app_reply_pending && r.apps.try_recv(&r.app_reply)) {
            MxFunctionChunk reply{};
            r.app_reply_pending = mochi::message_get(r.app_reply, &reply, MX_FUNCTION_TX) &&
                                  s.configured && reply.generation == s.usb_generation;
            progress = true;
        }
        if (r.app_reply_pending && r.link.tx().try_push_frame(r.app_reply)) {
            r.app_reply_pending = false; libk::zero(r.app_reply); progress = true;
        }
        if (s.app_reset) {
            MxFunctionChunk reset{s.usb_generation, 0, 0, 0,
                static_cast<mx_u8_t>(s.configured ? MX_FUNCTION_CONFIGURED : 0), {}};
            if (!r.apps.tx().try_push_msg(MX_FUNCTION_RESET, reset)) return progress;
            s.app_reset = false; progress = true;
        }
        if (s.app_pending) {
            const auto size = midix::debug::kHeader + libk::load_le<mx_u16_t>(s.parser.bytes + 10);
            const auto count = (size - s.app_offset < 32 ? size - s.app_offset : 32);
            MxFunctionChunk chunk{s.usb_generation, 0, static_cast<mx_u16_t>(count), 0, 0, {}};
            libk::copy_n(libk::as_bytes(chunk.data),
                libk::as_bytes(s.parser.bytes + s.app_offset, count), count);
            if (!r.apps.tx().try_push_msg(MX_FUNCTION_RX, chunk)) return progress;
            s.app_offset += count;
            if (s.app_offset == size) {
                s.app_pending = false;
                libk::zero(s.parser.bytes); s.parser.reset();
            }
            return true;
        }
    }
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
            MxFunctionChunk reset{};
            if (!mochi::message_get(slot, &reset, MX_FUNCTION_RESET) || !reset.generation ||
                reset.length || (reset.flags & ~MX_FUNCTION_CONFIGURED)) return progress;
            s.usb_generation = reset.generation;
            s.configured = (reset.flags & MX_FUNCTION_CONFIGURED) != 0;
            s.app_reset = r.apps.valid();
            r.app_reply_pending = false; libk::zero(r.app_reply);
            if (s.attached) {
                event(s, INPUT_SOURCE_RESET);
            }
            s.attached = false;
            s.wire_sequence = 0;
            s.parser.reset();
            libk::zero(s.parser.bytes);
            return progress;
        }
        if (!mochi::message_get(slot, &s.chunk, MX_FUNCTION_RX) || s.chunk.length > 32 ||
            !s.configured || s.chunk.generation != s.usb_generation || s.chunk.flags) {
            return progress;
        }
        s.position = 0;
        s.held = true;
    }
    while (s.position < s.chunk.length) {
        progress = true;
        const auto byte = s.chunk.data[s.position]; s.chunk.data[s.position++] = 0;
        if (s.parser.push(byte)) {
            frame(r);
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
    if (mochi::boot::handle(boot, "apps.tx")) {
        r->apps = mochi::driver::link(boot, "apps", mochi::va::native_driver::kLinks + 0x40000);
        if (!r->apps.valid()) mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return service(*r); })) {
            mx_yield();
            continue;
        }
        const auto& source = r->source;
        const bool retained = source.held || source.reply_pending || source.next < source.count ||
                              source.app_pending || source.app_reset || r->app_reply_pending;
        // Both producers use the bulk receiver's doorbell. Arm the app reply
        // ring too: the host waits for its ACK without sending another packet.
        const mochi::SpscEndpoint links[] = {r->link, r->apps};
        mochi::driver::idle(links, r->apps.valid() ? 2 : 1, r->link.rx_doorbell,
                            retained ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0,
                            (retained ? 0u : 1u) | (!r->app_reply_pending ? 2u : 0u));
    }
}
