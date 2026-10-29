#pragma once
// anko/arm64_esr.h — ARM64 ESR_EL1/ESR_EL2 exception class constants.
// Shared between panic.cc (EL1 fault decoding) and vm/vcpu.cc (EL2 exit decoding).

#include "anko/types.h"

namespace arm64 {

// Exception class field (ESR bits [31:26]).
constexpr u64 kEcWfx                = 0x01; // WFI/WFE trapped
constexpr u64 kEcHvc64              = 0x16; // HVC from AArch64
constexpr u64 kEcSmc64              = 0x17; // SMC from AArch64
constexpr u64 kEcSysReg             = 0x18; // MSR/MRS/SYS instruction
constexpr u64 kEcInstrAbortLower    = 0x20; // instruction abort from lower EL
constexpr u64 kEcInstrAbortCurrent  = 0x21; // instruction abort from current EL
constexpr u64 kEcPcAlignFault       = 0x22; // PC alignment fault
constexpr u64 kEcDataAbortLower     = 0x24; // data abort from lower EL
constexpr u64 kEcDataAbortCurrent   = 0x25; // data abort from current EL
constexpr u64 kEcSpAlignFault       = 0x26; // SP alignment fault
constexpr u64 kEcSError             = 0x2f; // SError interrupt
constexpr u64 kEcBreakpoint         = 0x3c; // breakpoint / watchpoint / BRK

// Extract EC and ISS fields from a raw ESR value.
inline u64 esr_ec(u64 esr)  { return (esr >> 26) & 0x3Full; }
inline u64 esr_iss(u64 esr) { return esr & 0x01FF'FFFFull; }

struct MmioAccess {
    u32 size;
    u32 reg;
    bool write;
    bool sign_extend;
    bool wide;
};

// Only emulate scalar AArch64 accesses with a valid syndrome and a stage-2
// translation fault. Table walks, cache maintenance and external aborts must
// reach the normal fault path rather than accidentally completing a store.
inline bool decode_mmio(u64 esr, MmioAccess* out) {
    const u32 fault = esr & 0x3f;
    if (!out || esr_ec(esr) != kEcDataAbortLower || !(esr & (1ull << 25)) ||
        !(esr & (1ull << 24)) || (esr & ((1ull << 10) | (1ull << 8) | (1ull << 7))) || fault < 4 ||
        fault > 7) {
        return false;
    }
    *out = {1u << ((esr >> 22) & 3), static_cast<u32>((esr >> 16) & 31), bool(esr & (1ull << 6)),
            bool(esr & (1ull << 21)), bool(esr & (1ull << 15))};
    return !(out->size == 8 && (!out->wide || out->sign_extend)) &&
           !(out->write && out->sign_extend);
}

inline u64 mmio_load_value(const MmioAccess& access, u64 value) {
    const u64 mask = access.size == 8 ? ~0ull : (1ull << (access.size * 8)) - 1;
    value &= mask;
    if (access.sign_extend && (value & (1ull << (access.size * 8 - 1)))) {
        value |= ~mask;
    }
    return access.wide ? value : value & 0xffffffffull;
}

} // namespace arm64
