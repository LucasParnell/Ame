#pragma once
// anko/panic_report.h — the panic report record and its renderers.
//
// One fixed-capacity record is filled from the faulting context, then rendered
// twice: a compact ASCII form for the QR code on screen, and a full form for the
// serial console. Everything is bounded and allocation-free so it is safe on a
// faulting CPU, and this module has no kernel-only dependencies (the caller
// supplies hardware facts), which keeps the renderers host-testable.

#include "anko/types.h"

namespace anko::panic_report {

constexpr u32 kMaxBacktrace = 16;
constexpr u32 kMaxQrPayload = 1536;   // bytes handed to the QR encoder
constexpr u32 kMaxSerialText = 4096;

struct Registers {
    u64 x[31];      // x0..x30 (x29 = fp, x30 = lr)
    u64 sp;
    u64 elr;
    u64 spsr;
    u64 esr;
    u64 far;
};

struct Hardware {
    bool present;       // hardware facts captured
    bool has_pcie;
    bool has_pci_mmio;
    bool has_smmu;
    u32  virtio_count;
    u64  pci_mmio_base;
    u64  pci_mmio_size;
    u64  ram_base;
    u64  ram_size;
    u32  boot_el;
};

struct Report {
    const char* title;      // "KERNEL PANIC"
    const char* reason;     // decoded trap, e.g. "data abort (EL1)"
    u32 core;
    Registers regs;
    u32 trace_count;
    u64 trace[kMaxBacktrace];
    Hardware hw;
    u32 cap_denials;        // rejected capability operations seen so far
    const char* build;      // build identifier (may be null)
};

// Fill the register record from the faulting context.
void capture_registers(Report& report, const u64* x, u64 sp, u64 elr, u64 spsr,
                       u64 esr, u64 far);

// Walk the AArch64 frame-pointer chain from `fp` into report.trace (bounded).
// Only pointers inside [lo, hi) are followed: the chain lives on the kernel
// stack in RAM, so restricting the walk to that window means the panic path
// never dereferences an attacker-shaped or stale frame pointer.
void capture_backtrace(Report& report, u64 fp, u64 lo, u64 hi);

// Compact line-oriented form for the QR code. Returns bytes written (excluding
// the NUL) and sets *truncated when the record did not fit.
u32 format_qr(const Report& report, char* out, u32 cap, bool* truncated);

// Full human-readable form for the serial console. Same truncation contract.
u32 format_serial(const Report& report, char* out, u32 cap, bool* truncated);

} // namespace anko::panic_report
