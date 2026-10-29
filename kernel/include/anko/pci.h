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

// ---------------------------------------------------------------------------
// Phase D: host function authority and isolation groups (§6.1, §6.3, §8.13)
// ---------------------------------------------------------------------------

// Upper bound on the functions in one isolation group / one guest config view.
constexpr u32 kMaxGroupFunctions = 8;
constexpr u32 kMaxAssignFns = 8;

// A capability-backed description of one assignable host PCI function.
// Produced by the inventory; it is the authority a VM needs before it can create
// an assignment, so the VM never names raw physical addresses itself (§6.1).
struct HostFunction {
    PciFunctionInfo info;
    u32 group_id; // isolation group this function belongs to
    u32 flags;
};

// PCI command-register bits the assignment layer mediates (PCI 3.0 §6.2.2).
constexpr u16 kPciCmdIoEnable = 1u << 0;
constexpr u16 kPciCmdMemoryEnable = 1u << 1;
constexpr u16 kPciCmdBusMaster = 1u << 2;
constexpr u16 kPciCmdWritableMask = kPciCmdIoEnable | kPciCmdMemoryEnable | kPciCmdBusMaster;

// Status-register bits (PCI 3.0 §6.2.3): capabilities list present.
constexpr u16 kStatusCapabilitiesList = 1u << 4;

// Isolation-group id: functions of the same PCIe device (segment/bus/device)
// share a device reset and are commonly behind one root port, so they cannot be
// independently reset or contained. They form one group that Dango must assign
// together or reject (§6.3, §8.13). Derived purely from the BDF, so it is
// stable across enumeration passes.
constexpr u32 isolation_group_id(u8 segment, u8 bus, u8 device) {
    return (static_cast<u32>(segment) << 16) | (static_cast<u32>(bus) << 8) |
           static_cast<u32>(device & 0x1Fu);
}

struct PciIsolationGroup {
    u32 group_id;
    u16 segment;
    u8 bus;
    u8 device;
    u32 function_mask; // one bit per function present in the group
    u32 function_count;
    u16 vendor_id; // of function 0 (informational)
    u16 device_id;
    u32 class_code;
    u32 requester_ids[kMaxGroupFunctions];
};
static_assert(sizeof(PciIsolationGroup) <= 128, "PciIsolationGroup must stay small");

// Enumerate isolation groups from the ECAM inventory. Returns the number of
// groups written (<= cap). Functions collapse into their isolation group and the
// group's `function_mask` carries which functions are present.
u32 enumerate_groups(PhysAddr ecam_base, PciIsolationGroup* out, u32 cap);

// Find the inventory record for a BDF. Returns false when absent.
bool find_function(PhysAddr ecam_base, u8 bus, u8 device, u8 function, PciFunctionInfo* out);

// Find the first function with the given vendor/device id. Used by boot to
// publish the appliance's assignable function as capability authority (§6.1).
bool find_by_id(PhysAddr ecam_base, u16 vendor_id, u16 device_id, PciFunctionInfo* out);

// Program the function's implemented MMIO BARs from the host PCIe MMIO window.
// Direct -kernel boot has no firmware, so BARs read as 0; each BAR's address
// register is written while preserving its read-only low type bits, and the
// cached `info` bases are updated.  `cursor_inout` is the next free window
// offset, shared across the functions of one assignment.  Returns false when the
// window cannot satisfy a BAR.
bool assign_bars(PciFunctionInfo* info, PhysAddr window_base, u64 window_size,
                 u64* cursor_inout);

// Read the host PCI command register for a BDF.
u16 read_command(u8 bus, u8 device, u8 function);
// Read a single config byte / the 16-bit status+command word for a BDF.
u8 cfg_read_byte(u8 bus, u8 device, u8 function, u32 off);
u16 cfg_status_word(u8 bus, u8 device, u8 function);
// Find the isolation group containing a function. Returns false when the
// function is absent from the inventory.
bool isolation_group_of(PhysAddr ecam_base, u8 bus, u8 device, PciIsolationGroup* out);
// Clear or set the command register's bus-master bit, preserving every other
// bit. These are the only host config writes the assignment layer performs, and
// only during the §8.11 / §8.12 state machines.
void set_bus_master(u8 bus, u8 device, u8 function, bool enable);
// Enable/disable memory decoding for a BDF (required before a BAR is reachable).
void set_memory_enable(u8 bus, u8 device, u8 function, bool enable);

// ---------------------------------------------------------------------------
// Phase D: synthetic / filtered guest PCI configuration view (§6.3, §8.11)
// ---------------------------------------------------------------------------
constexpr u32 kCfgSpaceBytes = 256;

// Config-space register offsets the mediator and the ECAM walk reason about
// (PCI Local Bus Spec 3.0 §6.1).
constexpr u32 kRegVendorId = 0x00;
constexpr u32 kRegCommand = 0x04;
constexpr u32 kRegRevisionId = 0x08;
constexpr u32 kRegClassCode = 0x09; // [0x0B:0x09] base class / subclass / prog-if
constexpr u32 kRegHeaderType = 0x0E;
constexpr u32 kRegBar0 = 0x10;
constexpr u32 kRegBarEnd = 0x28;
constexpr u32 kRegSubsystemIds = 0x2C; // [0x2F:0x2C] subsystem vendor / device id
constexpr u32 kRegRomBar = 0x30;
constexpr u32 kRegCapPtr = 0x34;
constexpr u32 kRegIntLine = 0x3C;
constexpr u32 kRegIntPin = 0x3D;

// The six BARs and the expansion ROM BAR must lie wholly inside the (type-0)
// 256-byte configuration space every consumer models.
static_assert((kRegBar0 - kRegVendorId) / 4u == 4u, "BAR0 must start four dwords after the vendor id");
static_assert(kRegBar0 + kMaxBars * 4u == kRegBarEnd, "BAR window must end at kRegBarEnd");
static_assert(kRegBarEnd <= kCfgSpaceBytes, "BAR window must fit in config space");
static_assert(kRegRomBar + 4u <= kCfgSpaceBytes, "expansion ROM BAR must fit in config space");

// BAR register decoding (PCI 3.0 §6.2.5.1).
constexpr u32 kBarTypeMask = 0x0Fu;          // low bits: address kind / flags
constexpr u32 kBarAddrMask = ~0x0Fu;         // high bits: writable base address
constexpr u64 kBarMinSize = 16;              // 16 bytes: the smallest BAR aperture
constexpr u64 kMmio32BarLimit = 1ull << 32;  // 32-bit MMIO BARs live below 4 GiB
constexpr u32 kBarProbeValue = 0xFFFF'FFFFu; // write to size a BAR, then restore
constexpr u64 kBarUpperAbsent = 0xFFFF'FFFF'0000'0000ull; // 64-bit BAR, no upper half

// Header-type byte (PCI 3.0 §6.1.2).
constexpr u8 kHeaderTypeMultiFunction = 0x80;
constexpr u8 kHeaderTypeMask = 0x7F;

// Capability-list walk bounds and capability ids (PCI 3.0 §6.7).
constexpr u32 kCfgCapMin = 0x40;
constexpr u32 kCfgCapMax = 0xFC;
constexpr u32 kMaxCapabilityWalk = 48; // cycle/overrun guard for a hostile list
constexpr u32 kCfgCapPtrMask = 0xFC;
constexpr u8 kCapIdMsi = 0x05;
constexpr u8 kCapIdVendorSpecific = 0x09;
constexpr u8 kCapIdMsix = 0x11;
static_assert(kCfgCapMin % 4u == 0 && kCfgCapMax % 4u == 0 && kCfgCapMax + 4u <= kCfgSpaceBytes,
              "capability window must stay inside config space");

// MSI / MSI-X capability geometry (PCI 3.0 §6.8).
constexpr u32 kMsiCtrlOffset = 2;      // control word, relative to the cap header
constexpr u32 kMsiCapBytes = 24;       // header + control + address + data
constexpr u32 kMsixVectorCountMask = 0x7FF;
constexpr u32 kMsixBirMask = 0x7;
constexpr u32 kMsixTableEntryBytes = 16;
constexpr u32 kMsixPbaEntryBytes = 8;
constexpr u32 kMsixPbaBitsPerEntry = 64;

// Well-known virtio PCI ids (the appliance's assignable function).
constexpr u16 kVirtioVendorId = 0x1AF4;
constexpr u16 kVirtioGpuDeviceId = 0x1050;

// QEMU virt swizzles PCI INTx onto GIC SPIs 3..6, one per slot (pin A -> SPI 3).
constexpr u32 kQemuVirtIntxPinA = 3;
constexpr u32 kPciIntxPinCount = 4;

// ECAM address for one config-space dword. Every index is masked so a malformed
// record or a hostile BDF can never address outside the function's 4 KiB window.
constexpr u64 ecam_offset(u64 base, u32 bus, u32 dev, u32 fn, u32 off) {
    return base | (static_cast<u64>(bus & 0xFFu) << 20) |
           (static_cast<u64>(dev & 0x1Fu) << 15) | (static_cast<u64>(fn & 0x07u) << 12) |
           (off & 0xFFFu);
}

// Little-endian 16-bit access through an identity-mapped ECAM window. Uses byte
// accesses so the adjacent status register's W1C bits are never clobbered.
inline u16 cfg_read_word(const volatile u8* ecam, u32 off) {
    const u16 lo = static_cast<u16>(ecam[off]);
    const u16 hi = static_cast<u16>(ecam[off + 1]);
    return static_cast<u16>(lo | static_cast<u16>(hi << 8));
}
inline void cfg_write_word(volatile u8* ecam, u32 off, u16 v) {
    ecam[off] = static_cast<u8>(v & 0xFFu);
    ecam[off + 1] = static_cast<u8>((v >> 8) & 0xFFu);
}

// 32-bit config-space access through an identity-mapped ECAM window. One home
// for every consumer (walker, assignment, SMMU containment) so the barrier,
// masking and the absent-bus value can never drift between copies. A zero base
// reads as all-ones, the "no device" value used by the assignment path.
inline u32 cfg_read(u64 base, u32 bus, u32 dev, u32 fn, u32 off) {
    if (base == 0) {
        return 0xFFFF'FFFFu;
    }
    asm volatile("dsb sy" ::: "memory");
    return *reinterpret_cast<volatile u32*>(ecam_offset(base, bus, dev, fn, off));
}
inline void cfg_write(u64 base, u32 bus, u32 dev, u32 fn, u32 off, u32 val) {
    if (base == 0) {
        return;
    }
    *reinterpret_cast<volatile u32*>(ecam_offset(base, bus, dev, fn, off)) = val;
    asm volatile("dsb sy" ::: "memory");
}

// Split a PCI requester id / SMMU stream id back into its BDF parts.
inline void bdf_from_requester_id(u32 sid, u8& bus, u8& dev, u8& fn) {
    bus = static_cast<u8>((sid >> 8) & 0xFFu);
    dev = static_cast<u8>((sid >> 3) & 0x1Fu);
    fn = static_cast<u8>(sid & 0x07u);
}

enum class CfgWriteAction : u8 {
    Accept,  // benign: store into the synthetic image
    Mediate, // routed to a subsystem (bus-master, MSI/MSI-X); never raw-stored
    Reject,  // hostile: dropped, and the mediator counter advances
};

// The filtered view a guest sees: only assigned functions, never the outer
// ECAM or neighbouring functions on the same bus.
struct GuestConfig {
    u32 fn_count;
    u32 pad0;
    u8 space[kMaxAssignFns][kCfgSpaceBytes];
    bool present[kMaxAssignFns];
    bool bme_allowed[kMaxAssignFns]; // guest bus-master has been authorised
    u16 msi_cap_off[kMaxAssignFns];  // config offset of each MSI/MSI-X capability (0 = none)
    u32 bar_mask[kMaxAssignFns][kMaxBars];
    u8 bar_probe[kMaxAssignFns];
    u8 bar_upper[kMaxAssignFns];
};

// One synthetic bus. Assigned functions occupy device 1, functions 0..7.
constexpr u64 kGuestEcamSize = 1ull << 20;
constexpr u32 kGuestDevice = 1;
struct EcamAddress {
    u32 function;
    u32 offset;
    bool assigned_device;
};
bool decode_ecam(u64 offset, u32 size, EcamAddress* out);
u32 config_read(const GuestConfig& cfg, u32 function, u32 offset, u32 size);
// BAR sizing/restoration affects only the synthetic image, never host BARs.
bool config_bar_write(GuestConfig& cfg, u32 function, u32 offset, u32 value, u32 size);
bool config_set_bar(GuestConfig& cfg, u32 function, u32 bar, u64 base, const PciBar& info);
// Reject raw mappings of MSI-X table/PBA bytes (including their enclosing pages).
bool bar_range_safe(const u8* host, const PciBar* bars, u32 bar, u64 offset, u64 size);
// Rebuild a bounded capability list containing only mapped virtio MMIO regions.
// MSI/MSI-X and indirect PCI_CFG capabilities are not exposed by this INTx profile.
bool config_filter_virtio_caps(GuestConfig& cfg, u32 function, const u8* host);

// Classify a guest config write. `offset` is within the function's 256 bytes.
// BARs, the expansion ROM, the capability pointer and read-only fields are
// rejected; the command register and MSI/MSI-X windows are mediated; benign
// fields are accepted into the synthetic image.
CfgWriteAction classify_config_write(const GuestConfig& cfg, u32 fn_index, u32 offset, u32 size);

// ---------------------------------------------------------------------------
// Phase D: MSI/MSI-X virtualization (§7.3)
// ---------------------------------------------------------------------------
// A guest MSI/MSI-X programming request, decoded from mediated config writes.
// It never becomes a raw host MSI target: the assignment layer turns it into an
// Anko IRQ route bound to the assigned function and appliance vCPU.
struct MsiMessage {
    u64 address;
    u32 data;
    u32 vector; // guest vector = data & 0xFF
    bool enabled;
};

// MSI capability geometry for the synthetic view of one function.
struct MsiState {
    bool present;
    bool msix;      // true: MSI-X table; false: MSI capability
    bool enabled;
    u32 cap_offset; // config-space offset of the MSI/MSI-X capability
    u32 table_bar;  // MSI-X: BAR holding the vector table
    u32 table_off;  // MSI-X: offset of the vector table in that BAR
    u32 nvectors;
    MsiMessage msg;
};

// Decode an MSI/MSI-X control/message write into `state`. Returns false when the
// write does not touch the function's MSI capability.
bool msi_decode_write(MsiState& state, u32 offset, u32 value, u32 size);

// The host IRQ line Anko installs for an assigned function's MSI/MSI-X path.
// QEMU's virtio-gpu-pci uses INTx for its config-time line; the MSI path is
// synthesized. Anko binds the assignment's route to `host_irq`.
u32 msi_host_irq(const MsiState& state, u32 fn_index, u32 fallback);

} // namespace pci
