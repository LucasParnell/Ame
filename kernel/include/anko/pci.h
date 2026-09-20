#pragma once
// anko/pci.h — capability-ready PCI function records produced by ECAM enumeration.
//
// Phase A: kernel-side inventory of the PCIe hierarchy. enumerate() walks the
// ECAM window and fills PciFunctionInfo records with everything a later
// capability grant needs: BDF, vendor/device id, class code, the six BARs, and
// the requester/stream id derived from the BDF.
//
// The PCI command register is *observed* (bus-master / memory-enable bits) but
// never written here.  The native xHCI path still owns bus-master enablement
// until Phase D; enforcement is out of scope for this inventory.
//
// Spec ref: PCI Local Bus Spec 3.0 §6.1 (configuration space), §6.2.5.1 (BARs).

#include "anko/types.h"

namespace pci {

// Number of base address registers in a type-0 configuration header.
constexpr u32 kMaxBars = 6;

// A present device reports a vendor id other than all-ones.
constexpr u16 kInvalidVendor = 0xFFFFu;

// Segment of the single ECAM window QEMU virt exposes (linux,pci-domain = 0).
constexpr u16 kSegment = 0;

// Upper bound on functions captured in one inventory pass.  The QEMU virt
// topologies exercised by the harness expose a handful; 64 leaves headroom.
constexpr u32 kMaxFunctions = 64;

// BAR address kind, derived from the read-only low bits of the register.
enum class BarType : u8 { None = 0, Io = 1, Mmio32 = 2, Mmio64 = 3 };

struct PciBar {
    PhysAddr base; // current base address (0 when left unassigned)
    u64 size;      // decoded size in bytes (0 when unimplemented)
    BarType type;
    bool prefetchable;
};

struct PciFunctionInfo {
    u16 segment;
    u8 bus;
    u8 device;
    u8 function;

    u16 vendor_id;
    u16 device_id;
    u32 class_code; // [23:16] base class, [15:8] subclass, [7:0] prog-if

    PciBar bars[kMaxBars];

    u32 requester_id; // PCI requester id (= SMMU stream id) from BDF
    u16 command;      // raw command-register snapshot (read-only)

    // Observations from the command register.  Never cleared here.
    bool bus_master;     // command bit 2
    bool memory_enabled; // command bit 1
};

// Derive the PCI requester id / SMMU stream id for a BDF.
// (bus << 8) | (device << 3) | function.
u32 requester_id(u8 bus, u8 device, u8 function);

// Enumerate every function reachable through ECAM at `ecam_base`.
// Writes at most `cap` records into `out`; returns the number written.
// `ecam_base` must be the identity-mapped ECAM window (see
// vm_space::map_bootstrap_identity); only the first 16 buses are scanned to
// stay inside that mapping.
u32 enumerate(PhysAddr ecam_base, PciFunctionInfo* out, u32 cap);

// Convenience wrapper: enumerate, then emit one Ame boot log line per function
// plus one line per non-empty BAR.  Used by the Phase A inventory.
void log_inventory(PhysAddr ecam_base);

// Hold every assignable (non-native) function non-bus-mastering with interrupts
// disabled until an assignment is fully constructed.  The native xHCI function
// is left untouched so the native USB path keeps working.  Returns the number
// of functions whose command register was changed.
u32 contain_assignable(PhysAddr ecam_base);

// Human-readable BAR type tag ("io", "mmio32", "mmio64", "none").
const char* bar_type_name(BarType type);

} // namespace pci
