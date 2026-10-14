// Standalone raw serial service. Mapping and wake ownership live here;
// bridge.h owns bounded byte/session forwarding. CDC policy stays in usb.fn.acm.
#include "bridge.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
using mochi::usb::serial::Runtime;
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "dev.serial");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->stream = mochi::driver::link(boot, "stream", mochi::va::native_driver::kLinks);
    r->input = {mochi::boot::map_ring(
        boot, {"serial.in.ring",
               (mochi::va::native_driver::kLinks + 2 * mochi::va::native_driver::kLinkStride),
               "serial.in.doorbell"})};
    r->output = {mochi::boot::map_ring(
        boot, {"serial.out.ring",
               (mochi::va::native_driver::kLinks + 3 * mochi::va::native_driver::kLinkStride),
               "serial.out.doorbell"})};
    if (!r->stream.valid() || !r->input.valid() || !r->output.valid()) {
        mochi::boot::invalid_boot();
    }
    // Both producers notify this same doorbell; views are built once.
    mochi::SpscEndpoint readers[2]{};
    readers[0] = r->stream;
    readers[1].rx_ring = r->output.ring;
    readers[1].order = r->output.order;
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return mochi::usb::serial::service(*r); })) {
            mx_yield();
            continue;
        }
        const auto deadline = mochi::usb::serial::retained(*r)
                                  ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs
                                  : 0;
        mochi::driver::idle(readers, 2, r->stream.rx_doorbell, deadline,
                            mochi::usb::serial::readable_mask(*r));
    }
}
