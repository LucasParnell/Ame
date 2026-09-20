// boards/qemu-virt/early_console.cc — Phase 0 UART for QEMU virt.
//
// Hardcodes PL011 base 0x09000000 and fires before DTB parse or any C++
// static initialisation.  Used only for the very first boot messages
// before console::bind() re-establishes the UART from g_hw.
//
// On QEMU virt the PL011 is pre-configured by the firmware at 115200 8N1.

#include "anko/types.h"

namespace drivers::early_uart {

static constexpr PhysAddr kBase = 0x0900'0000;
static constexpr u32 kFR  = 0x018; // flag register offset
static constexpr u32 kDR  = 0x000; // data register offset
static constexpr u32 kTXFF = 1u << 5; // TX FIFO full bit

void putchar(char c) {
    auto* base = reinterpret_cast<volatile u32*>(kBase);
    while (base[kFR / 4] & kTXFF) { /* spin */ }
    base[kDR / 4] = static_cast<u32>(static_cast<unsigned char>(c));
}

} // namespace drivers::early_uart
