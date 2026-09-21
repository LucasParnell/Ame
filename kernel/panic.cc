// kernel/panic.cc — kernel panic / bugcheck handler.
//
// Fires on EL1 synchronous faults (via vec_kernel_sync) and explicit
// panic()/panicf() calls. Builds one report record, dumps the full report to the
// UART, paints the panic screen (QR code + logo + title), then halts this CPU
// and waits for a machine reset (Ctrl-Alt-Del / power). The other CPUs keep
// running: a panic is a stop for the faulting core, not a silent spin.

#include "anko/panic.h"
#include "anko/panic_screen.h"
#include "anko/panic_report.h"
#include "anko/cap.h"
#include "anko/hw_info.h"
#include "anko/log.h"
#include "anko/types.h"
#include "anko/arm64_esr.h"

#include <cstdarg>

#ifndef ANKO_BUILD_DESCRIBE
#define ANKO_BUILD_DESCRIBE "unknown"
#endif

// Static storage: the report, the rendered text and the QR payload must never
// live on the stack of a faulting thread.
static anko::panic_report::Report g_report;
static char g_serial_text[anko::panic_report::kMaxSerialText];
static char g_qr_payload[anko::panic_report::kMaxQrPayload];

// First CPU to panic publishes the report and paints the screen; any other CPU
// that panics parks immediately so it cannot clobber the record mid-render.
static u32 g_panic_owner = 0;

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
// Report capture
// ---------------------------------------------------------------------------

static void capture_hardware() {
    g_report.hw.present     = true;
    g_report.hw.has_pcie    = g_hw.has_pcie;
    g_report.hw.has_pci_mmio = g_hw.has_pci_mmio;
    g_report.hw.has_smmu    = g_hw.has_smmu;
    g_report.hw.virtio_count = g_hw.virtio_count;
    g_report.hw.pci_mmio_base = g_hw.pci_mmio_base;
    g_report.hw.pci_mmio_size = g_hw.pci_mmio_size;
    g_report.hw.ram_base    = g_hw.ram_base;
    g_report.hw.ram_size    = g_hw.ram_size;
    g_report.hw.boot_el     = g_hw.boot_el;
    g_report.cap_denials    = static_cast<u32>(cap::denial_count());
    g_report.build          = ANKO_BUILD_DESCRIBE;
}

static void capture_current_registers() {
    // Snapshot x0..x30 with paired stores so the register file is captured as a
    // block rather than one hand-picked value at a time.
    u64 regs[31] = {};
    asm volatile(
        "stp x0,  x1,  [%0]\n"
        "stp x2,  x3,  [%0, #16]\n"
        "stp x4,  x5,  [%0, #32]\n"
        "stp x6,  x7,  [%0, #48]\n"
        "stp x8,  x9,  [%0, #64]\n"
        "stp x10, x11, [%0, #80]\n"
        "stp x12, x13, [%0, #96]\n"
        "stp x14, x15, [%0, #112]\n"
        "stp x16, x17, [%0, #128]\n"
        "stp x18, x19, [%0, #144]\n"
        "stp x20, x21, [%0, #160]\n"
        "stp x22, x23, [%0, #176]\n"
        "stp x24, x25, [%0, #192]\n"
        "stp x26, x27, [%0, #208]\n"
        "stp x28, x29, [%0, #224]\n"
        "str x30, [%0, #240]\n"
        :
        : "r"(regs)
        : "memory");
    u64 sp = 0;
    asm volatile("mov %0, sp" : "=r"(sp));
    anko::panic_report::capture_registers(g_report, regs, sp, regs[30], 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Shared epilogue: publish the report, paint the screen, halt this CPU.
// ---------------------------------------------------------------------------

[[noreturn]] static void halt_with(const char* reason, u64 fp) {
    if (__atomic_exchange_n(&g_panic_owner, 1u, __ATOMIC_ACQ_REL) != 0u) {
        for (;;) asm volatile("wfi" ::: "memory");
    }
    g_report.title  = "KERNEL PANIC";
    g_report.reason = reason ? reason : "unknown";
    g_report.core   = 0; // TPIDR_EL1 holds the boot CPU id
    {
        u64 core = 0;
        asm volatile("mrs %0, TPIDR_EL1" : "=r"(core));
        g_report.core = static_cast<u32>(core);
    }
    capture_hardware();
    anko::panic_report::capture_backtrace(g_report, fp);

    bool truncated = false;
    (void)anko::panic_report::format_serial(g_report, g_serial_text,
                                            sizeof(g_serial_text), &truncated);
    anko::log::panic_dump(g_serial_text);
    if (truncated) {
        anko::log::write(anko::log::Level::Fatal, "panic", "(serial report truncated)");
    }

    bool qr_truncated = false;
    (void)anko::panic_report::format_qr(g_report, g_qr_payload,
                                        sizeof(g_qr_payload), &qr_truncated);
    anko::panic_screen::show(g_report.title, g_report.reason, g_qr_payload);

    anko::log::write(anko::log::Level::Fatal, "panic",
                     "--- system halted: reset (Ctrl-Alt-Del / system_reset) to reboot ---");
    // Halt only this core. Interrupts stay masked so the report is never
    // clobbered; recovery is a machine reset, which is an external event.
    for (;;) asm volatile("wfi" ::: "memory");
}

// ---------------------------------------------------------------------------
// kernel_sync_fault — called from vec_kernel_sync assembly
// ---------------------------------------------------------------------------

extern "C" [[noreturn]] void kernel_sync_fault(u64 esr, u64 far, TrapFrame* frame) {
    const u32 ec = static_cast<u32>(arm64::esr_ec(esr));
    const char* reason = ec_name(ec);
    anko::panic_report::capture_registers(g_report, frame->x, frame->sp_el0,
                                          frame->elr, frame->spsr, esr, far);
    halt_with(reason, frame->x[29]);
}

// ---------------------------------------------------------------------------
// panic / panicf
// ---------------------------------------------------------------------------

[[noreturn]] void panic(const char* msg) {
    capture_current_registers();
    halt_with(msg ? msg : "panic", g_report.regs.x[29]);
}

[[noreturn]] void panicf(const char* fmt, ...) {
    static char message[anko::panic_report::kMaxSerialText];
    va_list ap;
    va_start(ap, fmt);
    va_list ap_screen;
    va_copy(ap_screen, ap);
    anko::log::vrawf(fmt, ap);
    anko::log::vformat_to(message, sizeof(message), fmt, ap_screen);
    va_end(ap_screen);
    va_end(ap);
    capture_current_registers();
    halt_with(message, g_report.regs.x[29]);
}
