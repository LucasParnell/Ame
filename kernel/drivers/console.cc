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
} // namespace

namespace console {

void bind(ConsoleKind k, PhysAddr base) {
    const ConsoleOps* ops = nullptr;
    switch (k) {
    case ConsoleKind::PL011:
        ops = &drivers::uart::kPl011Ops;
        break;
    case ConsoleKind::Tegra16550:
        ops = &drivers::uart::kTegraUartOps;
        break;
    }
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
