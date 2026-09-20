// boards/switch-t210/early_console.cc — Phase 0 UART for Nintendo Switch T210.
//
// Hardcodes Tegra UARTA base 0x70006000.  U-Boot pre-configures the UART
// at 115200 8N1 before handing off; we just poll LSR.THRE and write THR.
//
// 16550 core with reg-shift=2: each register occupies 4 bytes.
//   THR (transmit hold) = reg 0 → base[0]
//   LSR (line status)   = reg 5 → base[5]  (offset 0x14)
//   LSR.THRE            = bit 5

#include "anko/types.h"

namespace drivers::early_uart {

static constexpr PhysAddr kBase = 0x7000'6000;

void putchar(char c) {
    auto* base = reinterpret_cast<volatile u32*>(kBase);
    // Poll THRE (LSR bit 5) until TX FIFO has room.
    while (!(base[5] & (1u << 5))) { /* spin */ }
    base[0] = static_cast<u32>(static_cast<unsigned char>(c));
}

} // namespace drivers::early_uart
