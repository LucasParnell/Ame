// kernel/panic.cc — kernel panic / bugcheck handler.
//
// Fires on EL1 synchronous faults (via vec_kernel_sync) and explicit
// panic()/panicf() calls. Builds one report record, dumps the full report to the
// retained console, paints the panic screen (QR code + logo + title), then keeps
// the core alive indefinitely serving the watchdog. The other CPUs keep
// running: a panic is a stop for the faulting core, not a silent spin.
//
// Two invariants this file has to hold on the Huawei board in particular:
//
//  1. The panic path must outlive the SP805 watchdog the bootloader armed with
//     RESEN set. A `wfi` loop is reset by hardware, not by software, so the
//     handler takes ownership of the timer (anne::watchdog::hold/feed) instead of
//     parking and hoping.
//  2. Nothing may preempt the report. claim_or_park() masks DAIF, not just IRQ:
//     on this SoC the watchdog interrupt arrives as FIQ, and a report truncated
//     by the very event it is reporting is worse than no report.

#include "anko/panic.h"
#include "anko/exception_vectors.h"
#include "anko/panic_screen.h"
#include "anko/panic_report.h"
#include "anko/cap.h"
#include "anko/hw_info.h"
#include "anko/log.h"
#include "anko/types.h"
#include "anko/arm64_esr.h"

#if defined(ANKO_BOARD_HUAWEI_ANNE) && defined(ANKO_ANNE_WATCHDOG)
#include "watchdog.h"
#define ANKO_PANIC_HAS_WATCHDOG 1
#endif

#include <cstdarg>

#ifndef ANKO_BUILD_DESCRIBE
#define ANKO_BUILD_DESCRIBE "unknown"
#endif

// Static storage: the report, the rendered text and the QR payload must never
// live on the stack of a faulting thread.
static anko::panic_report::Report g_report;
static char g_serial_text[anko::panic_report::kMaxSerialText];
static char g_caption[anko::panic_report::kMaxCaptionText];
static char g_qr_payload[anko::panic_report::kUrlMax];

// First CPU to panic publishes the report and paints the screen; any other CPU
// that panics parks immediately so it cannot clobber the record mid-render.
static u32 g_panic_owner = 0;

// Set by claim_or_park when the SP805 had already fired. A FIQ that turns out to
// be the watchdog expiry is named in the report instead of being reported as an
// anonymous "unexpected exception".
static bool g_watchdog_fired = false;

// ---------------------------------------------------------------------------
// Platform: watchdog ownership and a bounded delay for the halt loop
// ---------------------------------------------------------------------------

#ifdef ANKO_PANIC_HAS_WATCHDOG
static void panic_watchdog_take() {
    g_watchdog_fired = anne::watchdog::absorb_interrupt();
    anne::watchdog::hold();
}

static void panic_watchdog_feed() {
    anne::watchdog::feed();
}
#else
static void panic_watchdog_take() {
}
static void panic_watchdog_feed() {
}
#endif

// Busy-wait on the virtual counter. Used by the halt loop so the feed cadence is
// time-based rather than instruction-based: `wfi` alone is unsafe here, because
// if nothing wakes the core the SP805 keeps counting and hardware resets it.
static void spin_micros(u32 us) {
    u64 freq = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0u) {
        // No usable counter: fall back to a bounded instruction delay rather than
        // returning instantly, which would turn the halt loop into a hot spin on
        // the watchdog registers.
        for (u32 i = 0; i < (1u << 20); ++i)
            asm volatile("yield" ::: "memory");
        return;
    }
    u64 start = 0;
    asm volatile("mrs %0, cntvct_el0" : "=r"(start));
    u64 ticks = (freq / 1000000ull) * us;
    if (ticks == 0u)
        ticks = 1u;
    u64 now = start;
    while ((now - start) < ticks)
        asm volatile("mrs %0, cntvct_el0" : "=r"(now));
}

// ---------------------------------------------------------------------------
// EC decoder
// ---------------------------------------------------------------------------

static const char* ec_name(u32 ec) {
    switch (ec) {
    case static_cast<u32>(arm64::kEcInstrAbortCurrent):
        return "instruction abort (EL1)";
    case static_cast<u32>(arm64::kEcPcAlignFault):
        return "PC alignment fault";
    case static_cast<u32>(arm64::kEcDataAbortCurrent):
        return "data abort (EL1)";
    case static_cast<u32>(arm64::kEcSpAlignFault):
        return "SP alignment fault";
    case static_cast<u32>(arm64::kEcSError):
        return "SError";
    case static_cast<u32>(arm64::kEcBreakpoint):
        return "breakpoint";
    default:
        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Report capture
// ---------------------------------------------------------------------------

static void capture_hardware() {
    g_report.hw.present = true;
    g_report.hw.has_pcie = g_hw.has_pcie;
    g_report.hw.has_pci_mmio = g_hw.has_pci_mmio;
    g_report.hw.has_smmu = g_hw.has_smmu;
    g_report.hw.virtio_count = g_hw.virtio_count;
    g_report.hw.pci_mmio_base = g_hw.pci_mmio_base;
    g_report.hw.pci_mmio_size = g_hw.pci_mmio_size;
    g_report.hw.ram_base = g_hw.ram_base;
    g_report.hw.ram_size = g_hw.ram_size;
    g_report.hw.boot_el = g_hw.boot_el;
    g_report.cap_denials = static_cast<u32>(cap::denial_count());
    g_report.build = ANKO_BUILD_DESCRIBE;
}

static void capture_current_registers() {
    // Snapshot x0..x30 with paired stores so the register file is captured as a
    // block rather than one hand-picked value at a time.
    u64 regs[31] = {};
    asm volatile("stp x0,  x1,  [%0]\n"
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

// The first CPU to panic owns the record; any other CPU parks immediately so it
// cannot overwrite the report while it is being rendered. DAIF is masked in full:
// masking only IRQ would leave the watchdog FIQ (and SError) free to truncate the
// report halfway through.
static void claim_or_park() {
    if (__atomic_exchange_n(&g_panic_owner, 1u, __ATOMIC_ACQ_REL) != 0u) {
        asm volatile("msr daifset, #15" ::: "memory");
        // Another core is already producing the report; leave a breadcrumb on the
        // wire (the screen belongs to the first report) and park this core.
        anko::log::panic_dump("(panic already in progress on another core; this core halted)\n");
        for (;;) {
            // Never park blind: this core may be the only one left feeding.
            panic_watchdog_feed();
            spin_micros(50000u);
        }
    }
    asm volatile("msr daifset, #15" ::: "memory");
    // Take the timer before any formatting: the deadline was armed at boot, not
    // when this function was called.
    panic_watchdog_take();
}

// Everything below runs with the record exclusively owned. Nothing on this path
// takes a lock, allocates, or calls the log ring: the report reaches the retained
// console through log::panic_dump only.
[[noreturn]] static void halt_and_report(const char* reason, u64 fp) {
    g_report.title = "KERNEL PANIC";
    g_report.reason = reason ? reason : "unknown";
    {
        u64 core = 0;
        asm volatile("mrs %0, TPIDR_EL1" : "=r"(core));
        g_report.core = static_cast<u32>(core);
    }
    capture_hardware();

    // The frame chain lives on the kernel stack in RAM; restrict the walk to
    // that window so a stale or corrupt x29 cannot fault inside the handler.
    if (g_hw.ram_size != 0) {
        anko::panic_report::capture_backtrace(g_report, fp, g_hw.ram_base,
                                              g_hw.ram_base + g_hw.ram_size);
    }

    bool truncated = false;
    const u32 serial_len = anko::panic_report::format_serial(g_report, g_serial_text,
                                                             sizeof(g_serial_text), &truncated);
    (void)serial_len;
    anko::log::panic_dump(g_serial_text);
    if (truncated)
        anko::log::panic_dump("(serial report truncated)\n");

    // State the watchdog verdict explicitly: "no expiry" is as useful as a hit
    // when the next boot is being diffed against this one.
#ifdef ANKO_PANIC_HAS_WATCHDOG
    anko::log::panic_dump(g_watchdog_fired ? "watchdog: SP805 interrupt had fired\n"
                                           : "watchdog: no expiry observed\n");
#endif

    bool caption_truncated = false;
    const u32 caption_len = anko::panic_report::format_caption(
        g_report, g_caption, sizeof(g_caption), &caption_truncated);
    (void)caption_len;
    bool url_truncated = false;
    const u32 url_len = anko::panic_report::format_url(g_report, g_qr_payload, sizeof(g_qr_payload),
                                                       &url_truncated);
    (void)url_len;
    if (caption_truncated || url_truncated) {
        anko::log::panic_dump("(panic screen: report truncated)\n");
    }

    // The screen fill is a multi-megabyte write on a 1080x2280 panel; feed the
    // watchdog between bands so the paint cannot outlive the deadline.
    anko::panic_screen::set_progress_hook(&panic_watchdog_feed);
    if (!anko::panic_screen::show(g_report.title, g_report.reason, g_caption, g_qr_payload)) {
        anko::log::panic_dump(
            anko::panic_screen::bound()
                ? "(panic screen: QR not drawn; report too large or screen too small)\n"
                : "(panic screen: no framebuffer bound)\n");
    }

    // Halt only this core, but keep the timer fed. Recovery is a machine reset,
    // which is external — and now deliberate rather than a watchdog accident.
    for (;;) {
        panic_watchdog_feed();
        spin_micros(50000u);
    }
}

// ---------------------------------------------------------------------------
// kernel_sync_fault — called from vec_kernel_sync assembly
// ---------------------------------------------------------------------------

extern "C" [[noreturn]] void kernel_sync_fault(u64 esr, u64 far, TrapFrame* frame) {
    claim_or_park();
    const u32 ec = static_cast<u32>(arm64::esr_ec(esr));
    anko::panic_report::capture_registers(g_report, frame->x, frame->sp_el0, frame->elr,
                                          frame->spsr, esr, far);
    halt_and_report(ec_name(ec), frame->x[29]);
}

// ---------------------------------------------------------------------------
// Unexpected exception vectors (see anko/exception_vectors.h). An SError or an
// unmasked FIQ used to spin silently; now it produces the same report.
// ---------------------------------------------------------------------------

extern "C" [[noreturn]] void kernel_unexpected_exception(u64 vector_index, u64 esr, u64 far,
                                                         TrapFrame* frame) {
    claim_or_park();
    anko::panic_report::capture_registers(g_report, frame->x, frame->sp_el0, frame->elr,
                                          frame->spsr, esr, far);
    const char* reason = vector_index < anko::kExceptionVectorCount
                             ? anko::kExceptionVectorReason[vector_index]
                             : "unexpected exception";
    // Vector 6 is "current EL with SPx, FIQ" — on this board that is the SP805
    // watchdog expiry, and naming it turns a mystery reset into a diagnosis.
    if (vector_index == 6u && g_watchdog_fired)
        reason = "watchdog FIQ (SP805 expired)";
    halt_and_report(reason, frame->x[29]);
}

// ---------------------------------------------------------------------------
// panic / panicf
// ---------------------------------------------------------------------------

[[noreturn]] void panic(const char* msg) {
    claim_or_park();
    capture_current_registers();
    halt_and_report(msg ? msg : "panic", g_report.regs.x[29]);
}

[[noreturn]] void panicf(const char* fmt, ...) {
    claim_or_park();
    static char message[anko::panic_report::kMaxSerialText];
    va_list ap;
    va_start(ap, fmt);
    anko::log::vformat_to(message, sizeof(message), fmt, ap);
    va_end(ap);
    capture_current_registers();
    halt_and_report(message, g_report.regs.x[29]);
}
