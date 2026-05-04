// kernel/panic.cc — kernel panic / bugcheck handler.
//
// Fires on EL1 synchronous faults (via vec_kernel_sync) and explicit
// panic()/panicf() calls.  Forces UART output regardless of ring state.

#include "anko/panic.h"
#include "anko/log.h"
#include "anko/types.h"

#include <cstdarg>

// ---------------------------------------------------------------------------
// EC decoder
// ---------------------------------------------------------------------------

static const char* ec_name(u32 ec) {
    switch (ec) {
    case 0x21: return "instruction abort (EL1)";
    case 0x22: return "PC alignment fault";
    case 0x25: return "data abort (EL1)";
    case 0x26: return "SP alignment fault";
    case 0x2f: return "SError";
    case 0x3c: return "breakpoint";
    default:   return "unknown";
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
// dump_frame — full register dump
// ---------------------------------------------------------------------------

static void dump_frame(TrapFrame* tf, u64 esr, u64 far) {
    const u32 ec  = static_cast<u32>((esr >> 26) & 0x3fu);
    const u32 iss = static_cast<u32>(esr & 0x00FFFFFFu);
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
    const u32 ec = static_cast<u32>((esr >> 26) & 0x3fu);
    anko::log::writef(anko::log::Level::Fatal, "panic",
                      "panic: %s", ec_name(ec));

    dump_frame(frame, esr, far);
    walk_frames(frame->x[29]);

    anko::log::write(anko::log::Level::Fatal, "panic",
                     "--- KERNEL PANIC: system halted ---");

    for (;;) asm volatile("wfe" ::: "memory");
}

// ---------------------------------------------------------------------------
// panic / panicf
// ---------------------------------------------------------------------------

[[noreturn]] void panic(const char* msg) {
    anko::log::writef(anko::log::Level::Fatal, "panic", "\npanic: %s", msg);

    u64 fp = 0;
    asm volatile("mov %0, x29" : "=r"(fp));
    walk_frames(fp);

    anko::log::write(anko::log::Level::Fatal, "panic",
                     "--- KERNEL PANIC: system halted ---");

    for (;;) asm volatile("wfe" ::: "memory");
}

[[noreturn]] void panicf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    anko::log::vrawf(fmt, ap);   // emit the formatted line
    va_end(ap);

    // Delegate to panic with a sentinel — walk + halt done there.
    // We can't call panic(buf) through the format path cleanly without
    // duplicating the buffer; instead just halt here with the same walk.
    (void)buf;

    u64 fp = 0;
    asm volatile("mov %0, x29" : "=r"(fp));
    walk_frames(fp);

    anko::log::write(anko::log::Level::Fatal, "panic",
                     "--- KERNEL PANIC: system halted ---");

    for (;;) asm volatile("wfe" ::: "memory");
}
