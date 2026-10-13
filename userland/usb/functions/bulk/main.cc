// Generic vendor bulk function: two opaque, independently backpressured byte
// directions. Diagnostic framing and keyboard input live in dev.usb-input.
#include "mochi/driver.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/usb_function.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint links[2];
    MxRingSlot slots[2];
    bool pending[2];
};
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
        for (mx_u32_t i = 0; i < 2; ++i) {
            if (r->pending[i]) {
                const auto& s = r->slots[i];
                if (r->links[i ^ 1].tx().try_push(s.type, s.payload, s.length)) {
                    r->pending[i] = false;
                }
            } else if (r->links[i].try_recv(&r->slots[i])) {
                const auto type = r->slots[i].type;
                r->pending[i] =
                    type == MX_FUNCTION_RX || type == MX_FUNCTION_TX || type == MX_FUNCTION_RESET;
            }
        }
        mochi::driver::idle(r->links, 2, r->links[0].rx_doorbell, mx_clock_ns() + 10000000);
    }
}
