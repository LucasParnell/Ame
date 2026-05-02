// kernel/drivers/uart/pl011.cc — PL011 UART driver.
//
// Spec ref: §5.11 UART. Two paths:
//   early_init / putchar: raw MMIO, used before anything else works.
//   Later: UART capability handed to io.console; kernel keeps early path
//          as panic fallback.
//
// PL011 is at platform::kUartBase on QEMU virt. After DTB parse the base
// is confirmed and may be re-pointed via early_init().
//
// Phase 0: MMU is off. We write the physical address directly. Phase 1
// will switch to the kernel virtual mapping.

#include "anko/platform.h"

namespace drivers::uart {

static volatile u32* g_base = reinterpret_cast<volatile u32*>(platform::kUartBase);

void putchar(char c) {
    // Wait for TX FIFO to drain enough for one byte.
    while (g_base[platform::kUartFR / 4] & platform::kUartFR_TXFF) {
        // spin
    }
    g_base[platform::kUartDR / 4] = static_cast<u32>(static_cast<unsigned char>(c));
}

void putchar_thunk(char c) { putchar(c); }

void early_init(PhysAddr base) {
    // QEMU virt already configures the PL011 (8N1, 115200). We just record
    // the base. On real hardware we will program LCR/IBRD/FBRD here.
    g_base = reinterpret_cast<volatile u32*>(base);
}

void puts(const char* s) {
    while (*s) putchar(*s++);
}

} // namespace drivers::uart
