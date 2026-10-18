// FUSB301 REG_TYPE: bit3 ("UFP") reads when a DFP host is attached, so our
// port is the device; bit4 ("DFP") reads when a UFP/OTG device is attached,
// so our port is the host. Matches vendor fusb301_detect_port_mode().
// Connector orientation is independent.
#include "mochi/drivers/i2c.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/board_usb.h"
#include "mochi/usb/typec_policy.h"
namespace {
struct Runtime {
    mochi::i2c::Client bus;
    mochi::SpscEndpoint control;
    MxBoardReply reply;
    mochi::usb::typec::SinkPreference policy;
    mx_u64_t manual_at;
    mx_u8_t observed0, observed1;
    bool raw_logged;
    bool pending;
};
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "drv.typec.fusb301");
    constexpr auto service = "drv.typec.fusb301";
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::driver::startup_failed(service, "state arena", MX_ERR_NOMEM);
    }
    r->bus = {mochi::driver::link(boot, "i2c", mochi::va::native_driver::kLinks), mx_clock_ns(),
              static_cast<mx_u16_t>(mochi::driver::value(boot, "i2c.address"))};
    r->control = mochi::driver::link(
        boot, "typec", (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    if (!r->control.valid()) mochi::driver::startup_failed(service, "typec link");
    if (!r->bus.endpoint.valid()) mochi::driver::startup_failed(service, "i2c link");
    mx_u8_t id = 0;
    if (!r->policy.configure(r->bus, mx_clock_ns(), id)) {
        char line[80];
        const int length = libk::snprintf(line, sizeof(line), "device id=0x%02x",
                                          static_cast<unsigned>(id));
        if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
            mochi::log_raw_marker(line);
        }
        mochi::driver::startup_failed(service, "port configuration/readback");
    }
    // The signed profile opts into automatic switching. An omitted binding
    // retains the peripheral-only policy for older/recovery descriptors.
    const bool device_only = mochi::driver::value(boot, "usb.device_only", 1) != 0;
    mochi::log_raw_marker(device_only ? "typec: sink-only; source disabled\n"
                                     : "typec: automatic; sink preferred, keyboard host enabled\n");
    mochi::boot::ready(boot);
    for (;;) {
        // FUSB301 requires at least 2 ms between selecting Sink and writing
        // UNATT_SNK. Schedule it; never busy-wait or block unrelated services.
        if (r->manual_at && mx_clock_ns() >= r->manual_at) {
            if (!r->bus.write(4, 8)) r->reply.status = MX_ERR_FAULT;
            r->manual_at = 0;
            r->pending = true;
        }
        if (r->pending && r->control.tx().try_push_msg(MX_TYPEC_REPLY, r->reply)) {
            r->pending = false;
        }
        MxRingSlot slot{};
        MxBoardRequest request{};
        if (!r->pending && !r->manual_at && r->control.try_recv(&slot) &&
            (mochi::message_get(slot, &request, MX_TYPEC_QUERY) ||
             mochi::message_get(slot, &request, MX_TYPEC_RESTART_SINK))) {
            r->reply = {request.id, MX_OK, 0, 0, 0};
            if (slot.type == MX_TYPEC_RESTART_SINK) {
                if (request.value || request.reserved || !r->policy.begin(r->bus, mx_clock_ns())) {
                    r->reply.status = MX_ERR_FAULT;
                    r->pending = true;
                } else {
                    r->policy.deadline = mx_clock_ns() + 2000000000ull;
                    r->manual_at = mx_clock_ns() + 2000000ull;
                }
                continue;
            }
            const mx_u8_t reg = 0x11;
            // Reading the latched interrupt byte catches detach/replug between
            // queries without introducing a second I2C transaction.
            mx_u8_t bytes[3]{};
            if (request.value || request.reserved ||
                !r->bus.transaction(libk::as_bytes(&reg, 1), libk::as_bytes(bytes))) {
                r->reply.status = MX_ERR_FAULT;
            } else {
                if (!r->raw_logged || bytes[0] != r->observed0 || bytes[1] != r->observed1) {
                    r->raw_logged = true;
                    r->observed0 = bytes[0];
                    r->observed1 = bytes[1];
                    char line[96];
                    const int length = libk::snprintf(line, sizeof(line),
                                                      "drv.typec.fusb301: raw 0x%02x/0x%02x\n",
                                                      static_cast<unsigned>(bytes[0]),
                                                      static_cast<unsigned>(bytes[1]));
                    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
                        mochi::log_raw_marker(line);
                    }
                }
                const auto decoded = r->policy.observe(r->bus, bytes[0], bytes[1], bytes[2],
                                                        mx_clock_ns(), device_only);
                r->reply.status = decoded.status;
                r->reply.value = decoded.value;
                r->reply.flags = decoded.flags;
            }
            r->pending = true;
        }
        mochi::driver::idle(&r->control, 1, r->control.rx_doorbell,
                             r->manual_at ? r->manual_at : mx_clock_ns() + 50000000,
                             r->manual_at ? 0u : 1u);
    }
}
