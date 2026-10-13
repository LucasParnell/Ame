// Type-C policy orchestrator. Stop controller -> remove source power -> prepare
// PHY -> enable source only for partner UFP -> start controller. A powered Mac
// is partner DFP: only peripheral mode is permitted for that attachment.
#include "mochi/driver.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/board_usb.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint links[4];
    mx_u64_t sequence, next;
    MxUsbRole role;
    bool fault;
};
bool call(Runtime& r, mx_u32_t link, mx_u32_t type, mx_u32_t response, mx_u32_t value,
          MxBoardReply* out = nullptr) {
    const MxBoardRequest request{++r.sequence, value, 0};
    const auto deadline = mx_clock_ns() + 3000000000ull;
    if (!r.links[link].tx().push_bounded(type, &request, sizeof(request), deadline)) {
        return false;
    }
    MxRingSlot slot{};
    MxBoardReply reply{};
    while (mochi::driver::receive(r.links[link], slot, deadline)) {
        if (mochi::message_get(slot, &reply, response) && reply.id == request.id) {
            if (out) {
                *out = reply;
            }
            return reply.status == MX_OK;
        }
    }
    return false;
}
bool change(Runtime& r, MxUsbRole next) {
    if (!call(r, 3, MX_ROLE_SET, MX_ROLE_REPLY, 0)) {
        return false;
    }
    if (!call(r, 1, MX_POWER_SET, MX_POWER_REPLY, 0)) {
        return false;
    }
    if (!call(r, 2, MX_PHY_SET, MX_PHY_REPLY, static_cast<mx_u32_t>(next))) {
        return false;
    }
    if (next == MxUsbRole::Host && !call(r, 1, MX_POWER_SET, MX_POWER_REPLY, 500)) {
        return false;
    }
    if (next != MxUsbRole::Detached &&
        !call(r, 3, MX_ROLE_SET, MX_ROLE_REPLY, static_cast<mx_u32_t>(next))) {
        return false;
    }
    r.role = next;
    return true;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "io.usb.role");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    const char* names[] = {"typec", "power", "phy", "role"};
    for (mx_u32_t i = 0; i < 4; ++i) {
        r->links[i] =
            mochi::driver::link(boot, names[i], mochi::va::native_driver::kLinks + i * 0x20000);
        if (!r->links[i].valid()) {
            mochi::boot::invalid_boot();
        }
    }
    r->sequence = mx_clock_ns();
    mochi::boot::ready(boot);
    for (;;) {
        MxBoardReply status{};
        MxUsbRole next = MxUsbRole::Detached;
        if (call(*r, 0, MX_TYPEC_QUERY, MX_TYPEC_REPLY, 0, &status)) {
            next = static_cast<MxUsbRole>(status.value);
            if (next == MxUsbRole::Device && !(status.flags & MX_TYPEC_VBUS)) {
                next = MxUsbRole::Detached;
            }
        }
        if (next != r->role) {
            if (!change(*r, next)) {
                (void)call(*r, 1, MX_POWER_SET, MX_POWER_REPLY, 0);
                (void)call(*r, 3, MX_ROLE_SET, MX_ROLE_REPLY, 0);
                (void)call(*r, 2, MX_PHY_SET, MX_PHY_REPLY, 0);
                r->role = MxUsbRole::Detached;
                r->fault = true;
                mochi::log_raw_marker("io.usb.role: transition fault; source disabled\n");
            } else {
                mochi::log_raw_marker(next == MxUsbRole::Host     ? "io.usb.role: host\n"
                                      : next == MxUsbRole::Device ? "io.usb.role: device\n"
                                                                  : "io.usb.role: detached\n");
            }
        }
        if (r->role == MxUsbRole::Host && !call(*r, 1, MX_POWER_SET, MX_POWER_REPLY, 500)) {
            (void)change(*r, MxUsbRole::Detached);
        }
        // Polling the CC chip is intentional until its GPIO IRQ service exists.
        // No controller, HID or terminal process polls board registers.
        mochi::driver::idle(r->links, 4, r->links[0].rx_doorbell, mx_clock_ns() + 50000000);
    }
}
