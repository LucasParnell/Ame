// CDC ACM owns line coding/control requests and an opaque byte stream. It
// does not interpret key usages, commands, debug framing or terminal state.
#include "bridge.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/usb_function.h"
using mochi::usb::acm::Runtime;
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
    r->links[2] = mochi::driver::link(boot, "control",
        mochi::va::native_driver::kLinks + 2 * mochi::va::native_driver::kLinkStride);
    r->control.init();
    if (!r->links[0].valid() || !r->links[1].valid() || !r->links[2].valid()) {
        mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return mochi::usb::acm::service(*r); })) {
            mx_yield();
            continue;
        }
        const bool blocked = r->pending[0].held || r->pending[1].held || r->control_reply.held;
        const mx_u64_t readable = (!r->pending[0].held ? 1u : 0u) |
                                  (!r->pending[1].held ? 2u : 0u) | (!r->control_reply.held ? 4u : 0u);
        mochi::driver::idle(r->links, 3, r->links[0].rx_doorbell,
                            blocked ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0,
                            readable);
    }
}
