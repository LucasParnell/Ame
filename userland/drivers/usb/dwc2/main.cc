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
    mx_u32_t stage;
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
    if (r.stage == 0) {
        if (r.role == MxUsbRole::Host && !r.host.quiesce()) {
            return true;
        }
        if (r.role == MxUsbRole::Device) {
            r.device.quiesce();
        }
        if (r.role == MxUsbRole::Detached && r.desired == MxUsbRole::Detached) {
            r.stage = 3;
        } else {
            if (r.desired != MxUsbRole::Detached) {
                if (!identify(r.registers, &r.geometry)) {
                    r.fault_stage = 20;
                    return false;
                }
                mx_u32_t cfg = 0;
                r.registers.read(r.registers.context, kUsbConfig, &cfg);
                if (r.geometry.high_speed_phy != 1 && r.geometry.high_speed_phy != 3) {
                    r.fault_stage = 21;
                    return false;
                }
                const bool wide = r.geometry.utmi_width == 1;
                // Select the advertised UTMI bus, with its documented PHY
                // turnaround. Software roles disable legacy HNP/SRP policy.
                cfg &= ~((1u << 29) | (1u << 30) | (15u << 10) | (1u << 3) | (1u << 4) | (1u << 6) |
                         (1u << 8) | (1u << 9) | (1u << 15));
                cfg |= (wide ? (1u << 3) : 0) | ((wide ? 5u : 9u) << 10);
                cfg |= r.desired == MxUsbRole::Host ? (1u << 29) : (1u << 30);
                r.registers.write(r.registers.context, kUsbConfig, cfg);
                libk::dma::order_mmio();
            }
            if (!r.reset.begin(r.registers, r.deadline)) {
                r.fault_stage = 22;
                return false;
            }
            r.stage = 1;
        }
    }
    if (r.stage == 1) {
        const auto state = r.reset.advance(r.registers, now);
        if (state == ResetState::Fault) {
            r.fault_stage = 30;
            return false;
        }
        if (state != ResetState::Ready) {
            return true;
        }
        r.stage = 2;
    }
    if (r.stage == 2) {
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
        r.stage = 3;
    }
    if (r.stage == 3) {
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
        r->host.poll();
        r->device.poll();
        if (!transition(*r)) {
            char line[128];
            const int length =
                libk::snprintf(line, sizeof(line),
                               "drv.usb.dwc2: transition fault stage=%u reason=%u desired=%u\n",
                               static_cast<unsigned>(r->stage),
                               static_cast<unsigned>(r->fault_stage),
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
            mochi::message_get(slot, &request, MX_ROLE_SET)) {
            r->reply = {request.id, MX_OK, 0, 0, 0};
            if (request.reserved || request.value > 2 ||
                (r->role != MxUsbRole::Detached && request.value)) {
                r->reply.status = MX_ERR_BUSY;
                r->pending = true;
            } else {
                r->desired = static_cast<MxUsbRole>(request.value);
                r->busy = true;
                r->stage = 0;
                r->deadline = mx_clock_ns() + 2000000000ull;
                char line[80];
                const int length = libk::snprintf(line, sizeof(line), "role-set value=%u\n",
                                                  static_cast<unsigned>(request.value));
                if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
                    mochi::log_raw_marker(line);
                }
            }
        }
        // Owned device sources are drained before rearming this level IRQ.
        if (r->role != MxUsbRole::Detached) {
            (void)mx_irq_ack(r->irq);
        }
        mochi::driver::idle(r->links, 3, r->wake,
                            mx_clock_ns() + (r->busy ? 1000000ull : 10000000ull));
    }
}
