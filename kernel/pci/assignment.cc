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
    volatile u32* reg = ecam_reg(g_hw.ecam_base, bus, device, function, kRegCommand);
    asm volatile("dsb sy" ::: "memory");
    u32 value = *reg;
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
    volatile u32* reg = ecam_reg(g_hw.ecam_base, bus, device, function, kRegCommand);
    asm volatile("dsb sy" ::: "memory");
    u32 value = *reg;
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

// ---------------------------------------------------------------------------
// Filtered guest config + mediation
// ---------------------------------------------------------------------------

CfgWriteAction classify_config_write(const GuestConfig& cfg, u32 fn_index, u32 offset, u32 size) {
    if (fn_index >= kMaxAssignFns || !cfg.present[fn_index]) {
        return CfgWriteAction::Reject;
    }
    if (size == 0 || size > 4 || offset >= kCfgSpaceBytes || (offset + size) > kCfgSpaceBytes) {
        return CfgWriteAction::Reject;
    }
    // The command register is always mediated: the caller decides which bits a
    // guest may actually change, and the bus-master bit only after ACTIVE.
    if (offset >= kRegCommand && offset < kRegCommand + 4) {
        return CfgWriteAction::Mediate;
    }
    // BAR relocation, the expansion ROM BAR and the capability pointer must
    // never be guest-controlled: moving a BAR would point the window at another
    // function's registers or at host memory (§6.3).
    if (offset >= kRegBar0 && offset < kRegBarEnd) {
        return CfgWriteAction::Reject;
    }
    if (offset >= kRegRomBar && offset < kRegRomBar + 4) {
        return CfgWriteAction::Reject;
    }
    if (offset >= kRegCapPtr && offset < kRegCapPtr + 4) {
        return CfgWriteAction::Reject;
    }
    // Interrupt line/pin and the cache-line/latency bytes are benign fields with
    // no isolation consequences.
    if (offset >= kRegIntLine && offset < 0x40) {
        return CfgWriteAction::Accept;
    }
    if (offset >= 0x0C && offset < kRegBar0) {
        return CfgWriteAction::Accept;
    }
    // MSI/MSI-X capability windows are mediated: the guest may program them,
    // but the value is bent into an Anko route, never a raw host MSI target.
    const u32 msi_off = cfg.msi_cap_off[fn_index];
    if (msi_off != 0 && offset >= msi_off && offset < msi_off + 24u) {
        return CfgWriteAction::Mediate;
    }
    // Anything else (device-specific read-only fields, power management, reset)
    // is rejected rather than blindly stored.
    return CfgWriteAction::Reject;
}

} // namespace pci
