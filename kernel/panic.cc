// kernel/panic.cc — kernel panic / bugcheck handler.
//
// Fires on EL1 synchronous faults (via vec_kernel_sync) and explicit
// panic()/panicf() calls.  Forces UART output regardless of ring state.

#include "anko/panic.h"
#include "anko/panic_screen.h"
#include "anko/log.h"
#include "anko/types.h"
#include "anko/arm64_esr.h"

#include <cstdarg>

// ---------------------------------------------------------------------------
// EC decoder
// ---------------------------------------------------------------------------

static const char* ec_name(u32 ec) {
    switch (ec) {
    case static_cast<u32>(arm64::kEcInstrAbortCurrent): return "instruction abort (EL1)";
    case static_cast<u32>(arm64::kEcPcAlignFault):      return "PC alignment fault";
    case static_cast<u32>(arm64::kEcDataAbortCurrent):  return "data abort (EL1)";
    case static_cast<u32>(arm64::kEcSpAlignFault):      return "SP alignment fault";
    case static_cast<u32>(arm64::kEcSError):            return "SError";
    case static_cast<u32>(arm64::kEcBreakpoint):        return "breakpoint";
    default:                                             return "unknown";
    }
}

// ---------------------------------------------------------------------------
// FP chain walk
// ---------------------------------------------------------------------------

static void walk_frames(u64 fp) {
    anko::log::write(anko::log::Level::Fatal, "panic", "Call trace:");

    constexpr u64 kKernelVaBase = 0xffff000000000000ull;

    for (int i = 0; i < 16; ++i) {
        if (fp == 0) break;
        if (fp & 7u) break;
        if (fp < kKernelVaBase) break;

        // AArch64 frame record: [fp+0] = prev_fp, [fp+8] = lr
        const u64* record = reinterpret_cast<const u64*>(fp);
        const u64 lr = record[1];

        anko::log::writef(anko::log::Level::Fatal, "panic",
                          "  #%d  0x%llx", i, static_cast<unsigned long long>(lr));

        fp = record[0];
    }
}

// ---------------------------------------------------------------------------
// Shared epilogue: walk frames, paint the panic screen, then halt forever.
// ---------------------------------------------------------------------------

[[noreturn]] static void do_halt(u64 fp, const char* detail) {
    walk_frames(fp);
    anko::log::write(anko::log::Level::Fatal, "panic",
                     "--- KERNEL PANIC: system halted ---");
    // Visible even when the serial console is not attached.
    anko::panic_screen::show("KERNEL PANIC", detail);
    for (;;) asm volatile("wfe" ::: "memory");
}

// ---------------------------------------------------------------------------
// dump_frame — full register dump
// ---------------------------------------------------------------------------

static void dump_frame(TrapFrame* tf, u64 esr, u64 far) {
    const u32 ec  = static_cast<u32>(arm64::esr_ec(esr));
    const u32 iss = static_cast<u32>(arm64::esr_iss(esr));
    const char* name = ec_name(ec);

    anko::log::writef(anko::log::Level::Fatal, "panic",
                      "Fatal trap 0x%x: %s, ISS=0x%x", ec, name, iss);
    anko::log::writef(anko::log::Level::Fatal, "panic",
                      "ESR_EL1=0x%llx  FAR_EL1=0x%llx",
                      static_cast<unsigned long long>(esr),
                      static_cast<unsigned long long>(far));
    anko::log::writef(anko::log::Level::Fatal, "panic",
                      "ELR_EL1=0x%llx  SPSR_EL1=0x%llx",
                      static_cast<unsigned long long>(tf->elr),
                      static_cast<unsigned long long>(tf->spsr));

    // General-purpose registers in groups of 4
    for (int row = 0; row < 7; ++row) {
        int base = row * 4;
        anko::log::writef(anko::log::Level::Fatal, "panic",
                          " x%d=0x%llx  x%d=0x%llx  x%d=0x%llx  x%d=0x%llx",
                          base,   static_cast<unsigned long long>(tf->x[base]),
                          base+1, static_cast<unsigned long long>(tf->x[base+1]),
                          base+2, static_cast<unsigned long long>(tf->x[base+2]),
                          base+3, static_cast<unsigned long long>(tf->x[base+3]));
    }

    // x28, fp(x29), lr(x30)
    anko::log::writef(anko::log::Level::Fatal, "panic",
                      "x28=0x%llx  fp=0x%llx  lr=0x%llx",
                      static_cast<unsigned long long>(tf->x[28]),
                      static_cast<unsigned long long>(tf->x[29]),
                      static_cast<unsigned long long>(tf->x[30]));

    // sp_el0 slot holds pre-fault SP_EL1 in kernel fault context
    anko::log::writef(anko::log::Level::Fatal, "panic",
                      "sp=0x%llx",
                      static_cast<unsigned long long>(tf->sp_el0));
}

// ---------------------------------------------------------------------------
// kernel_sync_fault — called from vec_kernel_sync assembly
// ---------------------------------------------------------------------------

extern "C" [[noreturn]] void kernel_sync_fault(u64 esr, u64 far, TrapFrame* frame) {
    const u32 ec = static_cast<u32>(arm64::esr_ec(esr));
    anko::log::writef(anko::log::Level::Fatal, "panic",
                      "panic: %s", ec_name(ec));
    dump_frame(frame, esr, far);
    do_halt(frame->x[29], ec_name(ec)); // walk from saved FP of the trapped context
}

// ---------------------------------------------------------------------------
// panic / panicf
// ---------------------------------------------------------------------------

[[noreturn]] void panic(const char* msg) {
    anko::log::writef(anko::log::Level::Fatal, "panic", "\npanic: %s", msg);
    u64 fp = 0;
    asm volatile("mov %0, x29" : "=r"(fp));
    do_halt(fp, msg);
}

[[noreturn]] void panicf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_screen;
    va_copy(ap_screen, ap);
    anko::log::vrawf(fmt, ap);
    char detail[160];
    anko::log::vformat_to(detail, sizeof(detail), fmt, ap_screen);
    va_end(ap_screen);
    va_end(ap);
    u64 fp = 0;
    asm volatile("mov %0, x29" : "=r"(fp));
    do_halt(fp, detail);
}
