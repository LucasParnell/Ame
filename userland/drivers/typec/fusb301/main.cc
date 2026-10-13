// FUSB301 reports the PARTNER type: DFP means our phone is a device;
// UFP means our phone is the host. Connector orientation is independent.
#include "mochi/drivers/i2c.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/board_usb.h"
namespace {
struct Runtime {
    mochi::i2c::Client bus;
    mochi::SpscEndpoint control;
    MxBoardReply reply;
    bool pending;
};
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "drv.typec.fusb301");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->bus = {mochi::driver::link(boot, "i2c", mochi::va::native_driver::kLinks), mx_clock_ns(),
              static_cast<mx_u16_t>(mochi::driver::value(boot, "i2c.address"))};
    r->control = mochi::driver::link(
        boot, "typec", (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    mx_u8_t id = 0;
    if (!r->control.valid() || !r->bus.endpoint.valid() || !r->bus.read(1, id) || !id ||
        id == 255 || !r->bus.write(2, 1u << 4)) {
        mochi::boot::invalid_boot(); // DRP, never force DFP onto powered Mac
    }
    mochi::boot::ready(boot);
    for (;;) {
        if (r->pending && r->control.tx().try_push_msg(MX_TYPEC_REPLY, r->reply)) {
            r->pending = false;
        }
        MxRingSlot slot{};
        MxBoardRequest request{};
        if (!r->pending && r->control.try_recv(&slot) &&
            mochi::message_get(slot, &request, MX_TYPEC_QUERY)) {
            r->reply = {request.id, MX_OK, 0, 0, 0};
            const mx_u8_t reg = 0x11;
            mx_u8_t bytes[2]{};
            if (request.value || request.reserved ||
                !r->bus.transaction(libk::as_bytes(&reg, 1), libk::as_bytes(bytes))) {
                r->reply.status = MX_ERR_FAULT;
            } else if (bytes[0] & 1) {
                const auto orientation = (bytes[0] >> 4) & 3;
                const auto type = bytes[1] & 0x18;
                if (orientation == 0 || orientation == 3 || (type != 8 && type != 16)) {
                    r->reply.status = MX_ERR_INVALID;
                } else {
                    r->reply.value =
                        static_cast<mx_u32_t>(type == 8 ? MxUsbRole::Host : MxUsbRole::Device);
                    r->reply.flags = MX_TYPEC_ATTACHED | ((bytes[0] & 8) ? MX_TYPEC_VBUS : 0) |
                                     (orientation == 1 ? MX_TYPEC_CC1 : MX_TYPEC_CC2);
                }
            }
            r->pending = true;
        }
        mochi::driver::idle(&r->control, 1, r->control.rx_doorbell, mx_clock_ns() + 50000000);
    }
}
