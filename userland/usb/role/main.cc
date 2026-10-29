// Type-C policy orchestrator. Remove source power -> stop controller -> prepare
// PHY -> recheck attachment -> optionally enable source -> start controller.
#include "mochi/driver.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/board_usb.h"
#include "mochi/usb/typec_policy.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint links[4];
    mx_u64_t sequence, renew_at;
    mochi::usb::typec::StableRole policy;
    mochi::usb::typec::SourceSession source;
    MxUsbRole role;
    bool fault;
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
    // A failed CC query must not hold host power through a multi-second wait.
    const auto deadline =
        mx_clock_ns() + (link == 0 || type == MX_ROLE_QUERY ? 250000000ull : 3000000000ull);
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
    bool attachment_changed = false;
};
Transition change(Runtime& r, MxUsbRole next, mx_u32_t cc = 0) {
    Transition result{false, false, "power-off", MX_OK};
    // Detach must cut boost even if the controller subsequently stalls.
    CallOutcome outcome = call(r, 1, MX_POWER_SET, MX_POWER_REPLY, 0);
    if (!outcome.ok) {
        result.replied = outcome.replied;
        result.status = outcome.status;
        return result;
    }
    if (r.role == MxUsbRole::Host && next != MxUsbRole::Host) {
        // CC can still describe the old sink while a replacement computer is
        // accepting our VBUS. Power is already off; explicitly restart Sink.
        result.stage = "sink-restart";
        outcome = call(r, 0, MX_TYPEC_RESTART_SINK, MX_TYPEC_REPLY, 0);
        if (!outcome.ok) {
            result.replied = outcome.replied;
            result.status = outcome.status;
            return result;
        }
    }
    result.stage = "role-quiesce";
    outcome = call(r, 3, MX_ROLE_SET, MX_ROLE_REPLY, 0);
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
        // PHY setup can take time. Never use an old CC reading to turn on
        // boost: the cable may now lead to a host supplying its own VBUS.
        result.stage = "source-check";
        MxBoardReply fresh{};
        outcome = call(r, 0, MX_TYPEC_QUERY, MX_TYPEC_REPLY, 0, &fresh);
        if (!outcome.ok || !mochi::usb::typec::can_start_source(fresh, cc)) {
            result.replied = outcome.replied;
            result.status = outcome.ok ? MX_ERR_INVALID : outcome.status;
            result.attachment_changed = true;
            return result;
        }
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
    if (next == MxUsbRole::Host) {
        r.source.begin(mx_clock_ns());
    }
    r.renew_at = mx_clock_ns() + 500000000ull;
    const auto* marker = next == MxUsbRole::Host     ? "usb role: host; source=500mA\n"
                         : next == MxUsbRole::Device ? "usb role: device; source off\n"
                                                     : "usb role: detached; source off\n";
    sys_debug_puts(marker, libk::str::length(marker));
    mochi::log_raw_marker(marker);
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
    const bool device_only = mochi::driver::value(boot, "usb.device_only", 1) != 0;
    mochi::boot::ready(boot);
    for (;;) {
        MxBoardReply status{};
        const auto query = call(*r, 0, MX_TYPEC_QUERY, MX_TYPEC_REPLY, 0, &status);
        if (query.ok) {
            r->query_fault_logged = false;
            // A failed boost/PHY/controller transition stays off until an
            // actual detach; do not repeatedly power-cycle a faulty partner.
            if (!status.value && !(status.flags & MX_TYPEC_ATTACHED)) {
                r->fault = false;
            }
        } else if (!r->query_fault_logged) {
            r->query_fault_logged = true;
            mochi::log_raw_marker("typec query failed\n");
        }
        auto next = r->policy.observe(status, query.ok, device_only, r->role, mx_clock_ns());
        if (r->role == MxUsbRole::Host) {
            MxBoardReply port{};
            const auto observed = call(*r, 3, MX_ROLE_QUERY, MX_ROLE_REPLY, 0, &port);
            if (!r->source.keep(port, observed.ok, mx_clock_ns())) {
                next = MxUsbRole::Detached;
                r->policy.reset();
                char line[160];
                const int length =
                    libk::snprintf(line, sizeof(line),
                                   "usb role: data connection lost; restart sink flags=%x epoch=%u "
                                   "expected=%u seen=%u query=%u\n",
                                   port.flags, port.value, r->source.generation,
                                   r->source.seen ? 1u : 0u, observed.ok ? 1u : 0u);
                if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
                    mochi::log_raw_marker(line);
                }
            }
        }
        if (r->fault) {
            next = MxUsbRole::Detached;
        }
        if (next != r->role) {
            const auto transition =
                change(*r, next, status.flags & mochi::usb::typec::kOrientation);
            if (!transition.ok) {
                (void)call(*r, 1, MX_POWER_SET, MX_POWER_REPLY, 0);
                (void)call(*r, 3, MX_ROLE_SET, MX_ROLE_REPLY, 0);
                (void)call(*r, 2, MX_PHY_SET, MX_PHY_REPLY, 0);
                r->role = MxUsbRole::Detached;
                r->policy.reset();
                r->fault = !transition.attachment_changed;
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
            }
        }
        // The source lease lasts two seconds. Renew with ample margin, not
        // on every 50 ms CC observation; power fault checks remain independent.
        if (r->role == MxUsbRole::Host && mx_clock_ns() >= r->renew_at) {
            const auto keep = call(*r, 1, MX_POWER_SET, MX_POWER_REPLY, 500);
            if (!keep.ok) {
                (void)change(*r, MxUsbRole::Detached);
                r->role = MxUsbRole::Detached;
                r->policy.reset();
                r->fault = true;
            } else {
                r->renew_at = mx_clock_ns() + 500000000ull;
            }
        }
        // Polling the CC chip is intentional until its GPIO IRQ service exists.
        // No controller, HID or terminal process polls board registers.
        mochi::driver::idle(r->links, 4, r->links[0].rx_doorbell, mx_clock_ns() + 50000000);
    }
}
