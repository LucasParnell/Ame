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

// Configuration-space register offsets come from anko/pci.h.  ECAM only permits
// naturally aligned accesses, so the revision (0x08) and header-type (0x0E)
// bytes are read as part of their enclosing 32-bit word.
constexpr u32 kCfgRevisionClass = 0x08; // [0x0B:0x08] revision + class code
constexpr u32 kCfgHeaderWord = 0x0C;     // [0x0F:0x0C] cache/latency + header type

// USB xHCI class code: the one PCI function Ame drives natively, so its
// bus-master enablement is left to drv.usb.xhci.
constexpr u32 kClassXhci = 0x0C0330u;

// The bootstrap identity map covers only the first kPciEcamProbeSize bytes of
// ECAM (16 buses × 32 devices × 8 functions × 4 KiB).  Stay inside it.
constexpr u32 kScanBuses = 16;
constexpr u32 kScanDevices = 32;
constexpr u32 kScanFunctions = 8;

// Config-space dword access uses pci::cfg_read/cfg_write (anko/pci.h) so every
// consumer masks and orders ECAM accesses identically.

// Size one BAR at `off`, writing ~0 and restoring the original value so the
// device is left exactly as it was found.  `is_mmio64` selects whether the
// upper 32-bit half must be sized alongside.  Returns the number of 32-bit
// slots consumed (1 or 2) and fills `out`.
u32 probe_bar(PhysAddr ecam_base, u32 bus, u32 dev, u32 fn, u32 off, PciBar& out) {
    const u32 orig = cfg_read(ecam_base, bus, dev, fn, off);
    cfg_write(ecam_base, bus, dev, fn, off, kBarProbeValue);
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
        cfg_write(ecam_base, bus, dev, fn, off_hi, kBarProbeValue);
        const u32 mask_hi = cfg_read(ecam_base, bus, dev, fn, off_hi);
        cfg_write(ecam_base, bus, dev, fn, off_hi, orig_hi);

        size_mask = (static_cast<u64>(mask_hi) << 32) | (mask & kBarAddrMask);
        bar_base = (static_cast<u64>(orig_hi) << 32) | (orig & kBarAddrMask);
        type = BarType::Mmio64;
        prefetchable = (orig & 0x8u) != 0;
        consumed = 2;
    } else {
        size_mask = mask & kBarAddrMask;
        bar_base = orig & kBarAddrMask;
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
        const u32 off = kRegBar0 + i * 4u;
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

static u32 scan_functions(PhysAddr ecam_base, PciFunctionInfo* out, u32 cap) {
    if (ecam_base == 0 || out == nullptr || cap == 0) {
        return 0;
    }

    u32 count = 0;
    for (u32 bus = 0; bus < kScanBuses; ++bus) {
        for (u32 dev = 0; dev < kScanDevices; ++dev) {
            const u32 vd = cfg_read(ecam_base, bus, dev, 0, kRegVendorId);
            const u16 vendor = static_cast<u16>(vd & 0xFFFFu);
            if (vendor == kInvalidVendor) {
                continue; // no device at this slot
            }

            // Header type bit 7 marks a multi-function device.
            const u32 dev_header = cfg_read(ecam_base, bus, dev, 0, kCfgHeaderWord);
            const u32 functions =
                ((dev_header >> 16) & kHeaderTypeMultiFunction) != 0 ? kScanFunctions : 1u;

            for (u32 fn = 0; fn < functions; ++fn) {
                const u32 vd_fn = cfg_read(ecam_base, bus, dev, fn, kRegVendorId);
                const u16 vf = static_cast<u16>(vd_fn & 0xFFFFu);
                if (vf == kInvalidVendor) {
                    continue;
                }
                if (count >= cap) {
                    return count;
                }

                const u32 fn_header = cfg_read(ecam_base, bus, dev, fn, kCfgHeaderWord);
                const u8 header_type = static_cast<u8>((fn_header >> 16) & kHeaderTypeMask);

                PciFunctionInfo rec{};
                rec.segment = kSegment;
                rec.bus = static_cast<u8>(bus);
                rec.device = static_cast<u8>(dev);
                rec.function = static_cast<u8>(fn);
                rec.vendor_id = vf;
                rec.device_id = static_cast<u16>(vd_fn >> 16);
                rec.class_code =
                    (cfg_read(ecam_base, bus, dev, fn, kCfgRevisionClass) >> 8) & 0x00FF'FFFFu;
                rec.command = static_cast<u16>(cfg_read(ecam_base, bus, dev, fn, kRegCommand));
                rec.bus_master = (rec.command & kPciCmdBusMaster) != 0;
                rec.memory_enabled = (rec.command & kPciCmdMemoryEnable) != 0;
                rec.requester_id = requester_id(rec.bus, rec.device, rec.function);
                probe_bars(ecam_base, bus, dev, fn, header_type, rec.bars);

                out[count++] = rec;
            }
        }
    }
    return count;
}

u32 enumerate(PhysAddr ecam_base, PciFunctionInfo* out, u32 cap) {
    static PciFunctionInfo inventory[kMaxFunctions];
    static u32 count;
    static bool ready;
    if (!ecam_base || !out || !cap) return 0;
    // Boot discovers this fixed QEMU topology before any native driver runs.
    // Re-probing BAR sizes later would temporarily relocate a live native BAR.
    if (!ready) {
        count = scan_functions(ecam_base, inventory, kMaxFunctions);
        ready = true;
    }
    const u32 written = count < cap ? count : cap;
    for (u32 i = 0; i < written; ++i) {
        auto record = inventory[i];
        record.command = cfg_read(ecam_base, record.bus, record.device, record.function, kRegCommand);
        record.bus_master = (record.command & kPciCmdBusMaster) != 0;
        record.memory_enabled = (record.command & kPciCmdMemoryEnable) != 0;
        for (u32 b = 0; b < kMaxBars; ++b) {
            auto& bar = record.bars[b];
            if (!bar.size) continue;
            const u32 low = cfg_read(ecam_base, record.bus, record.device, record.function, kRegBar0 + b * 4);
            bar.base = low & (bar.type == BarType::Io ? ~3u : kBarAddrMask);
            if (bar.type == BarType::Mmio64 && b + 1 < kMaxBars) {
                bar.base |= u64(cfg_read(ecam_base, record.bus, record.device, record.function,
                                        kRegBar0 + (b + 1) * 4)) << 32;
                ++b;
            }
        }
        out[i] = record;
    }
    return written;
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

u32 contain_assignable(PhysAddr ecam_base) {
    if (ecam_base == 0) {
        return 0;
    }
    static PciFunctionInfo functions[kMaxFunctions];
    const u32 count = enumerate(ecam_base, functions, kMaxFunctions);
    u32 contained = 0;
    for (u32 i = 0; i < count; ++i) {
        const PciFunctionInfo& f = functions[i];
        // The native xHCI function keeps the bus-master enablement its driver
        // relies on, and PCI-to-PCI bridges/root ports are not assignable
        // devices, so neither is touched.  Every other function is held
        // non-bus-mastering until an assignment is fully constructed.
        const u32 base_class = (f.class_code >> 16) & 0xFFu;
        if (f.class_code == kClassXhci || base_class == 0x06u) {
            continue;
        }
        const u32 command = cfg_read(ecam_base, f.bus, f.device, f.function, kRegCommand);
        const u32 contained_cmd = command & ~static_cast<u32>(kPciCmdBusMaster);
        if (contained_cmd == command) {
            continue;
        }
        cfg_write(ecam_base, f.bus, f.device, f.function, kRegCommand, contained_cmd);
        ++contained;
    }
    return contained;
}

} // namespace pci
