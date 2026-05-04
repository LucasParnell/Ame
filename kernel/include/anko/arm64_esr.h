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

} // namespace arm64
