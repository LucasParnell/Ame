#include "resources.h"

namespace {
using mochi::apps::Resources;
using mochi::apps::wipe;
struct Runtime {
    mochi::apps::Session<Resources> session;
    mochi::SpscEndpoint links[2]; // USB router, msh control; common wake doorbell
    midix::debug::Parser parser;
    MxFunctionChunk chunk;
    mx_u32_t position, sequence, reply_offset, reply_size;
    mx_u64_t generation;
    mx_u8_t reply[40];
    MxRingSlot control_reply;
    bool held, configured, control_pending;
};
bool service(Runtime& r) {
    using namespace midix::apps;
    const auto now = mx_clock_ns();
    const auto before = r.session.state;
    r.session.resources.drain_logs();
    r.session.tick(now);
    bool progress = before != r.session.state;
    if (r.control_pending) {
        if (r.links[1].tx().try_push_frame(r.control_reply)) {
            r.control_pending = false; progress = true;
        }
    } else {
        MxRingSlot control{};
        if (r.links[1].try_recv(&control)) {
            Control request{};
            if (mochi::message_get(control, &request, control.type)) {
                mx_status_t status = MX_OK;
                if (control.type == kStart) status = r.session.start(now);
                else if (control.type == kStop) r.session.cancel(true);
                else if (control.type != kStatus) status = MX_ERR_BAD_PROTOCOL;
                Reply reply{request.token, r.session.id, static_cast<mx_i32_t>(status), r.session.exit_code,
                            r.session.state, r.session.received};
                mochi::message_put(&r.control_reply, kReply, reply);
                r.control_pending = true;
            }
            progress = true;
        }
    }
    if (r.reply_offset < r.reply_size) {
        const auto count = (r.reply_size - r.reply_offset < 32 ? r.reply_size - r.reply_offset : 32);
        MxFunctionChunk reply{r.generation, ++r.sequence, static_cast<mx_u16_t>(count), 0, 0, {}};
        libk::copy_n(libk::as_bytes(reply.data), libk::as_bytes(r.reply + r.reply_offset, count), count);
        if (r.links[0].tx().try_push_msg(MX_FUNCTION_TX, reply)) {
            wipe(r.reply + r.reply_offset, count);
            r.reply_offset += count; progress = true;
        }
        return progress;
    }
    if (!r.held) {
        MxRingSlot slot{};
        if (!r.links[0].try_recv(&slot)) return progress;
        progress = true;
        if (mochi::message_get(slot, &r.chunk, MX_FUNCTION_RESET)) {
            if (!(r.chunk.flags & MX_FUNCTION_CONFIGURED) || r.generation) r.session.disconnect();
            r.parser.reset(); libk::zero(r.parser.bytes);
            r.generation = r.chunk.generation;
            r.configured = (r.chunk.flags & MX_FUNCTION_CONFIGURED) != 0;
            return progress;
        }
        if (!mochi::message_get(slot, &r.chunk, MX_FUNCTION_RX) || !r.configured ||
            r.chunk.generation != r.generation || r.chunk.flags || r.chunk.length > 32) return progress;
        r.position = 0; r.held = true;
    }
    while (r.position < r.chunk.length) {
        const auto byte = r.chunk.data[r.position]; r.chunk.data[r.position++] = 0;
        if (r.parser.push(byte)) {
            auto* frame = r.parser.bytes;
            const auto type = libk::load_le<mx_u16_t>(frame + 8);
            const auto seq = libk::load_le<mx_u32_t>(frame + 4);
            auto status = seq ? r.session.request(type,
                libk::as_bytes(frame + 16, libk::load_le<mx_u16_t>(frame + 10)), r.generation, now)
                              : MX_ERR_INVALID;
            libk::copy_n(libk::as_bytes(r.reply), libk::as_bytes(frame, 12), 12);
            libk::write_le<mx_u16_t>(libk::as_bytes(r.reply + 8, 2), type | midix::debug::kAck);
            libk::write_le<mx_u16_t>(libk::as_bytes(r.reply + 10, 2), 24);
            libk::write_le<mx_u32_t>(libk::as_bytes(r.reply + 16, 4), static_cast<mx_u32_t>(status));
            libk::write_le<mx_u32_t>(libk::as_bytes(r.reply + 20, 4), r.session.state);
            libk::write_le<mx_u32_t>(libk::as_bytes(r.reply + 24, 4), r.session.received);
            libk::write_le<mx_u32_t>(libk::as_bytes(r.reply + 28, 4), static_cast<mx_u32_t>(r.session.exit_code));
            libk::write_le<mx_u64_t>(libk::as_bytes(r.reply + 32, 8), r.session.id);
            libk::write_le<mx_u32_t>(libk::as_bytes(r.reply + 12, 4),
                                   midix::debug::checksum(libk::as_bytes(r.reply)));
            r.reply_size = 40; r.reply_offset = 0;
            r.parser.reset(); wipe(frame, sizeof(r.parser.bytes));
            return true;
        }
    }
    r.held = false;
    return true;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "dev.apps");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) mochi::boot::invalid_boot();
    r->session.exit_code = midix::apps::kNoExit;
    r->session.resources.supervisor = mochi::boot::require_handle(boot, "apps.supervisor");
    r->session.resources.cpu_count = mochi::driver::value(boot, "cpu.count", 1);
    r->session.resources.cpu_mask = mochi::driver::value(boot, "cpu.mask", 1);
    r->links[0] = mochi::driver::link(boot, "stream", mochi::va::native_driver::kLinks);
    r->links[1] = mochi::driver::link(boot, "control", mochi::va::native_driver::kLinks + 0x20000);
    if (!r->links[0].valid() || !r->links[1].valid()) mochi::boot::invalid_boot();
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return service(*r); })) { mx_yield(); continue; }
        const auto state = r->session.state;
        const bool active = state == midix::apps::Receiving || state == midix::apps::Running ||
                            state == midix::apps::Stopping || r->held || r->control_pending ||
                            r->reply_offset < r->reply_size;
        mochi::driver::idle(r->links, 2, r->links[0].rx_doorbell,
            active ? mx_clock_ns() + 1'000'000 : 0,
            (!r->held && r->reply_offset == r->reply_size ? 1u : 0u) |
                                     (!r->control_pending ? 2u : 0u));
    }
}
