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
#include "anko/panic.h"
#include "libk/integer.h"
#include "libk/string.h"

namespace pci {

namespace {

// Config-space dword access uses pci::cfg_read/cfg_write (anko/pci.h), which
// mask every index, order against the device and treat a zero base as absent.

// Read-modify-write one command-register bit, preserving every other bit.  Uses
// the little-endian word helpers so the adjacent status register's W1C bits are
// never touched.
void set_command_bit(u8 bus, u8 device, u8 function, u16 mask, bool enable) {
    if (g_hw.ecam_base == 0) {
        return;
    }
    volatile u8* reg = reinterpret_cast<volatile u8*>(
        ecam_offset(g_hw.ecam_base, bus, device, function, kRegCommand));
    asm volatile("dsb sy" ::: "memory");
    u16 value = cfg_read_word(reg, 0);
    if (enable) {
        value |= mask;
    } else {
        value &= static_cast<u16>(~mask);
    }
    asm volatile("dsb sy" ::: "memory");
    cfg_write_word(reg, 0, value);
    asm volatile("dsb sy" ::: "memory");
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
    // Read-modify-write so only the bus-master bit changes. The ECAM window is
    // bootstrap-identity mapped and never reachable from a guest.
    set_command_bit(bus, device, function, kPciCmdBusMaster, enable);
}

void set_memory_enable(u8 bus, u8 device, u8 function, bool enable) {
    set_command_bit(bus, device, function, kPciCmdMemoryEnable, enable);
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

bool assign_bars(PciFunctionInfo* info, PhysAddr window_base, u64 window_size,
                 u64* cursor_inout) {
    if (!info || !cursor_inout || window_size == 0 || g_hw.ecam_base == 0) {
        return false;
    }
    u64 offset = *cursor_inout;
    for (u32 i = 0; i < kMaxBars; ++i) {
        PciBar& bar = info->bars[i];
        if (bar.size == 0) {
            continue;
        }
        if (bar.type != BarType::Mmio32 && bar.type != BarType::Mmio64) {
            continue;
        }
        // BAR apertures are powers of two; alignment and masking below rely on it.
        ANKO_ASSERT((bar.size & (bar.size - 1)) == 0);
        offset = libk::align_up(offset, bar.size);
        if (offset > window_size || bar.size > window_size - offset) {
            return false;
        }
        const PhysAddr base = window_base + offset;
        const u32 reg = kRegBar0 + 4u * i;
        // Only the address bits are writable; preserve the read-only type bits.
        const u32 low = cfg_read(g_hw.ecam_base, info->bus, info->device, info->function, reg);
        cfg_write(g_hw.ecam_base, info->bus, info->device, info->function, reg,
                  (static_cast<u32>(base) & kBarAddrMask) | (low & kBarTypeMask));
        if (bar.type == BarType::Mmio64) {
            cfg_write(g_hw.ecam_base, info->bus, info->device, info->function, reg + 4,
                      static_cast<u32>(base >> 32));
            ++i; // the upper half occupies the next BAR slot
        }
        bar.base = base;
        offset += bar.size;
    }
    *cursor_inout = offset;
    return true;
}

bool find_by_id(PhysAddr ecam_base, u16 vendor_id, u16 device_id, PciFunctionInfo* out) {
    static PciFunctionInfo functions[kMaxFunctions];
    const u32 count = enumerate(ecam_base, functions, kMaxFunctions);
    for (u32 i = 0; i < count; ++i) {
        if (functions[i].vendor_id == vendor_id && functions[i].device_id == device_id) {
            if (out) { *out = functions[i]; }
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
        // Guard the class: the record array is fixed at kMaxGroupFunctions, so a
        // topology that would exceed it must trip here, not silently inflate the
        // count and overflow every later consumer.
        ANKO_ASSERT(g->function_count < kMaxGroupFunctions);
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
