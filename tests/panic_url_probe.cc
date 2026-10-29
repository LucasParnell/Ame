// tests/panic_url_probe.cc — host harness for panic_report::format_url.
//
// Builds a report with known values, renders the QR URL and prints it. The
// matching test decodes it with tests/panic_bundle.py and checks every field.

#include <cstdio>

#include "anko/panic_report.h"

int main() {
    anko::panic_report::Report report{};
    report.title = "KERNEL PANIC";
    report.reason = "data abort (EL1)";
    report.core = 2;
    report.build = "ame-test-1.2.3";

    u64 x[31] = {};
    for (u32 i = 0; i < 31; ++i) x[i] = 0x1000ull + i;
    x[29] = 0xffff000000123500ull;
    x[30] = 0x0000000010001f20ull;
    anko::panic_report::capture_registers(report, x, 0xffff000000123400ull,
                                          0x00000000100018a4ull, 0x00000000600003c5ull,
                                          0x0000000096000045ull, 0x0000000000000042ull);

    report.trace_count = 3;
    report.trace[0] = 0x0000000010001f20ull;
    report.trace[1] = 0x0000000010001f60ull;
    report.trace[2] = 0x0000000010001fa0ull;

    report.hw.present = true;
    report.hw.has_pcie = true;
    report.hw.has_pci_mmio = true;
    report.hw.has_smmu = true;
    report.hw.virtio_count = 4;
    report.hw.boot_el = 2;
    report.hw.pci_mmio_base = 0x10000000ull;
    report.hw.pci_mmio_size = 0x2eff0000ull;
    report.hw.ram_base = 0x40000000ull;
    report.hw.ram_size = 0x20000000ull;
    report.cap_denials = 3;

    static char url[anko::panic_report::kUrlMax];
    bool truncated = false;
    const u32 len = anko::panic_report::format_url(report, url, sizeof(url), &truncated);
    if (truncated || len == 0) {
        std::fprintf(stderr, "format_url failed (len=%u truncated=%d)\n", len, truncated ? 1 : 0);
        return 1;
    }
    std::printf("%s\n", url);
    return 0;
}
