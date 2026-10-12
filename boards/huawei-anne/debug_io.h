#pragma once
#include "anko/types.h"
#include "minimal/timing.h"

namespace anne {
#if defined(ANKO_ANNE_DIAG_TEST)
// QEMU places its DTB at RAM+64 MiB; keep the full-kernel test transport clear.
constexpr u64 kDebugRamBase = 0x46000000;
constexpr u64 kDebugPmicBase = 0x47000000;
constexpr u64 kDebugSctrlBase = 0x47100000;
#elif defined(ANNE_MINIMAL_TEST)
constexpr u64 kDebugRamBase = 0x44000000;
constexpr u64 kDebugPmicBase = 0x43000000;
constexpr u64 kDebugSctrlBase = 0x45000000;
#else
constexpr u64 kDebugRamBase = 0x34800000;
constexpr u64 kDebugPmicBase = 0xfff34000;
constexpr u64 kDebugSctrlBase = 0xfff0a000;
#endif
constexpr u64 kDebugRamBytes = 0x100000;
constexpr u64 kDebugZones[] = {kDebugRamBase + 0x60000, kDebugRamBase + 0xe0000,
                              kDebugRamBase, kDebugRamBase + 0x20000, kDebugRamBase + 0x40000};
constexpr u32 kDebugZoneSizes[] = {0x80000, 0x20000, 0x20000, 0x20000, 0x20000};
inline u8 debug_pmic_read(u32 reg) {
    return *reinterpret_cast<volatile u8*>(kDebugPmicBase + (reg << 2));
}
inline void debug_pmic_write(u32 reg, u8 value) {
    *reinterpret_cast<volatile u8*>(kDebugPmicBase + (reg << 2)) = value;
}
inline void debug_led(char stage) {
    if (stage == 'o') {
        debug_pmic_write(0x14d, debug_pmic_read(0x14d) & ~7u);
        return;
    }
    if (stage != 'r' && stage != 'g' && stage != 'b' && stage != 'w') return;
    // Match the stock driver's disable path: clear the channel enables. The
    // two-bit output controls are not disable bits; setting unselected pairs
    // to 3 can override a disabled channel and produce mixed colours.
    debug_pmic_write(0x14d, debug_pmic_read(0x14d) & ~7u);
    // Return every channel to normal output control, including any override
    // inherited from the loader or an earlier diagnostic image.
    debug_pmic_write(0x14e, debug_pmic_read(0x14e) & ~0x3fu);
    asm volatile("dsb sy" ::: "memory");
#if !defined(ANKO_ANNE_DIAG_TEST) && !defined(ANNE_MINIMAL_TEST)
    minimal::wait_ticks(minimal::kCounterHz / 1000);
#endif
    const u32 selected = stage == 'w' ? 7u : 1u << (stage == 'r' ? 0 : stage == 'g' ? 1 : 2);
    debug_pmic_write(0x156, 0x33);
    for (u32 id = 0; id < 3; ++id) {
        if (!(selected & (1u << id))) continue;
        debug_pmic_write(0x14f + 2 * id, id == 1 ? 1 : 7);
        debug_pmic_write(0x150 + 2 * id, 3);
    }
    debug_pmic_write(0x155, 0xf0);
    debug_pmic_write(0x14d, (debug_pmic_read(0x14d) & ~7u) | selected);
    debug_pmic_write(0x14e, debug_pmic_read(0x14e) & ~0x3fu);
}
inline void debug_clean(u64 base, u32 bytes) {
    u64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (u64 p = base & ~(line - 1); p < base + bytes; p += line)
        asm volatile("dc cvac, %0" :: "r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
}
}
