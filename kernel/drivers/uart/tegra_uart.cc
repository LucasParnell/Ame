// kernel/drivers/uart/tegra_uart.cc — Tegra 16550-compatible UART driver.
//
// Used on Nintendo Switch (Tegra X1 T210) UARTA @ 0x70006000.
// U-Boot pre-configures the UART at 115200 8N1; we just write characters.
//
// The 16550 core uses reg-shift=2 on Tegra: each register is 32-bit wide.
//   THR (transmit hold) = offset 0x00 → g_base[0]
//   LSR (line status)   = offset 0x14 → g_base[5]
//   LSR.THRE = bit 5: set when TX FIFO has room.

#include "anko/console.h"

namespace drivers::uart {

static volatile u32* g_tegra_base;

static void tegra_early_init(PhysAddr base) {
    // U-Boot already programmed IBRD/LCR/FCR; just record the base.
    g_tegra_base = reinterpret_cast<volatile u32*>(base);
}

static void tegra_putchar(char c) {
    // Spin until TX FIFO has room (LSR.THRE = 1).
    while (!(g_tegra_base[5] & (1u << 5))) {
        // spin
    }
    g_tegra_base[0] = static_cast<u32>(static_cast<unsigned char>(c));
}

extern const ConsoleOps kTegraUartOps = { tegra_early_init, tegra_putchar };

} // namespace drivers::uart
