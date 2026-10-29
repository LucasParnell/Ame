// userland/console/main.cc — io.console service.
//
// Spec ref: §5.11 UART, §7.4 io.console
//
// The service is a log sink: it drains every kernel/service log ring to the
// board UART and mirrors the same bytes to tty.fb0. Ring internals, the address
// layout and the shared-doorbell sleep/wake all live behind named bring-up
// steps below, so _start() reads as the four things the service does.

#include "mochi/syscall.h"
#include "mochi/handle.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/console_config.h"
#include "mochi/boot.h"
#include "mochi/terminal.h"
#include "mochi/va_layout.h"

namespace {

// The service's VA windows live in mochi/va_layout.h (one plan, precomputed).
using mochi::va::console::kConfigBase;
using mochi::va::console::kLogRingOrder;
using mochi::va::console::kWindows;

// --- Board UART ------------------------------------------------------------
//
// The console owns the UART and writes bytes to it directly. Word offsets are
// the PL011 layout (the QEMU virt bring-up console).
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
#ifdef ANKO_BOARD_HUAWEI_ANNE
        // Initial native bring-up keeps userland logs in the persistent kernel
        // console so they can be read after restoring the shared Huawei kernel.
        sys_debug_puts(data, len);
#else
        for (uint32_t i = 0; i < len; ++i) put(data[i]);
#endif
    }
    // One line in the shared log format.
    void line(mochi::LogLevel level, const char* component, const char* msg) const {
        char buf[256];
        bool truncated = false;
        const uint32_t n = mochi::log_format_line(buf, sizeof(buf), level, component, msg, &truncated);
        (void)truncated;
        write(buf, n);
    }
};

// --- Bring-up steps --------------------------------------------------------

// Map the console config arena and wait for init to publish its magic.
const ConsoleBootConfig* open_config(const MxBootInfo* boot) {
    const CapHandle cap = mochi::boot::require_handle(boot, "console.config");
    const ConsoleBootConfig* cfg =
        mochi::cap_map_when<ConsoleBootConfig>(cap, MX_CAP_ARENA, kConfigBase, MX_RIGHTS_RW);
    console_config_wait_ready(cfg);
    return cfg;
}

Uart open_uart(const ConsoleBootConfig* cfg) {
    return {mochi::cap_map_when<volatile uint32_t>(cfg->uart_cap, MX_CAP_UART, kWindows.uart,
                                                   MX_RIGHTS_RW)};
}

// Map every log ring into its own window and hand back a reader over the set.
mochi::RingReaderSet open_logs(const ConsoleBootConfig* cfg, RingPage** storage) {
    const uint32_t count = cfg->total_ring_count < ConsoleBootConfig::kMaxRings
                               ? cfg->total_ring_count
                               : ConsoleBootConfig::kMaxRings;
    for (uint32_t i = 0; i < count; ++i) {
        storage[i] = mochi::cap_map_when<RingPage>(cfg->log_ring_caps[i], MX_CAP_SHARED_RING,
                                                   kWindows.log_ring(i), MX_RIGHTS_RW);
    }
    mochi::cap_wait_type(cfg->log_doorbell_cap, MX_CAP_DOORBELL);
    return {storage, count, kLogRingOrder, cfg->log_doorbell_cap};
}

// Optional mirror to the framebuffer terminal; absent on a headless board.
mochi::terminal::OutputSink open_mirror(const MxBootInfo* boot) {
    mochi::terminal::OutputSink sink{};
    sink.order = kLogRingOrder;
    sink.ring = mochi::boot::map_ring(boot, "tty.mirror.ring", kWindows.mirror_ring,
                                      kLogRingOrder, &sink.order);
    sink.doorbell = mochi::boot::handle(boot, "tty.mirror.doorbell");
    return sink;
}

// What to do with one drained log slot: UART and the tty mirror, same bytes.
struct EmitSlot {
    const Uart& uart;
    const mochi::terminal::OutputSink& mirror;

    void operator()(const RingSlot& slot) const {
        if (!mochi::message_valid(slot)) return;
        const char* bytes = reinterpret_cast<const char*>(slot.payload);
        uart.write(bytes, slot.length);
        (void)mirror.try_write_bytes(bytes, slot.length);
    }
};

} // namespace

// --- Entry -----------------------------------------------------------------

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    const ConsoleBootConfig* cfg = open_config(boot);
    const Uart uart = open_uart(cfg);

    RingPage* log_rings[ConsoleBootConfig::kMaxRings]{};
    const mochi::RingReaderSet logs = open_logs(cfg, log_rings);
    const mochi::terminal::OutputSink mirror = open_mirror(boot);
    const EmitSlot emit{uart, mirror};

    logs.drain(emit);
    uart.line(mochi::LogLevel::Info, "io.console", "ready");
    mochi::boot::ready(boot);

    // Blocking drain loop; RingReaderSet owns the sleep/wake protocol.
    for (;;) {
        if (!logs.wait(emit)) sys_yield();
    }
}
