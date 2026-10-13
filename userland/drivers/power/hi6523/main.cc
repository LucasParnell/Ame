// Hi6523 USB source service. It owns only boost/charging-enable/watchdog fields,
// not charging-current or battery policy. Values follow the pinned Hi6250
// schargerV300 vendor sequence; 500 mA is the conservative Type-C default.
#include "mochi/drivers/i2c.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/board_usb.h"
namespace {
struct Runtime {
    mochi::i2c::Client bus;
    mochi::SpscEndpoint control;
    MxBoardReply reply;
    mx_u64_t lease, feed;
    bool enabled, pending;
};
bool disable(Runtime& r) {
    const bool ok = r.bus.update(0x80, 0x40, 0);
    r.enabled = false;
    r.lease = 0;
    return ok;
}
bool enable(Runtime& r) {
    if (!r.bus.update(0x90, 0x20, 0) || !r.bus.update(0x80, 0x58, 0x18)) {
        return false;
    }
    struct Setting {
        mx_u8_t reg, value;
    };
    static constexpr Setting boost[] = {{0x6a, 0x3f}, {0x6b, 0x5c}, {0x81, 0x5c},
                                        {0x86, 0x39}, {0x8a, 0x18}, {0x88, 0x9f}};
    for (auto s : boost) {
        if (!r.bus.write(s.reg, s.value)) {
            return false;
        }
    }
    if (!r.bus.write(0x0c, 0x1e) || !r.bus.update(0xe9, 3, 1) || !r.bus.write(0xe8, 1) ||
        !r.bus.update(0x80, 0x40, 0x40)) {
        return false;
    }
    r.enabled = true;
    r.feed = mx_clock_ns() + 1000000000ull;
    return true;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "drv.power.hi6523");
    constexpr auto service = "drv.power.hi6523";
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::driver::startup_failed(service, "state arena", MX_ERR_NOMEM);
    }
    r->bus = {mochi::driver::link(boot, "i2c", mochi::va::native_driver::kLinks), mx_clock_ns(),
              static_cast<mx_u16_t>(mochi::driver::value(boot, "i2c.address"))};
    r->control = mochi::driver::link(
        boot, "power", (mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride));
    if (!r->control.valid()) mochi::driver::startup_failed(service, "power link");
    if (!r->bus.endpoint.valid()) mochi::driver::startup_failed(service, "i2c link");
    mx_u8_t id = 0;
    if (!r->bus.read(4, id)) mochi::driver::startup_failed(service, "device id read");
    if (id != 1 && id != 2 && id != 3) {
        char line[80];
        const int length = libk::snprintf(line, sizeof(line), "device id=0x%02x",
                                          static_cast<unsigned>(id));
        if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
            mochi::log_raw_marker(line);
        }
        mochi::driver::startup_failed(service, "device id");
    }
    if (!disable(*r)) mochi::driver::startup_failed(service, "boost disable");
    mochi::boot::ready(boot);
    for (;;) {
        const auto now = mx_clock_ns();
        if (r->enabled && now >= r->lease) {
            (void)disable(*r);
        }
        if (r->enabled && now >= r->feed) {
            mx_u8_t faults = 0;
            if (!r->bus.read(0x0c, faults) || (faults & 0x1e) || !r->bus.write(0xe8, 1)) {
                (void)disable(*r);
            }
            r->feed = now + 1000000000ull;
        }
        if (r->pending && r->control.tx().try_push_msg(MX_POWER_REPLY, r->reply)) {
            r->pending = false;
        }
        MxRingSlot slot{};
        MxBoardRequest request{};
        if (!r->pending && r->control.try_recv(&slot) &&
            mochi::message_get(slot, &request, MX_POWER_SET)) {
            bool ok = !request.reserved && (request.value == 0 || request.value == 500);
            if (ok) {
                ok = request.value ? (r->enabled || enable(*r)) : disable(*r);
            }
            if (!ok) {
                (void)disable(*r);
                char line[96];
                const int length = libk::snprintf(line, sizeof(line),
                                                  "drv.power.hi6523: set %u failed\n",
                                                  static_cast<unsigned>(request.value));
                if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
                    mochi::log_raw_marker(line);
                }
            }
            if (ok && request.value) {
                r->lease = mx_clock_ns() + 2000000000ull;
            }
            r->reply = {request.id, ok ? MX_OK : MX_ERR_FAULT, r->enabled ? 500u : 0u, 0, 0};
            r->pending = true;
        }
        mochi::driver::idle(&r->control, 1, r->control.rx_doorbell, mx_clock_ns() + 100000000);
    }
}
