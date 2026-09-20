// kernel/pci/assignment.cc — PCI assignment: isolation groups, BAR ownership,
// filtered guest config, and config-write mediation (Phase D).
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §6.1 (assignment object), §6.2 (MMIO),
// §6.3 (isolation groups, filtered config, mediation), §8.13 (IOMMU grouping).
//
// This file is the PCI-specific half of the assignment layer. It owns:
//   * isolation-group derivation from the Phase A inventory — assignment is at
//     the group level, so a device that cannot be isolated is the unit;
//   * the only host config-space writes the assignment layer performs (bus
//     master, memory decode), and only from the §8.11 / §8.12 state machines;
//   * the synthetic/filtered guest configuration view containing only assigned
//     functions, and the policy that classifies a guest config write as
//     acceptable, mediated, or rejected.
//
// The outer ECAM is never mapped into a guest, and a guest write can never move
// a BAR, reprogram power/reset, or turn on host bus mastering except through the
// mediated paths the assignment object owns.

#include "anko/pci.h"

#include "anko/hw_info.h"
#include "libk/string.h"

namespace pci {

namespace {

volatile u32* ecam_reg(PhysAddr ecam_base, u8 bus, u8 dev, u8 fn, u32 off) {
    const u64 addr = ecam_base | (static_cast<u64>(bus) << 20) |
                     (static_cast<u64>(dev & 0x1Fu) << 15) | (static_cast<u64>(fn & 0x07u) << 12) |
                     (off & 0xFFFu);
    return reinterpret_cast<volatile u32*>(addr);
}

u32 cfg_read(PhysAddr base, u8 bus, u8 dev, u8 fn, u32 off) {
    if (base == 0) {
        return 0xFFFF'FFFFu;
    }
    asm volatile("dsb sy" ::: "memory");
    return *ecam_reg(base, bus, dev, fn, off);
}

} // namespace

// ---------------------------------------------------------------------------
// Host config access
// ---------------------------------------------------------------------------

u16 read_command(u8 bus, u8 device, u8 function) {
    return static_cast<u16>(cfg_read(g_hw.ecam_base, bus, device, function, kRegCommand) & 0xFFFFu);
}

u8 cfg_read_byte(u8 bus, u8 device, u8 function, u32 off) {
    const u32 word = cfg_read(g_hw.ecam_base, bus, device, function, off & ~3u);
    return static_cast<u8>((word >> ((off & 3u) * 8u)) & 0xFFu);
}

u16 cfg_status_word(u8 bus, u8 device, u8 function) {
    // Status lives in the upper half of the word at 0x04 (PCI 3.0 §6.2.2).
    return static_cast<u16>((cfg_read(g_hw.ecam_base, bus, device, function, kRegCommand) >> 16) &
                            0xFFFFu);
}

void set_bus_master(u8 bus, u8 device, u8 function, bool enable) {
    if (g_hw.ecam_base == 0) {
        return;
    }
    // Read-modify-write so only the bus-master bit changes. The ECAM window is
    // bootstrap-identity mapped and never reachable from a guest.
    // Use a halfword store: the adjacent status register contains W1C bits.
    volatile u16* reg = reinterpret_cast<volatile u16*>(
        ecam_reg(g_hw.ecam_base, bus, device, function, kRegCommand));
    asm volatile("dsb sy" ::: "memory");
    u16 value = *reg;
    if (enable) {
        value |= 1u << 2;
    } else {
        value &= ~(1u << 2);
    }
    *reg = value;
    asm volatile("dsb sy" ::: "memory");
}

void set_memory_enable(u8 bus, u8 device, u8 function, bool enable) {
    if (g_hw.ecam_base == 0) {
        return;
    }
    // Use a halfword store: the adjacent status register contains W1C bits.
    volatile u16* reg = reinterpret_cast<volatile u16*>(
        ecam_reg(g_hw.ecam_base, bus, device, function, kRegCommand));
    asm volatile("dsb sy" ::: "memory");
    u16 value = *reg;
    if (enable) {
        value |= 1u << 1;
    } else {
        value &= ~(1u << 1);
    }
    *reg = value;
    asm volatile("dsb sy" ::: "memory");
}

// ---------------------------------------------------------------------------
// Inventory lookups / isolation groups
// ---------------------------------------------------------------------------

bool find_function(PhysAddr ecam_base, u8 bus, u8 device, u8 function, PciFunctionInfo* out) {
    if (!out) {
        return false;
    }
    static PciFunctionInfo functions[kMaxFunctions];
    const u32 count = enumerate(ecam_base, functions, kMaxFunctions);
    for (u32 i = 0; i < count; ++i) {
        const PciFunctionInfo& f = functions[i];
        if (f.bus == bus && f.device == device && f.function == function) {
            *out = f;
            return true;
        }
    }
    return false;
}

u32 enumerate_groups(PhysAddr ecam_base, PciIsolationGroup* out, u32 cap) {
    if (!out || cap == 0) {
        return 0;
    }
    static PciFunctionInfo functions[kMaxFunctions];
    const u32 count = enumerate(ecam_base, functions, kMaxFunctions);
    u32 groups = 0;
    for (u32 i = 0; i < count; ++i) {
        const PciFunctionInfo& f = functions[i];
        const u32 gid = isolation_group_id(f.segment, f.bus, f.device);
        PciIsolationGroup* g = nullptr;
        for (u32 j = 0; j < groups; ++j) {
            if (out[j].group_id == gid) {
                g = &out[j];
                break;
            }
        }
        if (!g) {
            if (groups >= cap) {
                continue;
            }
            g = &out[groups++];
            libk::memset(g, 0, sizeof(*g));
            g->group_id = gid;
            g->segment = f.segment;
            g->bus = f.bus;
            g->device = f.device;
            g->vendor_id = f.vendor_id;
            g->device_id = f.device_id;
            g->class_code = f.class_code;
        }
        if (f.function < 32u) {
            g->function_mask |= (1u << f.function);
        }
        if (g->function_count < kMaxGroupFunctions) {
            g->requester_ids[g->function_count] = f.requester_id;
        }
        ++g->function_count;
    }
    return groups;
}

bool isolation_group_of(PhysAddr ecam_base, u8 bus, u8 device, PciIsolationGroup* out) {
    if (!out) {
        return false;
    }
    static PciIsolationGroup groups[pci::kMaxFunctions];
    const u32 count = enumerate_groups(ecam_base, groups, pci::kMaxFunctions);
    const u32 want = isolation_group_id(kSegment, bus, device);
    for (u32 i = 0; i < count; ++i) {
        if (groups[i].group_id == want) {
            *out = groups[i];
            return true;
        }
    }
    return false;
}

} // namespace pci
