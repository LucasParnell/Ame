#include <array>
#include <cassert>
#include "rpmb.h"
#include "rpmb_diagnostics.h"
#include "../kernel/drivers/gic/gicv2.cc"

HwInfo g_hw{};
static unsigned ticks, ipis, deliveries;
namespace drivers::timer { void handle_tick() { ++ticks; } }
namespace smp { void handle_ipi() { ++ipis; } }
namespace irq { void deliver(u32) { ++deliveries; } }

static std::array<u32, 1024> dist{}, cpu{};
static std::array<u8, 0x8000> window{};
static unsigned calls;
static u64 firmware(u64 fid, u64, u64 x2, u64 x3) {
    assert(fid == anne::rpmb::kRequestAddressSmc && !x2 && !x3);
    ++calls;
    return 0;
}
static MxDiagEl3Rpmb snapshot(u32 action) {
    MxDiagEl3Rpmb info{};
    anne::rpmb_diag::execute(action, info, window.data(), firmware);
    return info;
}
int main() {
    constexpr u32 priority = 0x44332211;
    dist[0x404 / 4] = priority;
    dist[0x80 / 4] = 0x12345678; // Firmware's security groups must be preserved.
    dist[0x800 / 4] = 1;
    g_hw.timer_irq = 30;
    drivers::gic::init(reinterpret_cast<u64>(dist.data()), reinterpret_cast<u64>(cpu.data()));
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    assert(dist[0x404 / 4] == 0x4433a011);
    assert(dist[0x100 / 4] == (1u << 5));
    dist[0x404 / 4] = priority;
    drivers::gic::init_cpu();
    assert(dist[0x404 / 4] == 0x4433a011); // Every secondary enables its banked SGI.
#else
    assert(dist[0x404 / 4] == priority && dist[0x100 / 4] == 0);
#endif
    assert(dist[0x80 / 4] == 0x12345678);
    assert(snapshot(MX_RPMB_ARM).result == MX_RPMB_OK);
    // SGI source CPU bits must survive EOI; matching uses only the INTID.
    const u32 notification = (3u << 10) | 5;
    cpu[0xc / 4] = notification;
    drivers::gic::handle_irq();
    assert(cpu[0x10 / 4] == notification);
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    assert(snapshot(MX_RPMB_STATUS).events == 1 && !deliveries);
#else
    assert(snapshot(MX_RPMB_STATUS).events == 0 && deliveries == 1);
#endif
    assert(calls == 1); // No SMC from interrupt context.
    const auto events = snapshot(MX_RPMB_STATUS).events;
    cpu[0xc / 4] = 1; drivers::gic::handle_irq();
    cpu[0xc / 4] = 30; drivers::gic::handle_irq();
    const auto previous = deliveries;
    cpu[0xc / 4] = 100; drivers::gic::handle_irq();
    assert(ipis == 1 && ticks == 1 && deliveries == previous + 1);
    assert(snapshot(MX_RPMB_STATUS).events == events);
    cpu[0xc / 4] = 1023;
    cpu[0x10 / 4] = 0xfeed;
    drivers::gic::handle_irq();
    assert(cpu[0x10 / 4] == 0xfeed && calls == 1);
}
