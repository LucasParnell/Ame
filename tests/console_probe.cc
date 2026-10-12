#include "anko/console.h"
#include <cassert>

namespace {
char captured[8]{};
unsigned count = 0;
void capture(char c) {
    assert(count < sizeof(captured));
    captured[count++] = c;
}
}

int main() {
    // No MMIO access before binding, but a secondary diagnostic sink can work.
    console::set_secondary_sink(capture);
    console::putchar('a');
    assert(count == 1 && captured[0] == 'a');

    // Exercise the actual drivers against two independent register banks.
    // A Tegra UART must never touch the PL011 bank (or a fixed QEMU address).
    alignas(8) u32 pl011[16]{};
    alignas(8) u32 tegra[16]{};
    tegra[5] = 1u << 5; // Tegra LSR.THRE
    console::bind(ConsoleKind::PL011, reinterpret_cast<PhysAddr>(pl011));
    console::putchar('b');
    assert(pl011[0] == 'b' && tegra[0] == 0);
    console::bind(ConsoleKind::Tegra16550, reinterpret_cast<PhysAddr>(tegra));
    console::putchar('c');
    assert(tegra[0] == 'c' && pl011[0] == 'b');
    assert(count == 3 && captured[1] == 'b' && captured[2] == 'c');

    // Rebinding after DT discovery changes the hardware destination.
    alignas(8) u32 discovered[16]{};
    discovered[5] = 1u << 5;
    console::bind(ConsoleKind::Tegra16550, reinterpret_cast<PhysAddr>(discovered));
    console::set_secondary_sink(nullptr);
    console::putchar('d');
    assert(discovered[0] == 'd' && tegra[0] == 'c' && count == 3);
}
