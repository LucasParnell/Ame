// kernel/drivers/pci_probe.cc — Temporary kernel-side PCI scan (M3.5 shortcut).
//
// TEMPORARY: Kernel scans PCIe config space to find qemu-xhci (class 0x0C0330),
// reads BAR0, and grants the MMIO range + an IRQ line to init via BootInfo.
//
// This shortcut will be replaced by the drv.pci userland service (M10) once
// the PCI ECAM CAP_MMIO grant path is established.
//
// Spec ref: PCI Local Bus Spec 3.0, §6.1 Configuration Space.

#include "anko/types.h"
#include "anko/cap.h"
#include "anko/mm.h"
#include "anko/log.h"
#include "anko/platform.h"

namespace drivers::pci_probe {

// QEMU virt PCIe ECAM base (from DTS: pcie@10000000 / reg).
constexpr PhysAddr kEcamBase = platform::kPciEcamBase;
// ECAM window: 256 buses × 32 devices × 8 fns × 4096 bytes
[[maybe_unused]] constexpr u64 kEcamSize = 256ull * 32 * 8 * 4096;

// Config space offsets.
constexpr u32 kCfgVendorId   = 0x00;
constexpr u32 kCfgCommand    = 0x04;
constexpr u32 kCfgClassCode  = 0x08; // [31:24]=class [23:16]=sub [15:8]=progif
constexpr u32 kCfgBar0       = 0x10;
constexpr u32 kCfgBar1       = 0x14;
constexpr u32 kCfgIntLine    = 0x3C;

// USB xHCI class code: class=0x0C sub=0x03 progif=0x30
constexpr u32 kClassXhci = 0x0C0330u;

static volatile u32* ecam_reg(u32 bus, u32 dev, u32 fn, u32 off) {
    const u64 addr = kEcamBase
        | (static_cast<u64>(bus) << 20)
        | (static_cast<u64>(dev) << 15)
        | (static_cast<u64>(fn)  << 12)
        | off;
    return reinterpret_cast<volatile u32*>(addr);
}

static u32 cfg_read(u32 bus, u32 dev, u32 fn, u32 off) {
    asm volatile("dsb sy" ::: "memory");
    return *ecam_reg(bus, dev, fn, off);
}

static void cfg_write(u32 bus, u32 dev, u32 fn, u32 off, u32 val) {
    *ecam_reg(bus, dev, fn, off) = val;
    asm volatile("dsb sy" ::: "memory");
}

struct XhciInfo {
    PhysAddr bar0_phys;
    u64      bar0_size;
    u32      irq_line;
    bool     found;
};

XhciInfo find_xhci() {
    XhciInfo result{};

    for (u32 bus = 0; bus < 16 && !result.found; ++bus) {
        for (u32 dev = 0; dev < 32 && !result.found; ++dev) {
            const u32 vendor = cfg_read(bus, dev, 0, kCfgVendorId);
            if ((vendor & 0xFFFFu) == 0xFFFF) continue; // no device

            const u32 class_full = cfg_read(bus, dev, 0, kCfgClassCode);
            const u32 class_code = (class_full >> 8) & 0xFFFFFFu;
            if (class_code != kClassXhci) continue;

            // Found xHCI — read BAR0 and handle both 32-bit and 64-bit MMIO BARs.
            const u32 bar0_orig = cfg_read(bus, dev, 0, kCfgBar0);
            const bool bar64 = (bar0_orig & 0x6u) == 0x4u;
            const u32 bar1_orig = bar64 ? cfg_read(bus, dev, 0, kCfgBar1) : 0;
            cfg_write(bus, dev, 0, kCfgBar0, 0xFFFF'FFFFu);
            if (bar64) cfg_write(bus, dev, 0, kCfgBar1, 0xFFFF'FFFFu);
            const u32 bar0_sz = cfg_read(bus, dev, 0, kCfgBar0);
            const u32 bar1_sz = bar64 ? cfg_read(bus, dev, 0, kCfgBar1) : 0;
            cfg_write(bus, dev, 0, kCfgBar0, bar0_orig); // restore
            if (bar64) cfg_write(bus, dev, 0, kCfgBar1, bar1_orig);

            PhysAddr bar0_base = static_cast<PhysAddr>(bar0_orig & ~0xFull);
            if (bar64) {
                bar0_base |= static_cast<PhysAddr>(bar1_orig) << 32;
            }
            const u64 size_mask = bar64
                ? ((static_cast<u64>(bar1_sz) << 32) |
                   static_cast<u64>(bar0_sz & ~0xFull))
                : static_cast<u64>(bar0_sz & ~0xFull);
            const u64 bar0_size = size_mask ? (~size_mask + 1ull) : 0;

            if (bar0_base == 0 && bar0_size != 0) {
                // Direct -kernel boots have no firmware to assign BARs. Use
                // the start of QEMU virt's 32-bit PCI MMIO aperture.
                bar0_base = 0x1000'0000ull;
                cfg_write(bus, dev, 0, kCfgBar0,
                          static_cast<u32>(bar0_base) | (bar0_orig & 0xFu));
                if (bar64) cfg_write(bus, dev, 0, kCfgBar1, 0);
            }

            // Enable memory decoding and bus mastering for DMA-capable xHCI.
            const u32 cmd_status = cfg_read(bus, dev, 0, kCfgCommand);
            cfg_write(bus, dev, 0, kCfgCommand, cmd_status | 0x6u);

            const u32 irq = cfg_read(bus, dev, 0, kCfgIntLine) & 0xFFu;

            ANKO_LOGF_INFO("pci_probe", "xhci found bus=%u dev=%u bar0=0x%lx size=0x%lx irq=%u",
                           bus, dev,
                           static_cast<unsigned long>(bar0_base),
                           static_cast<unsigned long>(bar0_size),
                           irq);
            result.bar0_phys = bar0_base;
            result.bar0_size = bar0_size;
            result.irq_line  = irq;
            result.found     = true;
        }
    }
    return result;
}

MmioRange* alloc_xhci_mmio(const XhciInfo& info) {
    if (!info.found) return nullptr;
    auto* range = static_cast<MmioRange*>(
        mm::heap_alloc(sizeof(MmioRange), alignof(MmioRange)));
    if (!range) return nullptr;
    range->phys_base   = info.bar0_phys;
    range->size        = info.bar0_size;
    range->attr        = MX_MMIO_DEVICE_nGnRnE;
    range->flags       = 0;
    range->device_hint = 0;
    range->region_index = 0;
    return range;
}

IrqLine* alloc_xhci_irq(const XhciInfo& info) {
    if (!info.found) return nullptr;
    auto* line = static_cast<IrqLine*>(
        mm::heap_alloc(sizeof(IrqLine), alignof(IrqLine)));
    if (!line) return nullptr;
    line->irq_id      = info.irq_line;
    line->trigger     = 1; // edge
    line->flags       = 0;
    line->device_hint = 0;
    line->doorbell_cap = 0;
    return line;
}

} // namespace drivers::pci_probe
