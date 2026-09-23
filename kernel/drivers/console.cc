// kernel/drivers/console.cc — console ops dispatch.
//
// console::bind() selects the right UART driver for the current board and
// initialises it.  drivers::uart::putchar_thunk() forwards to the active
// ops so the rest of the kernel is driver-agnostic.

#include "anko/console.h"

namespace drivers::uart {
    extern const ConsoleOps kPl011Ops;
    extern const ConsoleOps kTegraUartOps;
} // namespace drivers::uart

namespace {
    const ConsoleOps* g_console_ops = nullptr;
    void (*g_secondary_sink)(char)  = nullptr;

    // One row per ConsoleKind the kernel knows how to drive. Board selection is
    // a data table, not a dispatch switch: adding a board is one row here,
    // matching the function-pointer ops table the drivers already use.
    struct ConsoleDriver {
        ConsoleKind       kind;
        const ConsoleOps* ops;
    };
    const ConsoleDriver kConsoleDrivers[] = {
        { ConsoleKind::PL011,      &drivers::uart::kPl011Ops },
        { ConsoleKind::Tegra16550, &drivers::uart::kTegraUartOps },
    };

    const ConsoleOps* ops_for(ConsoleKind kind) {
        for (const ConsoleDriver& driver : kConsoleDrivers) {
            if (driver.kind == kind) return driver.ops;
        }
        return nullptr;
    }
} // namespace

namespace console {

void bind(ConsoleKind k, PhysAddr base) {
    const ConsoleOps* ops = ops_for(k);
    if (!ops) return;
    ops->early_init(base);
    g_console_ops = ops;
}

} // namespace console

namespace console {

void set_secondary_sink(void (*fn)(char)) {
    g_secondary_sink = fn;
}

} // namespace console

namespace drivers::uart {

void putchar_thunk(char c) {
    if (g_console_ops) g_console_ops->putc(c);
    if (g_secondary_sink) g_secondary_sink(c);
}

} // namespace drivers::uart
