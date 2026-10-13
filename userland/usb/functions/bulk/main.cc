// Generic vendor bulk function: two opaque, independently backpressured byte
// directions. Diagnostic framing and keyboard input live in dev.usb-input.
#include "mochi/usb/function_stream.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/usb_function.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint links[2];
    mochi::usb::PendingFrame pending[2];
};
bool service(Runtime& r) {
    bool progress = false;
    for (mx_u32_t i = 0; i < 2; ++i) {
        auto& pending = r.pending[i];
        if (pending.held) {
            progress |= pending.flush(r.links[i ^ 1].tx());
        } else {
            MxRingSlot slot{};
            if (r.links[i].try_recv(&slot)) {
                (void)pending.retain(slot);
                progress = true;
            }
        }
    }
    return progress;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "usb.fn.bulk");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->links[0] = mochi::driver::link(boot, "function", mochi::va::native_driver::kLinks);
    r->links[1] = mochi::driver::link(
        boot, "stream", (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    if (!r->links[0].valid() || !r->links[1].valid()) {
        mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return service(*r); })) {
            mx_yield();
            continue;
        }
        const bool blocked = r->pending[0].held || r->pending[1].held;
        const mx_u64_t readable = (!r->pending[0].held ? 1u : 0u) | (!r->pending[1].held ? 2u : 0u);
        mochi::driver::idle(r->links, 2, r->links[0].rx_doorbell,
                            blocked ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0,
                            readable);
    }
}
