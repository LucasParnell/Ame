// kernel/pci/enumerate.cc — ECAM-based PCI function enumeration (Phase A).
//
// Walks the PCIe ECAM hierarchy for segment 0 and fills capability-ready
// PciFunctionInfo records: BDF, vendor/device id, class code, the BARs, and the
// requester/stream id derived from the BDF.  The PCI command register is read
// (bus-master / memory-enable bits) but never written here — the native xHCI
// path still owns bus-master enablement until Phase D.
//
// Spec ref: PCI Local Bus Spec 3.0 §6.1 (configuration space), §6.2.5.1 (BARs).

#include "anko/pci.h"
#include "anko/log.h"

namespace pci {

namespace {

// Configuration space register offsets (PCI Local Bus Spec 3.0 §6.1).
// ECAM only permits naturally aligned 32-bit accesses, so vendor/device are
// read as one word at 0x00.
constexpr u32 kCfgVendorDevice = 0x00;
constexpr u32 kCfgCommand = 0x04;
constexpr u32 kCfgClassCode = 0x08;
constexpr u32 kCfgHeaderTyp = 0x0C; // [23:16] = header type byte
constexpr u32 kCfgBar0 = 0x10;

// Command register bits we report but never modify.
constexpr u16 kCmdMemoryEnable = 1u << 1;
constexpr u16 kCmdBusMaster = 1u << 2;

// The bootstrap identity map covers only the first kPciEcamProbeSize bytes of
// ECAM (16 buses × 32 devices × 8 functions × 4 KiB).  Stay inside it.
constexpr u32 kScanBuses = 16;
constexpr u32 kScanDevices = 32;
constexpr u32 kScanFunctions = 8;

// Config-space word accessor.  All indices are masked so a malformed record
// can never address outside the ECAM window.
volatile u32* ecam_reg(PhysAddr ecam_base, u32 bus, u32 dev, u32 fn, u32 off) {
    const u64 addr = ecam_base | (static_cast<u64>(bus & 0xFFu) << 20) |
                     (static_cast<u64>(dev & 0x1Fu) << 15) | (static_cast<u64>(fn & 0x07u) << 12) |
                     (off & 0xFFFu);
    return reinterpret_cast<volatile u32*>(addr);
}

u32 cfg_read(PhysAddr base, u32 bus, u32 dev, u32 fn, u32 off) {
    asm volatile("dsb sy" ::: "memory");
    return *ecam_reg(base, bus, dev, fn, off);
}

void cfg_write(PhysAddr base, u32 bus, u32 dev, u32 fn, u32 off, u32 val) {
    *ecam_reg(base, bus, dev, fn, off) = val;
    asm volatile("dsb sy" ::: "memory");
}

// Size one BAR at `off`, writing ~0 and restoring the original value so the
// device is left exactly as it was found.  `is_mmio64` selects whether the
// upper 32-bit half must be sized alongside.  Returns the number of 32-bit
// slots consumed (1 or 2) and fills `out`.
u32 probe_bar(PhysAddr ecam_base, u32 bus, u32 dev, u32 fn, u32 off, PciBar& out) {
    const u32 orig = cfg_read(ecam_base, bus, dev, fn, off);
    cfg_write(ecam_base, bus, dev, fn, off, 0xFFFF'FFFFu);
    const u32 mask = cfg_read(ecam_base, bus, dev, fn, off);
    cfg_write(ecam_base, bus, dev, fn, off, orig);

    const bool is_io = ((orig | mask) & 0x1u) != 0;
    const bool is_mmio64 = !is_io && (((orig | mask) & 0x6u) == 0x4u);

    u64 size_mask = 0;
    u64 bar_base = 0;
    BarType type = BarType::None;
    bool prefetchable = false;
    u32 consumed = 1;

    if (is_io) {
        size_mask = mask & ~0x3u;
        bar_base = orig & ~0x3u;
        type = BarType::Io;
    } else if (is_mmio64) {
        const u32 off_hi = off + 4u;
        const u32 orig_hi = cfg_read(ecam_base, bus, dev, fn, off_hi);
        cfg_write(ecam_base, bus, dev, fn, off_hi, 0xFFFF'FFFFu);
        const u32 mask_hi = cfg_read(ecam_base, bus, dev, fn, off_hi);
        cfg_write(ecam_base, bus, dev, fn, off_hi, orig_hi);

        size_mask = (static_cast<u64>(mask_hi) << 32) | (mask & ~0xFu);
        bar_base = (static_cast<u64>(orig_hi) << 32) | (orig & ~0xFu);
        type = BarType::Mmio64;
        prefetchable = (orig & 0x8u) != 0;
        consumed = 2;
    } else {
        size_mask = mask & ~0xFu;
        bar_base = orig & ~0xFu;
        type = BarType::Mmio32;
        prefetchable = (orig & 0x8u) != 0;
    }

    out = PciBar{};
    if (size_mask != 0) {
        // Isolate the lowest set bit: the alignment/size of the region.  This is
        // width-correct for both 32- and 64-bit BARs.
        out.base = bar_base;
        out.size = size_mask & (~size_mask + 1ull);
        out.type = type;
        out.prefetchable = prefetchable;
    }
    return consumed;
}

// Size the BARs of a function.  Header type 0 has six BARs; a PCI-to-PCI bridge
// (header type 1) has only two (offsets 0x10 and 0x14).  Other header types
// have none.  Writing to a bridge's other registers would corrupt its windows,
// so we only touch the BAR slots the header actually defines.
void probe_bars(PhysAddr ecam_base, u32 bus, u32 dev, u32 fn, u8 header_type, PciBar* bars) {
    const u32 nbars = (header_type == 0u) ? kMaxBars : (header_type == 1u ? 2u : 0u);
    u32 i = 0;
    while (i < nbars) {
        const u32 off = kCfgBar0 + i * 4u;
        const u32 consumed = probe_bar(ecam_base, bus, dev, fn, off, bars[i]);
        i += (consumed == 2u && i + 1 < nbars) ? 2u : 1u;
    }
}

} // namespace

u32 requester_id(u8 bus, u8 device, u8 function) {
    return (static_cast<u32>(bus) << 8) | (static_cast<u32>(device & 0x1Fu) << 3) |
           static_cast<u32>(function & 0x07u);
}

const char* bar_type_name(BarType type) {
    switch (type) {
    case BarType::Io:
        return "io";
    case BarType::Mmio32:
        return "mmio32";
    case BarType::Mmio64:
        return "mmio64";
    case BarType::None:
    default:
        return "none";
    }
}

u32 enumerate(PhysAddr ecam_base, PciFunctionInfo* out, u32 cap) {
    if (ecam_base == 0 || out == nullptr || cap == 0) {
        return 0;
    }

    u32 count = 0;
    for (u32 bus = 0; bus < kScanBuses; ++bus) {
        for (u32 dev = 0; dev < kScanDevices; ++dev) {
            const u32 vd = cfg_read(ecam_base, bus, dev, 0, kCfgVendorDevice);
            const u16 vendor = static_cast<u16>(vd & 0xFFFFu);
            if (vendor == kInvalidVendor) {
                continue; // no device at this slot
            }

            // Header type bit 7 marks a multi-function device.
            const u32 dev_header = cfg_read(ecam_base, bus, dev, 0, kCfgHeaderTyp);
            const u32 functions = ((dev_header >> 16) & 0x80u) != 0 ? kScanFunctions : 1u;

            for (u32 fn = 0; fn < functions; ++fn) {
                const u32 vd_fn = cfg_read(ecam_base, bus, dev, fn, kCfgVendorDevice);
                const u16 vf = static_cast<u16>(vd_fn & 0xFFFFu);
                if (vf == kInvalidVendor) {
                    continue;
                }
                if (count >= cap) {
                    return count;
                }

                const u32 fn_header = cfg_read(ecam_base, bus, dev, fn, kCfgHeaderTyp);
                const u8 header_type = static_cast<u8>((fn_header >> 16) & 0x7Fu);

                PciFunctionInfo rec{};
                rec.segment = kSegment;
                rec.bus = static_cast<u8>(bus);
                rec.device = static_cast<u8>(dev);
                rec.function = static_cast<u8>(fn);
                rec.vendor_id = vf;
                rec.device_id = static_cast<u16>(vd_fn >> 16);
                rec.class_code =
                    (cfg_read(ecam_base, bus, dev, fn, kCfgClassCode) >> 8) & 0x00FF'FFFFu;
                rec.command = static_cast<u16>(cfg_read(ecam_base, bus, dev, fn, kCfgCommand));
                rec.bus_master = (rec.command & kCmdBusMaster) != 0;
                rec.memory_enabled = (rec.command & kCmdMemoryEnable) != 0;
                rec.requester_id = requester_id(rec.bus, rec.device, rec.function);
                probe_bars(ecam_base, bus, dev, fn, header_type, rec.bars);

                out[count++] = rec;
            }
        }
    }
    return count;
}

void log_inventory(PhysAddr ecam_base) {
    static PciFunctionInfo functions[kMaxFunctions];
    const u32 count = enumerate(ecam_base, functions, kMaxFunctions);

    ANKO_LOGF_INFO("pci", "inventory: %u function(s) via ECAM 0x%llx", count,
                   static_cast<unsigned long long>(ecam_base));
    for (u32 i = 0; i < count; ++i) {
        const PciFunctionInfo& f = functions[i];
        ANKO_LOGF_INFO(
            "pci", "  bdf %x:%x.%u vendor=%x device=%x class=%x reqid=%x cmd=%x bm=%u mem=%u",
            f.bus, f.device, f.function, f.vendor_id, f.device_id, f.class_code, f.requester_id,
            f.command, f.bus_master ? 1u : 0u, f.memory_enabled ? 1u : 0u);
        for (u32 b = 0; b < kMaxBars; ++b) {
            const PciBar& bar = f.bars[b];
            if (bar.size == 0) {
                continue;
            }
            ANKO_LOGF_INFO("pci", "    bar%u %s base=0x%llx size=0x%llx prefetch=%u", b,
                           bar_type_name(bar.type), static_cast<unsigned long long>(bar.base),
                           static_cast<unsigned long long>(bar.size), bar.prefetchable ? 1u : 0u);
        }
    }
}

} // namespace pci
