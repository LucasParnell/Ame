// CDC ACM owns line coding/control requests and an opaque byte stream. It
// does not interpret key usages, commands, debug framing or terminal state.
#include "mochi/driver.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/usb_function.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint links[2];
    MxRingSlot pending[2];
    bool held[2];
    mx_u8_t coding[7];
    mx_u16_t control;
};
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
    libk::write_le<mx_u32_t>(libk::as_bytes(r->coding, sizeof(mx_u32_t)), 115200);
    r->coding[6] = 8;
    if (!r->links[0].valid() || !r->links[1].valid()) {
        mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        for (mx_u32_t from = 0; from < 2; ++from) {
            const auto to = from ^ 1;
            if (r->held[from]) {
                const auto& slot = r->pending[from];
                if (r->links[to].tx().try_push(slot.type, slot.payload, slot.length)) {
                    r->held[from] = false;
                }
                continue;
            }
            if (!from && r->held[1]) {
                continue;
            }
            MxRingSlot slot{};
            if (!r->links[from].try_recv(&slot)) {
                continue;
            }
            if (!from && slot.type == MX_FUNCTION_SETUP) {
                MxFunctionSetup req{};
                if (!mochi::message_get(slot, &req, slot.type)) {
                    continue;
                }
                req.status = MX_ERR_NOT_IMPLEMENTED;
                if (req.setup.index == 0) {
                    if (req.setup.request_type == 0xa1 && req.setup.request == 0x21 &&
                        req.setup.length == 7) {
                        libk::copy_n(libk::as_bytes(req.data), libk::as_bytes(r->coding), 7);
                        req.length = 7;
                        req.status = MX_OK;
                    } else if (req.setup.request_type == 0x21 && req.setup.request == 0x20 &&
                               req.setup.length == 7 && req.length == 7) {
                        if (req.data[4] <= 2 && req.data[5] <= 4 &&
                            (req.data[6] == 5 || req.data[6] == 6 || req.data[6] == 7 ||
                             req.data[6] == 8)) {
                            libk::copy_n(libk::as_bytes(r->coding), libk::as_bytes(req.data, 7), 7);
                            req.length = 0;
                            req.status = MX_OK;
                        }
                    } else if (req.setup.request_type == 0x21 && !req.setup.length &&
                               (req.setup.request == 0x22 || req.setup.request == 0x23)) {
                        r->control = req.setup.value;
                        req.length = 0;
                        req.status = MX_OK;
                    }
                }
                // Reply travels back to the gadget, with retained backpressure.
                mochi::message_put(&r->pending[1], MX_FUNCTION_SETUP_REPLY, req);
                r->held[1] = true;
            } else if (slot.type == MX_FUNCTION_RX || slot.type == MX_FUNCTION_TX ||
                       slot.type == MX_FUNCTION_RESET) {
                r->pending[from] = slot;
                r->held[from] = true;
            }
        }
        mochi::driver::idle(r->links, 2, r->links[0].rx_doorbell, mx_clock_ns() + 10000000);
    }
}
