// kernel/panic_report.cc — panic report capture and rendering.
//
// Pure formatting: no allocation, no kernel-only headers, bounded output. The
// same record produces the QR payload (compact) and the serial dump (full), so
// the two can never disagree about what happened.

#include "anko/panic_report.h"

#include "anko/lzma.h"

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
    void hex16_bare(u64 value) {
        static const char* digits = "0123456789abcdef";
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

// Bounded length of a NUL-terminated string (null-safe).
static u32 slen(const char* s) {
    u32 n = 0;
    while (s && s[n]) ++n;
    return n;
}

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
    // Compact, positional, `0x`-free: the same field order every time so a scan
    // can be read without a schema. The full register file and trace are on the
    // serial line; see panic_report.h for the split.
    o.str("AMEQR1 core=");
    o.dec(report.core);
    o.str(" build=");
    o.str(report.build ? report.build : "unknown");
    o.ch('\n');

    o.str("reason=");
    o.str(report.reason ? report.reason : "unknown");
    o.ch('\n');

    o.str("esr=");
    o.hex16_bare(report.regs.esr);
    o.str(" far=");
    o.hex16_bare(report.regs.far);
    o.str(" elr=");
    o.hex16_bare(report.regs.elr);
    o.ch('\n');

    o.str("spsr=");
    o.hex16_bare(report.regs.spsr);
    o.str(" sp=");
    o.hex16_bare(report.regs.sp);
    o.str(" fp=");
    o.hex16_bare(report.regs.x[29]);
    o.str(" lr=");
    o.hex16_bare(report.regs.x[30]);
    o.ch('\n');

    for (u32 i = 0; i < kQrRegisterCount; ++i) {
        o.ch('x');
        o.dec(i);
        o.ch('=');
        o.hex16_bare(report.regs.x[i]);
        o.ch(i + 1u == kQrRegisterCount ? '\n' : ' ');
    }

    o.str("bt=");
    o.dec(report.trace_count);
    for (u32 i = 0; i < report.trace_count && i < kQrBacktrace; ++i) {
        o.ch(' ');
        o.hex16_bare(report.trace[i]);
    }
    o.ch('\n');

    if (report.hw.present) {
        o.str("hw=");
        o.str(flag(report.hw.has_pcie));
        o.str(flag(report.hw.has_pci_mmio));
        o.str(flag(report.hw.has_smmu));
        o.str(" virtio=");
        o.dec(report.hw.virtio_count);
        o.str(" el=");
        o.dec(report.hw.boot_el);
        o.ch('\n');
        o.str("pci=");
        o.hex16_bare(report.hw.pci_mmio_base);
        o.ch('+');
        o.hex16_bare(report.hw.pci_mmio_size);
        o.str(" ram=");
        o.hex16_bare(report.hw.ram_base);
        o.ch('+');
        o.hex16_bare(report.hw.ram_size);
        o.ch('\n');
    }
    o.str("cap=");
    o.dec(report.cap_denials);
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

// --- machine-readable bundle + URL -----------------------------------------

namespace {

struct ByteWriter {
    u8* buf;
    u32 cap;
    u32 len = 0;
    bool over = false;
    void put(u32 value) {
        if (len < cap) {
            buf[len] = static_cast<u8>(value);
        } else {
            over = true;
        }
        ++len;
    }
    void u16le(u32 value) {
        put(value & 0xFFu);
        put((value >> 8) & 0xFFu);
    }
    void u32le(u32 value) {
        for (int i = 0; i < 4; ++i) put((value >> (8 * i)) & 0xFFu);
    }
    void u64le(u64 value) {
        for (int i = 0; i < 8; ++i) put(static_cast<u32>((value >> (8 * i)) & 0xFFu));
    }
    void cstr(const char* s) {
        for (; s && *s; ++s) put(static_cast<u32>(static_cast<u8>(*s)));
    }
};

const char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Base64url, unpadded. Returns characters written.
u32 base64url(const u8* in, u32 len, char* out, u32 cap) {
    u32 o = 0;
    u32 i = 0;
    while (i + 3u <= len) {
        const u32 v = (static_cast<u32>(in[i]) << 16) |
                      (static_cast<u32>(in[i + 1]) << 8) | static_cast<u32>(in[i + 2]);
        if (o + 4u > cap) break;
        out[o++] = kBase64Url[(v >> 18) & 0x3F];
        out[o++] = kBase64Url[(v >> 12) & 0x3F];
        out[o++] = kBase64Url[(v >> 6) & 0x3F];
        out[o++] = kBase64Url[v & 0x3F];
        i += 3;
    }
    if (i + 1u == len && o + 2u <= cap) {
        const u32 v = static_cast<u32>(in[i]) << 16;
        out[o++] = kBase64Url[(v >> 18) & 0x3F];
        out[o++] = kBase64Url[(v >> 12) & 0x3F];
    } else if (i + 2u == len && o + 3u <= cap) {
        const u32 v = (static_cast<u32>(in[i]) << 16) | (static_cast<u32>(in[i + 1]) << 8);
        out[o++] = kBase64Url[(v >> 18) & 0x3F];
        out[o++] = kBase64Url[(v >> 12) & 0x3F];
        out[o++] = kBase64Url[(v >> 6) & 0x3F];
    }
    return o;
}

// Pack the record into the documented binary body.
u32 pack_body(const Report& report, u8* out, u32 cap, bool* truncated) {
    ByteWriter w{out, cap};
    w.put(report.core);
    const char* reason = report.reason ? report.reason : "";
    const u32 reason_len = slen(reason) > 255u ? 255u : slen(reason);
    w.put(reason_len);
    for (u32 i = 0; i < reason_len; ++i) w.put(static_cast<u32>(static_cast<u8>(reason[i])));

    w.u64le(report.regs.esr);
    w.u64le(report.regs.far);
    w.u64le(report.regs.elr);
    w.u64le(report.regs.spsr);
    w.u64le(report.regs.sp);
    w.u64le(report.regs.x[29]);
    w.u64le(report.regs.x[30]);

    w.put(kQrRegisterCount);
    for (u32 i = 0; i < kQrRegisterCount; ++i) w.u64le(report.regs.x[i]);

    const u32 frames =
        report.trace_count < kQrBacktrace ? report.trace_count : kQrBacktrace;
    w.put(frames);
    for (u32 i = 0; i < frames; ++i) w.u64le(report.trace[i]);

    u32 flags = 0;
    if (report.hw.present) {
        flags |= report.hw.has_pcie ? 1u : 0u;
        flags |= report.hw.has_pci_mmio ? 2u : 0u;
        flags |= report.hw.has_smmu ? 4u : 0u;
    }
    w.put(flags);
    w.put(report.hw.virtio_count & 0xFFu);
    w.put(report.hw.boot_el);
    w.u64le(report.hw.pci_mmio_base);
    w.u64le(report.hw.pci_mmio_size);
    w.u64le(report.hw.ram_base);
    w.u64le(report.hw.ram_size);
    w.u32le(report.cap_denials);

    const char* build = report.build ? report.build : "";
    const u32 build_len = slen(build) > 255u ? 255u : slen(build);
    w.put(build_len);
    for (u32 i = 0; i < build_len; ++i) w.put(static_cast<u32>(static_cast<u8>(build[i])));

    if (truncated) *truncated = w.over;
    return w.over ? 0 : w.len;
}

} // namespace

u32 format_url(const Report& report, char* out, u32 cap, bool* truncated) {
    static u8 body[kBundleMax];
    static u8 blob[kBundleMax + 8u];
    static u8 comp[1024];

    bool body_truncated = false;
    const u32 body_len = pack_body(report, body, sizeof(body), &body_truncated);
    if (body_len == 0) {
        if (truncated) *truncated = true;
        if (cap) out[0] = '\0';
        return 0;
    }

    // LZMA only when it actually pays for its header.
    const u32 comp_len = anko::lzma::compress(body, body_len, comp, sizeof(comp));
    const bool use_lzma = comp_len > 0 && comp_len + 4u < body_len;

    ByteWriter h{blob, sizeof(blob)};
    h.put('A');
    h.put('P');
    h.put(1);                                  // version
    h.put(use_lzma ? 1u : 0u);                 // flags: bit0 = LZMA
    h.u16le(body_len);
    const u8* payload = use_lzma ? comp : body;
    const u32 payload_len = use_lzma ? comp_len : body_len;
    for (u32 i = 0; i < payload_len; ++i) h.put(payload[i]);

    Out o{out, cap};
    o.str(kQrUrlPrefix);
    u32 written = o.bytes();
    const u32 room = cap > written ? cap - written : 0u;
    written += base64url(blob, h.len, out + written, room);
    if (written < cap) out[written] = '\0';
    if (truncated) *truncated = body_truncated || h.over || written >= cap;
    return written;
}

} // namespace anko::panic_report
