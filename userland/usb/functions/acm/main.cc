// CDC ACM owns line coding/control requests and an opaque byte stream. It
// does not interpret key usages, commands, debug framing or terminal state.
#include "control.h"
#include "mochi/usb/function_stream.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/usb_function.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint links[2];
    mochi::usb::PendingFrame pending[2], control_reply;
    mochi::usb::acm::Control control;
};
// Control replies have their own retained row, so full byte-stream output
// cannot prevent accepting lifecycle/control input from the gadget. One SPSC
// producer still owns the gadget reply ring; control is published first.
bool service(Runtime& r) {
    bool progress = r.control_reply.flush(r.links[0].tx());
    for (mx_u32_t from = 0; from < 2; ++from) {
        auto& pending = r.pending[from];
        if (pending.held) {
            if (!from || !r.control_reply.held) {
                progress |= pending.flush(r.links[from ^ 1].tx());
            }
            continue;
        }
        if (!from && r.control_reply.held) {
            continue;
        }
        MxRingSlot slot{};
        if (!r.links[from].try_recv(&slot)) {
            continue;
        }
        progress = true;
        if (!from && slot.type == MX_FUNCTION_SETUP) {
            MxFunctionSetup request{};
            if (!mochi::message_get(slot, &request, slot.type)) {
                continue;
            }
            r.control.reply(request);
            mochi::message_put(&r.control_reply.slot, MX_FUNCTION_SETUP_REPLY, request);
            r.control_reply.slot.seq = slot.seq;
            r.control_reply.held = true;
        } else {
            (void)pending.retain(slot);
        }
    }
    return progress;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "usb.fn.acm");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->links[0] = mochi::driver::link(boot, "function", mochi::va::native_driver::kLinks);
    r->links[1] = mochi::driver::link(
        boot, "stream", (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    r->control.init();
    if (!r->links[0].valid() || !r->links[1].valid()) {
        mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return service(*r); })) {
            mx_yield();
            continue;
        }
        const bool blocked = r->pending[0].held || r->pending[1].held || r->control_reply.held;
        const mx_u64_t readable = (!r->pending[0].held && !r->control_reply.held ? 1u : 0u) |
                                  (!r->pending[1].held ? 2u : 0u);
        mochi::driver::idle(r->links, 2, r->links[0].rx_doorbell,
                            blocked ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0,
                            readable);
    }
}
