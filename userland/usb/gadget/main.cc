// Standalone gadget process: construction and event-loop ownership only.
// EP0 policy and function routing live in control.h; FIFO execution belongs
// to the controller, CDC requests and diagnostic framing to their functions.
#include "control.h"
#include "mochi/usb/link.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "io.usb.gadget");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 32768);
    auto* r = arena.alloc<mochi::usb::gadget::Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->links[0] = mochi::driver::link(boot, "device", mochi::va::native_driver::kLinks);
    r->links[1] = mochi::driver::link(
        boot, "function.0",
        (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    r->links[2] = mochi::driver::link(
        boot, "function.1",
        (mochi::va::native_driver::kLinks + 2 * mochi::va::native_driver::kLinkStride));
    r->links[3] = mochi::driver::link(
        boot, "function.2", mochi::va::native_driver::kLinks + 3 * mochi::va::native_driver::kLinkStride);
    r->fido2_demand = mochi::boot::map_ring(boot, {"fido2.demand.ring",
        mochi::va::native_driver::kLinks + 4 * mochi::va::native_driver::kLinkStride, "fido2.demand.db"});
    r->acm_control = mochi::driver::link(boot, "acm.control",
        mochi::va::native_driver::kLinks + 5 * mochi::va::native_driver::kLinkStride);
    r->payload =
        mochi::usb::map_payload(boot, "device.payload", mochi::va::native_driver::kPayload);
    r->next_id = mx_clock_ns();
    r->function_generation = r->next_id | 1;
    if (!r->payload.data || r->payload.size < 8192 || !r->links[0].valid() ||
        !r->links[1].valid() || !r->links[2].valid() || !r->links[3].valid() || !r->acm_control.valid() || !r->fido2_demand.valid()) {
        mochi::boot::invalid_boot();
    }
    mochi::SpscEndpoint readers[5]{};
    for (unsigned i=0;i<4;i++) readers[i]=r->links[i];
    readers[4]=r->acm_control;
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return mochi::usb::gadget::service(*r); })) {
            mx_yield();
            continue;
        }
        const auto deadline = mochi::usb::gadget::retry_pending(*r)
                                  ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs
                                  : 0;
        mochi::driver::idle(readers, 5, r->links[0].rx_doorbell, deadline,
                            mochi::usb::gadget::readable_mask(*r) | (mx_u64_t{1} << 4));
    }
}
