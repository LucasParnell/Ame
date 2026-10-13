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
    mx_u32_t observed_value, observed_flags;
    bool query_fault_logged;
};
struct CallOutcome {
    bool ok, replied;
    mx_status_t status;
};
CallOutcome call(Runtime& r, mx_u32_t link, mx_u32_t type, mx_u32_t response, mx_u32_t value,
                 MxBoardReply* out = nullptr) {
    CallOutcome result{false, false, MX_OK};
    const MxBoardRequest request{++r.sequence, value, 0};
    const auto deadline = mx_clock_ns() + 3000000000ull;
    if (!r.links[link].tx().push_bounded(type, &request, sizeof(request), deadline)) {
        return result;
    }
    MxRingSlot slot{};
    MxBoardReply reply{};
    while (mochi::driver::receive(r.links[link], slot, deadline)) {
        if (mochi::message_get(slot, &reply, response) && reply.id == request.id) {
            if (out) {
                *out = reply;
            }
            result.replied = true;
            result.status = reply.status;
            result.ok = reply.status == MX_OK;
            return result;
        }
    }
    return result;
}
struct Transition {
    bool ok, replied;
    const char* stage;
    mx_status_t status;
};
Transition change(Runtime& r, MxUsbRole next) {
    Transition result{false, false, "role-quiesce", MX_OK};
    CallOutcome outcome = call(r, 3, MX_ROLE_SET, MX_ROLE_REPLY, 0);
    if (!outcome.ok) {
        result.replied = outcome.replied;
        result.status = outcome.status;
        return result;
    }
    result.stage = "power-off";
    outcome = call(r, 1, MX_POWER_SET, MX_POWER_REPLY, 0);
    if (!outcome.ok) {
        result.replied = outcome.replied;
        result.status = outcome.status;
        return result;
    }
    result.stage = "phy-set";
    outcome = call(r, 2, MX_PHY_SET, MX_PHY_REPLY, static_cast<mx_u32_t>(next));
    if (!outcome.ok) {
        result.replied = outcome.replied;
        result.status = outcome.status;
        return result;
    }
    if (next == MxUsbRole::Host) {
        result.stage = "power-on";
        outcome = call(r, 1, MX_POWER_SET, MX_POWER_REPLY, 500);
        if (!outcome.ok) {
            result.replied = outcome.replied;
            result.status = outcome.status;
            return result;
        }
    }
    if (next != MxUsbRole::Detached) {
        result.stage = "role-set";
        outcome = call(r, 3, MX_ROLE_SET, MX_ROLE_REPLY, static_cast<mx_u32_t>(next));
        if (!outcome.ok) {
            result.replied = outcome.replied;
            result.status = outcome.status;
            return result;
        }
    }
    r.role = next;
    result.ok = true;
    return result;
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
    r->observed_value = ~0u;
    mochi::boot::ready(boot);
    for (;;) {
        MxBoardReply status{};
        MxUsbRole next = MxUsbRole::Detached;
        const auto query = call(*r, 0, MX_TYPEC_QUERY, MX_TYPEC_REPLY, 0, &status);
        if (query.ok) {
            r->query_fault_logged = false;
            next = static_cast<MxUsbRole>(status.value);
            if (next == MxUsbRole::Device && !(status.flags & MX_TYPEC_VBUS)) {
                next = MxUsbRole::Detached;
            }
            if (status.value != r->observed_value || status.flags != r->observed_flags) {
                r->observed_value = status.value;
                r->observed_flags = status.flags;
                char line[96];
                const int length = libk::snprintf(line, sizeof(line), "typec role=%u flags=0x%x\n",
                                                  static_cast<unsigned>(status.value),
                                                  static_cast<unsigned>(status.flags));
                if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
                    mochi::log_raw_marker(line);
                }
            }
        } else if (!r->query_fault_logged) {
            r->query_fault_logged = true;
            mochi::log_raw_marker("typec query failed\n");
        }
        if (next != r->role) {
            const auto transition = change(*r, next);
            if (!transition.ok) {
                (void)call(*r, 1, MX_POWER_SET, MX_POWER_REPLY, 0);
                (void)call(*r, 3, MX_ROLE_SET, MX_ROLE_REPLY, 0);
                (void)call(*r, 2, MX_PHY_SET, MX_PHY_REPLY, 0);
                r->role = MxUsbRole::Detached;
                r->fault = true;
                char line[160];
                const int length =
                    transition.replied
                        ? libk::snprintf(line, sizeof(line),
                                         "transition fault at %s: status=%d; source disabled\n",
                                         transition.stage, static_cast<int>(transition.status))
                        : libk::snprintf(line, sizeof(line),
                                         "transition fault at %s: no reply; source disabled\n",
                                         transition.stage);
                if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
                    mochi::log_raw_marker(line);
                }
            } else {
                mochi::log_raw_marker(next == MxUsbRole::Host     ? "io.usb.role: host\n"
                                      : next == MxUsbRole::Device ? "io.usb.role: device\n"
                                                                  : "io.usb.role: detached\n");
            }
        }
        if (r->role == MxUsbRole::Host) {
            const auto keep = call(*r, 1, MX_POWER_SET, MX_POWER_REPLY, 500);
            if (!keep.ok) {
                (void)change(*r, MxUsbRole::Detached);
            }
        }
        // Polling the CC chip is intentional until its GPIO IRQ service exists.
        // No controller, HID or terminal process polls board registers.
        mochi::driver::idle(r->links, 4, r->links[0].rx_doorbell, mx_clock_ns() + 50000000);
    }
}
