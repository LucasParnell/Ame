#pragma once
#include "anko/arm64_esr.h"

namespace arm64 {
struct PhysicalTimerAccess { u32 reg, rt; bool read; };
// CNTP_{TVAL,CTL,CVAL}_EL0: S3_3_C14_C2_{0,1,2}. The host retains
// the hardware CNTP timer; these trapped guest registers are software state.
inline bool decode_physical_timer(u64 esr, PhysicalTimerAccess& out) {
    const u64 iss = esr_iss(esr);
    const u32 reg = (iss >> 17) & 7;
    if (esr_ec(esr) != kEcSysReg || ((iss >> 20) & 3) != 3 ||
        ((iss >> 14) & 7) != 3 || ((iss >> 10) & 15) != 14 ||
        ((iss >> 1) & 15) != 2 || reg > 2) return false;
    out = {reg, static_cast<u32>((iss >> 5) & 31), bool(iss & 1)};
    return true;
}
inline u64 physical_timer_read(u32 reg, u64 control, u64 compare, u64 now) {
    if (reg == 0) return static_cast<u32>(compare - now);
    if (reg == 1) return (control & 3) | ((control & 1) && now >= compare ? 4 : 0);
    return compare;
}
inline void physical_timer_write(u32 reg, u64 value, u64 now, u64& control, u64& compare) {
    if (reg == 0) compare = now + static_cast<i64>(static_cast<i32>(value));
    else if (reg == 1) control = value & 3;
    else compare = value;
}
} // namespace arm64
