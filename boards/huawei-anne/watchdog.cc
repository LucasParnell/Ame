#include "watchdog.h"
#include "boot_diagnostics.h"
#include "anko/platform_clock.h"
#include "libk/sp805_watchdog.h"
#include "libk/printf.h"

namespace anne::watchdog {
namespace {
struct State {
    u64 magic;
    libk::sp805::Probe owner;
    u64 last_feed, feeds;
    bool paused;
};
// Adopt before BSS/constructors. This state must survive their initialization.
__attribute__((section(".data.anne_watchdog"))) State state{0x574447414e4e4531, {}, 0, 0, false};
struct Bus {
    u32 read(u32 offset) {
        asm volatile("dsb sy" ::: "memory");
        return *reinterpret_cast<volatile u32*>(kBase + offset);
    }
    void write(u32 offset, u32 value) {
        *reinterpret_cast<volatile u32*>(kBase + offset) = value;
        asm volatile("dsb sy" ::: "memory");
    }
};
bool primary() {
    u64 cpu; asm volatile("mrs %0, tpidr_el1" : "=r"(cpu));
    return cpu == 0;
}
}
void report(const char* phase) {
    Bus bus;
    char line[320];
    const int size = libk::snprintf(line, sizeof(line),
        "\nWATCHDOG phase=%s status=%u id=0x%x control=0x%x load=0x%x value=0x%x ris=0x%x feeds=%llu\n",
        phase, static_cast<u32>(state.owner.status), state.owner.id, bus.read(libk::sp805::kControl),
        bus.read(libk::sp805::kLoad), bus.read(libk::sp805::kValue), bus.read(libk::sp805::kRawStatus),
        static_cast<unsigned long long>(state.feeds));
    if (size > 0 && static_cast<usize>(size) < sizeof(line)) boot_log_write(line, static_cast<usize>(size));
}
void adopt() {
    if (!primary()) return;
    Bus bus;
    state.owner = libk::sp805::probe(bus);
    state.last_feed = minimal::counter(); state.feeds = 0; state.paused = false;
    if (libk::sp805::keepalive(bus, state.owner)) ++state.feeds;
    report("adopt");
}
void poll() {
    if (!primary() || state.paused || state.owner.status != libk::sp805::Status::Active) return;
    const u64 now = minimal::counter();
    u64 reported; asm volatile("mrs %0, cntfrq_el0" : "=r"(reported));
    if (!minimal::elapsed(state.last_feed, now, platform::counter_frequency(reported))) return;
    Bus bus;
    if (!libk::sp805::keepalive(bus, state.owner)) {
        state.owner.status = libk::sp805::Status::Expired;
        report("lost");
        return;
    }
    ++state.feeds; state.last_feed = now;
}
bool pause_for_capture() {
    if (state.owner.status != libk::sp805::Status::Active) return false;
    state.paused = true;
    return true;
}
}
