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

// What the QR code carries. The code has to stay small enough to sit in a
// corner at a scannable module size, so it holds the identifying subset (trap,
// the exception registers, the call/argument registers and a short trace); the
// serial dump carries the complete register file and the full trace.
constexpr u32 kQrRegisterCount = 8;   // x0..x7
constexpr u32 kQrBacktrace = 8;

// Where the QR URL points. The fragment carries the whole payload, so the host
// is a placeholder for a decoder page and can be changed freely at build time.
constexpr const char* kQrUrlPrefix = "https://ame.example/p#";
constexpr u32 kBundleMax = 512;       // packed body budget
constexpr u32 kUrlMax = 1024;         // URL budget

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

// Compact line-oriented form for the QR code's caption: labelled, ASCII,
// positional, no `0x` prefixes, and bounded to the fields listed above. Returns
// bytes written (excluding the NUL) and sets *truncated when it did not fit.
u32 format_qr(const Report& report, char* out, u32 cap, bool* truncated);

// Machine-readable form for the QR code itself: a base64url URL whose fragment
// is an LZMA-compressed binary bundle of the whole record (falling back to an
// uncompressed bundle when compression would not pay). Bundle layout, so a
// decoder can be written against it:
//
//   bundle := header body_or_stream
//   header := magic 'A','P' | version u8 (=1) | flags u8 (bit0 compressed)
//             | raw_len u16 LE (length of the packed body)
//   body   := core u8 | reason u8_len + bytes
//             | esr,far,elr,spsr,sp,fp,lr as u64 LE
//             | reg_count u8 | x0..x{reg_count-1} as u64 LE
//             | bt_count u8 | bt[] as u64 LE
//             | hw_flags u8 | virtio u8 | boot_el u8
//             | pci_base,pci_size,ram_base,ram_size as u64 LE | cap_denials u32 LE
//             | build u8_len + bytes
//
// The stream is raw LZMA1 (lc=3, lp=0, pb=2, dict_size=4096) with no end marker;
// header.raw_len tells the decoder how much to expect.
u32 format_url(const Report& report, char* out, u32 cap, bool* truncated);

// Full human-readable form for the serial console. Same truncation contract.
u32 format_serial(const Report& report, char* out, u32 cap, bool* truncated);

} // namespace anko::panic_report
