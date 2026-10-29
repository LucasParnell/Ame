// One DesignWare core owns mutually exclusive host/device register banks.
// Board preparation and role selection arrive through a separate cold link.
#include "host.h"
#include "device.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "libk/printf.h"
#include "midix/abi/platform.h"
#include "midix/abi/protocols/board_usb.h"
namespace {
using namespace mochi::usb::dwc2;
enum class TransitionStep : mx_u8_t { Quiescing, Resetting, WaitingMode, Complete };
struct Runtime {
    libk::Mmio mmio;
    Registers registers;
    Geometry geometry;
    Reset reset;
    Host host;
    Device device;
    mochi::SpscEndpoint links[3];
    MxUsbRole role, desired;
    MxBoardReply reply;
    mx_u64_t deadline;
    TransitionStep stage;
    mx_u32_t fault_stage;
    mx_handle_t irq, wake;
    bool busy, pending;
};
bool transition(Runtime& r) {
    if (!r.busy) {
        return true;
    }
    const auto now = mx_clock_ns();
    if (now >= r.deadline) {
        r.fault_stage = 99;
        return false;
    }
    if (r.stage == TransitionStep::Quiescing) {
        if (r.role == MxUsbRole::Host && !r.host.quiesce()) {
            return true;
        }
        if (r.role == MxUsbRole::Device && !r.device.quiesce()) return true;
        if (r.role == MxUsbRole::Detached && r.desired == MxUsbRole::Detached) {
            r.stage = TransitionStep::Complete;
        } else {
            if (r.desired != MxUsbRole::Detached) {
                if (!identify(r.registers, &r.geometry)) {
                    r.fault_stage = 20;
                    return false;
                }
                if (!configure_utmi(r.registers, r.geometry, r.desired == MxUsbRole::Host)) {
                    r.fault_stage = 21;
                    return false;
                }
            }
            if (!r.reset.begin(r.registers, r.deadline)) {
                r.fault_stage = 22;
                return false;
            }
            r.stage = TransitionStep::Resetting;
        }
    }
    if (r.stage == TransitionStep::Resetting) {
        const auto state = r.reset.advance(r.registers, now);
        if (state == ResetState::Fault) {
            r.fault_stage = 30;
            return false;
        }
        if (state != ResetState::Ready) {
            return true;
        }
        // CSFTRST can restore timing defaults. Reapply the selected bus
        // configuration before the role executor exposes any endpoints.
        if (r.desired != MxUsbRole::Detached &&
            !configure_utmi(r.registers, r.geometry, r.desired == MxUsbRole::Host)) {
            r.fault_stage = 31;
            return false;
        }
        r.stage = TransitionStep::WaitingMode;
    }
    if (r.stage == TransitionStep::WaitingMode) {
        mx_u32_t mode = 0;
        r.registers.read(r.registers.context, kInterruptStatus, &mode);
        if (r.desired == MxUsbRole::Host && !(mode & kHostMode)) {
            return true;
        }
        if (r.desired == MxUsbRole::Device && (mode & kHostMode)) {
            return true;
        }
        if (r.desired == MxUsbRole::Host && !r.host.activate(r.registers, r.geometry)) {
            r.fault_stage = 40;
            return false;
        }
        if (r.desired == MxUsbRole::Device && !r.device.activate(r.registers, r.geometry)) {
            r.fault_stage = 41;
            return false;
        }
        r.stage = TransitionStep::Complete;
    }
    if (r.stage == TransitionStep::Complete) {
        r.role = r.desired;
        r.busy = false;
        r.pending = true;
        r.reply.value = static_cast<mx_u32_t>(r.role);
    }
    return true;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "drv.usb.dwc2");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 256 * 1024);
    auto* r = arena.alloc<Runtime>();
    const auto fence = mochi::boot::handle(boot, "usb.fence");
    if (!r || mx_platform_control(fence, MX_PLATFORM_CLAIM) != MX_OK) {
        mochi::boot::invalid_boot();
    }
    r->mmio = mochi::driver::registers(boot, "dwc2.mmio", mochi::va::native_driver::kRegisters);
    r->registers = Registers::mapped(r->mmio);
    r->links[0] = mochi::usb::map_link(boot, true, mochi::va::native_driver::kLinks,
                                       (mochi::va::native_driver::kLinks + 0x10000));
    r->links[1] = mochi::driver::link(
        boot, "role", (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    r->links[2] = mochi::driver::link(
        boot, "device",
        (mochi::va::native_driver::kLinks + 2 * mochi::va::native_driver::kLinkStride));
    auto payload =
        mochi::usb::map_payload(boot, "usb.controller.payload", mochi::va::native_driver::kPayload);
    r->device.payload =
        mochi::usb::map_payload(boot, "device.payload", mochi::va::native_driver::kDevicePayload);
    r->device.link = r->links[2];
    r->wake = mochi::boot::handle(boot, "usb.controller.rx.db");
    r->irq = mochi::boot::handle(boot, "dwc2.irq");
    if (!r->mmio.base || !r->links[1].valid() || !r->links[2].valid() ||
        !r->host.init(arena, r->links[0], payload) || !r->device.payload.data ||
        mx_irq_bind_doorbell(r->irq, r->wake, 0) != MX_OK) {
        mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        const auto expected = mx_doorbell_read(r->wake);
        r->host.poll();
        r->device.poll();
        if (!transition(*r)) {
            char line[128];
            const int length = libk::snprintf(
                line, sizeof(line),
                "drv.usb.dwc2: transition fault stage=%u reason=%u desired=%u\n",
                static_cast<unsigned>(r->stage), static_cast<unsigned>(r->fault_stage),
                static_cast<unsigned>(r->desired));
            if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
                mochi::log_raw_marker(line);
            }
            mx_exit(MX_ERR_FAULT);
        }
        if (r->pending && r->links[1].tx().try_push_msg(MX_ROLE_REPLY, r->reply)) {
            r->pending = false;
        }
        MxRingSlot slot{};
        MxBoardRequest request{};
        if (!r->busy && !r->pending && r->links[1].try_recv(&slot) &&
            (mochi::message_get(slot, &request, MX_ROLE_SET) ||
             mochi::message_get(slot, &request, MX_ROLE_QUERY))) {
            r->reply = {request.id, MX_OK, 0, 0, 0};
            if (slot.type == MX_ROLE_QUERY) {
                if (request.reserved || request.value) r->reply.status = MX_ERR_INVALID;
                r->reply.value = static_cast<mx_u32_t>(r->host.sequence);
                r->reply.flags = (r->host.active ? MX_ROLE_HOST_ACTIVE : 0u) |
                                  (r->host.connected ? MX_ROLE_DATA_CONNECTED : 0u);
                r->pending = true;
            } else if (request.reserved || request.value > 2 ||
                (r->role != MxUsbRole::Detached && request.value)) {
                r->reply.status = MX_ERR_BUSY;
                r->pending = true;
            } else {
                r->desired = static_cast<MxUsbRole>(request.value);
                r->busy = true;
                r->stage = TransitionStep::Quiescing;
                r->deadline = mx_clock_ns() + 2000000000ull;
            }
        }
        // Owned device sources are drained before rearming this level IRQ.
        if (r->role != MxUsbRole::Detached) {
            (void)mx_irq_ack(r->irq);
        }
        const auto now = mx_clock_ns();
        mx_u64_t deadline = r->busy ? now + 100000ull : 0;
        deadline = libk::earlier_deadline(deadline, r->host.wake_deadline(now));
        deadline = libk::earlier_deadline(deadline, r->device.wake_deadline(now));
        if (r->pending) {
            deadline = libk::earlier_deadline(deadline, now + mochi::driver::kBackpressureRetryNs);
        }
        // Blocked cold requests stay queued. They must not turn the reader
        // recheck into a busy loop while reset/addressing/publication runs.
        const mx_u64_t readable = (!r->host.reply_pending && !r->host.addressing ? 1u : 0u) |
                                  (!r->busy && !r->pending ? 2u : 0u) |
                                  (!r->device.event_pending ? 4u : 0u);
        mochi::driver::idle_on_counter(r->links, 3, r->wake, expected, deadline, readable);
    }
}
