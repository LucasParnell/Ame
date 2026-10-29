#include "mochi/driver.h"
#include "mochi/drivers/dw_i2c.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/platform.h"
namespace {
struct Runtime {
    mochi::SpscEndpoint links[2];
    mx_u16_t addresses[2];
    mochi::i2c::Executor executor;
    MxI2cReply replies[2];
    bool pending[2];
    mx_u32_t owner, next;
};
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "drv.i2c.dw");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    const auto cap = mochi::boot::handle(boot, "i2c.control");
    constexpr auto service = "drv.i2c.dw";
    if (!r) mochi::driver::startup_failed(service, "state arena", MX_ERR_NOMEM);
    auto status = mx_platform_control(cap, MX_PLATFORM_CLAIM);
    if (status != MX_OK) mochi::driver::startup_failed(service, "platform claim", status);
    status = mx_platform_control(cap, MX_PLATFORM_I2C_CLOCK_ON);
    if (status != MX_OK) mochi::driver::startup_failed(service, "clock enable", status);
    status = mx_platform_control(cap, MX_PLATFORM_I2C_PINS);
    if (status != MX_OK) mochi::driver::startup_failed(service, "pin mux", status);
    r->executor.mmio = mochi::driver::registers(boot, "i2c.mmio", mochi::va::native_driver::kLinks,
                                              service);
    r->links[0] = mochi::driver::link(boot, "i2c.0", mochi::va::native_driver::kPayload);
    r->links[1] = mochi::driver::link(
        boot, "i2c.1",
        (mochi::va::native_driver::kPayload + mochi::va::native_driver::kLinkStride));
    r->addresses[0] = mochi::driver::value(boot, "i2c.0.address");
    r->addresses[1] = mochi::driver::value(boot, "i2c.1.address");
    if (!r->links[0].valid()) mochi::driver::startup_failed(service, "Type-C transport");
    if (!r->links[1].valid()) mochi::driver::startup_failed(service, "power transport");
    if (!r->addresses[0] || !r->addresses[1]) mochi::driver::startup_failed(service, "address leases");
    mx_u32_t comp = 0;
    (void)r->executor.mmio.read32(0xfc, &comp);
    char comp_line[80];
    const int comp_len = libk::snprintf(comp_line, sizeof(comp_line), "controller comp=0x%08x",
                                        static_cast<unsigned>(comp));
    if (comp_len > 0 && static_cast<size_t>(comp_len) < sizeof(comp_line)) {
        mochi::log_raw_marker(comp_line);
    }
    mochi::boot::ready(boot);
    for (;;) {
        r->executor.advance(mx_clock_ns());
        if (r->executor.phase == mochi::i2c::Phase::Fault) {
            char fault_line[80];
            const int fault_len = libk::snprintf(fault_line, sizeof(fault_line),
                                                 "fault abort=0x%08x",
                                                 static_cast<unsigned>(r->executor.abort_source));
            if (fault_len > 0 && static_cast<size_t>(fault_len) < sizeof(fault_line)) {
                mochi::log_raw_marker(fault_line);
            }
            mx_exit(MX_ERR_FAULT);
        }
        if (r->executor.phase == mochi::i2c::Phase::Done && !r->pending[r->owner]) {
            r->replies[r->owner] = r->executor.result;
            r->pending[r->owner] = true;
            r->executor.release();
        }
        for (mx_u32_t n = 0; n < 2; ++n) {
            if (r->pending[n] && r->links[n].tx().try_push_msg(MX_I2C_REPLY, r->replies[n])) {
                r->pending[n] = false;
            }
        }
        if (r->executor.phase == mochi::i2c::Phase::Idle) {
            for (mx_u32_t n = 0; n < 2; ++n) {
                const auto i = (r->next + n) & 1;
                if (r->pending[i]) {
                    continue;
                }
                MxRingSlot slot{};
                if (!r->links[i].try_recv(&slot)) {
                    continue;
                }
                MxI2cTransaction request{};
                if (mochi::message_get(slot, &request, MX_I2C_TRANSACTION)) {
                    if (request.address != r->addresses[i] ||
                        !r->executor.start(request, mx_clock_ns())) {
                        r->replies[i] = {request.id, MX_ERR_PERM, 0, {}};
                        r->pending[i] = true;
                    } else {
                        r->owner = i;
                        r->next = i ^ 1;
                    }
                }
                break;
            }
        }
        mochi::driver::idle(r->links, 2, r->links[0].rx_doorbell, mx_clock_ns() + 1000000);
    }
}
