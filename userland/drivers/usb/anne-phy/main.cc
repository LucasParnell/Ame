// Hi6250 board PHY sequencing, separate from the DesignWare core. Delays are
// minimum settling times in the pinned vendor source, never busy waits.
#include "mochi/driver.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/platform.h"
#include "midix/abi/protocols/board_usb.h"
#include "libk/dma.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint control;
    libk::Mmio ahb;
    mx_handle_t authority;
    MxBoardReply reply;
    MxUsbRole role, desired;
    mx_u64_t next, deadline;
    mx_u32_t step;
    bool busy, pending;
};
bool operation(Runtime& r, mx_u32_t op, mx_u32_t arg = 0) {
    return mx_platform_control(r.authority, op, arg) == MX_OK;
}
bool update(Runtime& r, mx_u32_t reg, mx_u32_t mask, mx_u32_t value) {
    mx_u32_t old = 0;
    return r.ahb.read32(reg, &old) && r.ahb.write32(reg, (old & ~mask) | value);
}
bool advance(Runtime& r) {
    if (!r.busy || mx_clock_ns() < r.next) {
        return true;
    }
    bool ok = mx_clock_ns() < r.deadline;
    mx_u64_t delay = 0;
    if (!ok) {
        r.reply.status = MX_ERR_FAULT;
        r.busy = false;
        r.pending = true;
        return false;
    }
    switch (r.step++) {
    case 0:
        ok = operation(r, MX_PLATFORM_RESET_ASSERT, MX_ANNE_USB_RESETS);
        delay = 1000;
        break;
    case 1:
        if (r.desired == MxUsbRole::Detached) {
            ok = operation(r, MX_PLATFORM_USB_CLOCK_OFF);
            r.step = 7;
        } else {
            const auto reference = mx_platform_control(r.authority, MX_PLATFORM_USB_REFERENCE);
            if (reference == MX_ERR_BUSY) {
                --r.step;
                r.next = mx_clock_ns() + 1000000;
                return true;
            }
            ok = reference == MX_OK && operation(r, MX_PLATFORM_USB_CLOCK_ON) &&
                 operation(r, MX_PLATFORM_RESET_RELEASE,
                           (1u << 10) | (1u << 11) | (1u << 14) | (1u << 27));
        }
        delay = 1000000;
        break;
    case 2:
        ok = update(r, 0, (3u << 4) | (1u << 6) | (1u << 2) | (1u << 3),
                    (1u << 2) | (r.desired == MxUsbRole::Device ? (1u << 6) : 0)) &&
             r.ahb.write32(0xc, r.desired == MxUsbRole::Host ? 0x073f679f : 0x06b866db) &&
             operation(r, MX_PLATFORM_RESET_RELEASE, 1u << 13);
        delay = 50000;
        break;
    case 3:
        ok = operation(r, MX_PLATFORM_RESET_RELEASE, 1u << 12);
        delay = 100000;
        break;
    case 4:
        ok = operation(r, MX_PLATFORM_RESET_RELEASE, (1u << 8) | (1u << 9));
        delay = 100000;
        break;
    case 5:
        ok = update(r, 8, (1u << 2) | (1u << 3), (1u << 2) | (1u << 3));
        delay = 1000000;
        break;
    default:
        r.role = r.desired;
        r.reply.value = static_cast<mx_u32_t>(r.role);
        r.busy = false;
        r.pending = true;
        break;
    }
    libk::dma::order_mmio();
    r.next = mx_clock_ns() + delay;
    if (!ok) {
        (void)operation(r, MX_PLATFORM_RESET_ASSERT, MX_ANNE_USB_RESETS);
        r.role = MxUsbRole::Detached;
        r.reply.status = MX_ERR_FAULT;
        r.busy = false;
        r.pending = true;
    }
    return ok;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "drv.usb.anne-phy");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->control = mochi::driver::link(boot, "phy", mochi::va::native_driver::kLinks);
    r->ahb = mochi::driver::registers(boot, "usb.phy.mmio", mochi::va::native_driver::kPayload);
    r->authority = mochi::boot::handle(boot, "usb.phy.control");
    if (!r->control.valid() || !r->ahb.base || !operation(*r, MX_PLATFORM_CLAIM)) {
        mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        (void)advance(*r);
        if (r->pending && r->control.tx().try_push_msg(MX_PHY_REPLY, r->reply)) {
            r->pending = false;
        }
        MxRingSlot slot{};
        MxBoardRequest request{};
        if (!r->busy && !r->pending && r->control.try_recv(&slot) &&
            mochi::message_get(slot, &request, MX_PHY_SET)) {
            r->reply = {request.id, MX_OK, 0, 0, 0};
            if (request.reserved || request.value > 2) {
                r->reply.status = MX_ERR_INVALID;
                r->pending = true;
            } else {
                r->desired = static_cast<MxUsbRole>(request.value);
                r->step = 0;
                r->next = 0;
                r->deadline = mx_clock_ns() + 1000000000ull;
                r->busy = true;
            }
        }
        mochi::driver::idle(&r->control, 1, r->control.rx_doorbell,
                            r->busy ? r->next : mx_clock_ns() + 1000000000);
    }
}
