// kernel/panic_report.cc — panic report capture and rendering.
//
// Pure formatting: no allocation, no kernel-only headers, bounded output. The
// same record produces the QR payload (compact) and the serial dump (full), so
// the two can never disagree about what happened.

#include "anko/panic_report.h"

namespace anko::panic_report {
namespace {

struct Out {
    char* buf;
    u32 cap;
    u32 len = 0;
    bool truncated = false;

    void ch(char c) {
        if (len + 1 < cap) {
            buf[len++] = c;
        } else {
            truncated = true;
        }
    }
    void str(const char* s) {
        for (; s && *s; ++s) ch(*s);
    }
    void hex16(u64 value) {
        static const char* digits = "0123456789abcdef";
        ch('0');
        ch('x');
        for (int shift = 60; shift >= 0; shift -= 4) {
            ch(digits[(value >> shift) & 0xF]);
        }
    }
    void dec(u32 value) {
        char tmp[10];
        u32 n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        } while (value != 0 && n < sizeof(tmp));
        while (n) ch(tmp[--n]);
    }
    void finish() {
        if (cap != 0) buf[len] = '\0';
    }
    u32 bytes() const { return len; }
};

const char* flag(bool value) { return value ? "1" : "0"; }

void label(Out& out, const char* name) {
    out.str(name);
    out.ch('=');
}

// Register names stay positional (x0..x30) so a photo can be read in order.
void write_register(Out& out, u32 index, u64 value) {
    out.ch('x');
    out.dec(index);
    out.ch('=');
    out.hex16(value);
}

} // namespace

void capture_registers(Report& report, const u64* x, u64 sp, u64 elr, u64 spsr,
                       u64 esr, u64 far) {
    for (u32 i = 0; i < 31; ++i) report.regs.x[i] = x ? x[i] : 0;
    report.regs.sp = sp;
    report.regs.elr = elr;
    report.regs.spsr = spsr;
    report.regs.esr = esr;
    report.regs.far = far;
}

void capture_backtrace(Report& report, u64 fp, u64 lo, u64 hi) {
    report.trace_count = 0;
    for (u32 i = 0; i < kMaxBacktrace; ++i) {
        // Stay inside a caller-supplied window (kernel RAM) and require 8-byte
        // alignment: outside it a frame record cannot be trusted, and following
        // it could fault inside the panic handler.
        if (fp < lo || fp + 16u > hi || (fp & 7u) != 0) break;
        const u64* record = reinterpret_cast<const u64*>(fp);
        const u64 next = record[0];
        report.trace[report.trace_count++] = record[1];
        fp = next;
    }
}

u32 format_qr(const Report& report, char* out, u32 cap, bool* truncated) {
    Out o{out, cap};
    o.str(report.title ? report.title : "KERNEL PANIC");
    o.ch(' ');
    o.str("v1 core=");
    o.dec(report.core);
    o.ch('\n');

    label(o, "build");
    o.str(report.build ? report.build : "unknown");
    o.ch('\n');

    label(o, "reason");
    o.str(report.reason ? report.reason : "unknown");
    o.ch('\n');

    label(o, "esr");
    o.hex16(report.regs.esr);
    o.ch(' ');
    label(o, "far");
    o.hex16(report.regs.far);
    o.ch(' ');
    label(o, "elr");
    o.hex16(report.regs.elr);
    o.ch('\n');

    label(o, "spsr");
    o.hex16(report.regs.spsr);
    o.ch(' ');
    label(o, "sp");
    o.hex16(report.regs.sp);
    o.ch(' ');
    label(o, "fp");
    o.hex16(report.regs.x[29]);
    o.ch(' ');
    label(o, "lr");
    o.hex16(report.regs.x[30]);
    o.ch('\n');

    if (report.hw.present) {
        o.str("hw pcie=");
        o.str(flag(report.hw.has_pcie));
        o.str(" mmio=");
        o.str(flag(report.hw.has_pci_mmio));
        o.str(" smmu=");
        o.str(flag(report.hw.has_smmu));
        o.str(" virtio=");
        o.dec(report.hw.virtio_count);
        o.str(" el=");
        o.dec(report.hw.boot_el);
        o.ch('\n');
        o.str("pci win=");
        o.hex16(report.hw.pci_mmio_base);
        o.ch('+');
        o.hex16(report.hw.pci_mmio_size);
        o.str(" ram=");
        o.hex16(report.hw.ram_base);
        o.ch('+');
        o.hex16(report.hw.ram_size);
        o.ch('\n');
    }
    o.str("cap_denied=");
    o.dec(report.cap_denials);
    o.ch('\n');

    for (u32 i = 0; i < 31; i += 4) {
        for (u32 j = 0; j < 4 && i + j < 31; ++j) {
            if (j) o.ch(' ');
            write_register(o, i + j, report.regs.x[i + j]);
        }
        o.ch('\n');
    }

    o.str("bt");
    for (u32 i = 0; i < report.trace_count; ++i) {
        o.ch(' ');
        o.hex16(report.trace[i]);
    }
    o.ch('\n');
    o.str("bt_n=");
    o.dec(report.trace_count);
    o.ch('\n');

    o.finish();
    if (truncated) *truncated = o.truncated;
    return o.bytes();
}

u32 format_serial(const Report& report, char* out, u32 cap, bool* truncated) {
    Out o{out, cap};
    o.str("\n=== ");
    o.str(report.title ? report.title : "KERNEL PANIC");
    o.str(" ===\n");
    label(o, "reason");
    o.str(report.reason ? report.reason : "unknown");
    o.ch('\n');
    label(o, "build");
    o.str(report.build ? report.build : "unknown");
    o.str(" core=");
    o.dec(report.core);
    o.ch('\n');

    label(o, "esr_el1");
    o.hex16(report.regs.esr);
    o.ch('\n');
    label(o, "far_el1");
    o.hex16(report.regs.far);
    o.ch('\n');
    label(o, "elr_el1");
    o.hex16(report.regs.elr);
    o.ch('\n');
    label(o, "spsr_el1");
    o.hex16(report.regs.spsr);
    o.ch('\n');
    label(o, "sp");
    o.hex16(report.regs.sp);
    o.ch('\n');

    for (u32 i = 0; i < 31; ++i) {
        write_register(o, i, report.regs.x[i]);
        o.ch((i % 3 == 2) ? '\n' : ' ');
    }
    o.ch('\n');

    o.str("call trace (");
    o.dec(report.trace_count);
    o.str(" frames)\n");
    for (u32 i = 0; i < report.trace_count; ++i) {
        o.ch('#');
        o.dec(i);
        o.ch(' ');
        o.hex16(report.trace[i]);
        o.ch('\n');
    }

    if (report.hw.present) {
        o.str("hardware:\n");
        o.str("  pcie=");
        o.str(flag(report.hw.has_pcie));
        o.str(" pci_mmio=");
        o.str(flag(report.hw.has_pci_mmio));
        o.ch(' ');
        o.hex16(report.hw.pci_mmio_base);
        o.ch('+');
        o.hex16(report.hw.pci_mmio_size);
        o.ch('\n');
        o.str("  smmu=");
        o.str(flag(report.hw.has_smmu));
        o.str(" virtio=");
        o.dec(report.hw.virtio_count);
        o.str(" boot_el=");
        o.dec(report.hw.boot_el);
        o.ch('\n');
        o.str("  ram=");
        o.hex16(report.hw.ram_base);
        o.ch('+');
        o.hex16(report.hw.ram_size);
        o.ch('\n');
    }
    o.str("capability denials=");
    o.dec(report.cap_denials);
    o.ch('\n');
    o.str("--- system halted; reset to reboot ---\n");

    o.finish();
    if (truncated) *truncated = o.truncated;
    return o.bytes();
}

} // namespace anko::panic_report
