// userland/console/main.cc — io.console service.
//
// Spec ref: §5.11 UART, §7.4 io.console
//
// Boot handshake: init fills a ConsoleBootConfig page and grants it as cap slot
// 0. Console maps the config, waits for magic, then maps the UART, the shared
// log doorbell and the log rings (the ring count is dynamic).
//
// The service is a log sink: it drains every log ring to the board UART and
// mirrors the same bytes to tty.fb0. The ring set and its shared-doorbell
// sleep/wake are the library abstraction mochi::RingReaderSet, so the body here
// is only "what to do with one slot". The service never touches ring internals,
// a raw slot type or the doorbell protocol directly.

#include "mochi/syscall.h"
#include "mochi/handle.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/console_config.h"
#include "mochi/boot.h"
#include "mochi/terminal.h"

constexpr uint32_t kLogRingOrder = 6;

constexpr uint64_t kVaConsoleConfig = 0x3000'0000ull;
constexpr uint64_t kVaUart          = 0x2000'0000ull;
constexpr uint64_t kVaLogRingBase   = 0x2001'0000ull;
constexpr uint64_t kVaTtyMirrorRing = 0x2040'0000ull;

// Every log ring gets a private, generously aligned VA window so the rings can
// never overlap; the window must hold one whole ring page.
constexpr uint64_t kLogRingVaStride = 0x1'0000ull;
static_assert(kLogRingVaStride >= mx_ring_page_bytes(kLogRingOrder),
              "log ring VA stride must hold one ring page");

namespace {

// The board UART io.console owns. Word offsets are the PL011 layout, the console
// on the QEMU virt bring-up path; naming them keeps the write loop data-shaped.
struct Uart {
    volatile uint32_t* regs;

    static constexpr uint32_t kDr     = 0x00u / 4u; // data register
    static constexpr uint32_t kFr     = 0x18u / 4u; // flag register
    static constexpr uint32_t kFrTxFf = 1u << 5;    // FR.TXFF: transmit FIFO full

    void put(char c) const {
        while (regs[kFr] & kFrTxFf) { }
        regs[kDr] = static_cast<uint32_t>(static_cast<unsigned char>(c));
    }
    void write(const char* data, uint32_t len) const {
        for (uint32_t i = 0; i < len; ++i) put(data[i]);
    }
};

// One log line straight to the UART, in the shared log format.
void uart_log(const Uart& uart, mochi::LogLevel level, const char* component, const char* msg) {
    char line[256];
    bool truncated = false;
    const uint32_t len = mochi::log_format_line(line, sizeof(line), level, component, msg, &truncated);
    (void)truncated;
    uart.write(line, len);
}

} // namespace

// ---- Entry -----------------------------------------------------------------

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();

    // The console-specific config is a named arena in BootInfo, so the fixed cap
    // slots other services still use never appear here.
    const CapHandle config_cap = mochi::boot::require_handle(boot, "console.config");
    auto* cfg = mochi::cap_map_when<ConsoleBootConfig>(config_cap, kCapTypeArena,
                                                       kVaConsoleConfig,
                                                       RIGHT_READ | RIGHT_WRITE);
    while (__atomic_load_n(&cfg->magic, __ATOMIC_ACQUIRE) != ConsoleBootConfig::kMagic) {
        sys_yield();
    }

    const uint32_t ring_count = cfg->total_ring_count < ConsoleBootConfig::kMaxRings
                                    ? cfg->total_ring_count
                                    : ConsoleBootConfig::kMaxRings;

    const Uart uart{mochi::cap_map_when<volatile uint32_t>(static_cast<CapHandle>(cfg->uart_cap),
                                                           kCapTypeUart, kVaUart,
                                                           RIGHT_READ | RIGHT_WRITE)};

    const CapHandle log_doorbell = static_cast<CapHandle>(cfg->log_doorbell_cap);
    mochi::cap_wait_type(log_doorbell, kCapTypeDoorbell);

    // Map every log ring into its own VA window; RingReaderSet drains the set.
    RingPage* log_rings[ConsoleBootConfig::kMaxRings]{};
    for (uint32_t i = 0; i < ring_count; ++i) {
        log_rings[i] = mochi::cap_map_when<RingPage>(
            static_cast<CapHandle>(cfg->log_ring_caps[i]), kCapTypeSharedRing,
            kVaLogRingBase + static_cast<uint64_t>(i) * kLogRingVaStride,
            RIGHT_READ | RIGHT_WRITE);
    }
    const mochi::RingReaderSet logs{log_rings, ring_count, kLogRingOrder, log_doorbell};

    // Optional mirror to the framebuffer terminal; absent on a headless board.
    uint32_t mirror_order = kLogRingOrder;
    RingPage* mirror_ring = mochi::boot::map_ring(boot, "tty.mirror.ring", kVaTtyMirrorRing,
                                                  kLogRingOrder, &mirror_order);
    mochi::terminal::OutputSink mirror{};
    mirror.ring = mirror_ring;
    mirror.order = mirror_order;
    mirror.doorbell = mochi::boot::handle(boot, "tty.mirror.doorbell");

    // Every log slot: write it to the UART, and mirror the same bytes to the tty.
    auto emit = [&uart, &mirror](const RingSlot& slot) {
        const char* bytes = reinterpret_cast<const char*>(slot.payload);
        uart.write(bytes, slot.length);
        (void)mirror.try_write_bytes(bytes, slot.length);
    };

    logs.drain(emit);
    uart_log(uart, mochi::LogLevel::Info, "io.console", "ready");
    mochi::boot::ready(boot);

    // Blocking drain loop: RingReaderSet owns the sleep/wake protocol.
    for (;;) {
        if (!logs.wait(emit)) sys_yield();
    }
}
