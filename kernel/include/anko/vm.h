#pragma once
// anko/vm.h — Dango VM scaffolding.
//
// Dango creates capability-mediated VM objects, memory objects, stage-2
// mappings, dormant vCPUs, and paravirt channels.

#include "anko/types.h"
#include "anko/cap.h"
#include "anko/ipc.h"
#include "anko/mm.h"
#include "anko/sched.h"
#include "anko/syscall.h"

namespace vm {

constexpr u32 kMaxVms = 4;
constexpr u32 kMaxVcpusPerVm = 4;
constexpr u32 kMaxVmMemoryObjects = 32;
constexpr u32 kMaxVmChannels = 16;
constexpr u32 kMaxStage2Mappings = 32768;
constexpr u32 kVgicMaxIrqs = 64;
constexpr u32 kVgicBitmapWords = kVgicMaxIrqs / 32;

constexpr u64 kDefaultGuestMidr = 0x410FD034ull; // ARM Cortex-A53 MIDR.


constexpr u64 kGuestPhysMin = 0x4000'0000ull;
constexpr u64 kGuestPhysMax = 0x4800'0000ull;
constexpr u64 kDangoRamBase = 0x4000'0000ull;
constexpr u64 kDangoRamSize = 128ull * 1024ull * 1024ull;

enum VmState : u32 {
    VM_EMPTY = 0,
    VM_CREATED = 1,
    VM_MEMORY_READY = 2,
    VM_VCPU_READY = 3,
    VM_READY = 4,
    VM_RUNNING = 5,
    VM_PAUSED = 6,
    VM_DEAD = 7,
};

enum VcpuState : u32 {
    VCPU_CREATED = 0,
    VCPU_CONFIGURED = 1,
    VCPU_READY = 2,
    VCPU_RUNNING = 3,
    VCPU_BLOCKED = 4,
    VCPU_DEAD = 5,
};

enum VmChannelEndpoint : u32 {
    VM_CHANNEL_HOST_TO_GUEST_RING = 0,
    VM_CHANNEL_GUEST_TO_HOST_RING = 1,
    VM_CHANNEL_HOST_TO_GUEST_DOORBELL = 2,
    VM_CHANNEL_GUEST_TO_HOST_DOORBELL = 3,
};

enum VmDebugCounter : u64 {
    VM_DEBUG_CREATE_CALLS = 32,
    VM_DEBUG_CREATED = 33,
    VM_DEBUG_DESTROYED = 34,
    VM_DEBUG_MEMORY_CREATED = 35,
    VM_DEBUG_MEMORY_MAPPED = 36,
    VM_DEBUG_MEMORY_UNMAPPED = 37,
    VM_DEBUG_STAGE2_TABLES_CREATED = 38,
    VM_DEBUG_STAGE2_PAGES_MAPPED = 39,
    VM_DEBUG_VCPU_CREATED = 40,
    VM_DEBUG_VCPU_CONFIGURED = 41,
    VM_DEBUG_VCPU_RUN_ATTEMPTS = 42,
    VM_DEBUG_VCPU_RUN_NOTIMPL = 43,
    VM_DEBUG_CHANNELS_CREATED = 44,
    VM_DEBUG_ERRORS = 45,
    VM_DEBUG_EL_CURRENT = 46,
    VM_DEBUG_EL2_AVAILABLE = 47,
    VM_DEBUG_VHE_AVAILABLE = 48,
    VM_DEBUG_GUEST_BOOT_STUB = 49,
    VM_DEBUG_GUEST_AGENT_START = 50,
    VM_DEBUG_GUEST_LINUX_HELLO = 51,
    VM_DEBUG_VCPU_WFX_EXITS = 52,
    VM_DEBUG_VCPU_IRQ_EXITS = 53,
    VM_DEBUG_VCPU_UNSUPPORTED_EXITS = 54,
};

struct VmRoot {
    u32 magic;
};

struct Stage2Mapping {
    u64 ipa;
    PhysAddr pa;
    u32 rights;
    bool valid;
    u8 pad[3];
};

struct Stage2AddressSpace {
    PhysAddr root_table_phys;
    VirtAddr root_table_va;
    PhysAddr next_level_tables[96];
    u32 next_level_table_count;
    u32 reserved0;
    u64 ipa_bits;
    u64 page_size;
    u64 mapped_pages;
    bool built;
    bool active;
    u8 pad[6];
    Stage2Mapping mappings[kMaxStage2Mappings];
};

struct VmMemory {
    u32 mem_id;
    u32 owner_vm_id;
    PhysAddr host_phys_base;
    VirtAddr host_va;
    PhysAddr* pages;
    u64 page_count;
    u64 size;
    u64 guest_pa_base;
    u64 mapped_size;
    u32 rights;
    u32 flags;
    bool mapped_stage2;
    bool dead;
    u8 pad[6];
};

struct El2RunState {
    u64 regs[31];
    u64 host_sp_el1;
    u64 pc;
    u64 pstate;
    u64 vttbr;
    u64 vtcr;
    u64 exit_esr;
    u64 exit_far;
    u64 exit_hpfar;
    u64 host_elr;
    u64 host_spsr;
    u64 host_sctlr;
    u64 host_ttbr0;
    u64 host_tcr;
    u64 host_mair;
    u64 host_vbar;
    u64 guest_sctlr;
    u64 guest_ttbr0;
    u64 guest_ttbr1;
    u64 guest_tcr;
    u64 guest_mair;
    u64 guest_vbar;
    u64 guest_sp_el1;
    u64 initialized;
    u64 host_cntp_ctl;
    u64 host_cntp_cval;
    u64 guest_cntp_ctl;
    u64 guest_cntp_cval;
    u64 guest_cntv_ctl;
    u64 guest_cntv_cval;

    /*
     * Set by the host before guest entry.
     *
     * guest_irq_pending != 0 causes el2.S to set HCR_EL2.VI, which is how the
     * software VGIC injects an IRQ into the EL1 guest while HCR_EL2.IMO routes
     * physical IRQs to EL2.
     */
    u64 guest_irq_pending;

    /*
     * Bit 0: mask the backing physical timer while the guest physical timer IRQ
     *        is pending/active in the software VGIC.
     * Bit 1: mask the backing virtual timer while the guest virtual timer IRQ
     *        is pending/active in the software VGIC.
     */
    u64 timer_hw_mask;

    /*
     * Written by el2.S on every guest exit.
     *
     * 0 = synchronous trap
     * 1 = physical IRQ routed to EL2
     * 2 = physical FIQ routed to EL2
     * 3 = SError routed to EL2
     *
     * ESR_EL2 is only valid for synchronous traps. For IRQ/FIQ/SError exits,
     * el2.S zeros exit_esr/exit_far/exit_hpfar and records the kind here.
     */
    u64 exit_kind;

    /*
     * Stable architectural CPU identity exposed to the guest through
     * VMPIDR_EL2/VPIDR_EL2.  Do not let the guest observe the host CPU it
     * happens to be scheduled on.
     */
    u64 guest_vmpidr;
    u64 guest_vpidr;

    /*
     * EL1 is shared hardware state between the host kernel and the guest when
     * Anko runs non-VHE.  Linux relies on more than the translation registers:
     * SP_EL0 holds current task state, TPIDR_EL1 holds the percpu offset,
     * CPACR/CNTKCTL are configured during bring-up, and exception registers can
     * be live across an MMIO/HVC exit.  Keep host and guest copies explicit so a
     * VM exit cannot silently replace guest EL1 state with host EL1 state, or
     * vice versa.
     */
    u64 host_ttbr1;
    u64 host_sp_el0;
    u64 host_tpidr_el1;
    u64 host_tpidr_el0;
    u64 host_tpidrro_el0;
    u64 host_contextidr_el1;
    u64 host_cpacr_el1;
    u64 host_cntkctl_el1;
    u64 host_amair_el1;

    u64 guest_sp_el0;
    u64 guest_tpidr_el1;
    u64 guest_tpidr_el0;
    u64 guest_tpidrro_el0;
    u64 guest_contextidr_el1;
    u64 guest_cpacr_el1;
    u64 guest_cntkctl_el1;
    u64 guest_amair_el1;
    u64 guest_elr_el1;
    u64 guest_spsr_el1;
    u64 guest_far_el1;
    u64 guest_esr_el1;
    u64 guest_afsr0_el1;
    u64 guest_afsr1_el1;
};

struct VgicState {
    u32 dist_ctlr;
    u32 cpu_ctlr;
    u32 cpu_pmr;
    u32 cpu_bpr;
    u32 enabled[kVgicBitmapWords];
    u32 pending[kVgicBitmapWords];
    u32 active[kVgicBitmapWords];
    u32 level[kVgicBitmapWords];
    u32 group[kVgicBitmapWords];
    u32 config[kVgicMaxIrqs / 16];
    u8 priority[kVgicMaxIrqs];
    u8 targets[kVgicMaxIrqs];
    u32 last_iar;
    u32 reserved;
};
static_assert(__builtin_offsetof(El2RunState, guest_irq_pending) == 480);
static_assert(__builtin_offsetof(El2RunState, timer_hw_mask) == 488);
static_assert(__builtin_offsetof(El2RunState, exit_kind) == 496);
static_assert(__builtin_offsetof(El2RunState, guest_vmpidr) == 504);
static_assert(__builtin_offsetof(El2RunState, guest_vpidr) == 512);
static_assert(__builtin_offsetof(El2RunState, host_ttbr1) == 520);
static_assert(__builtin_offsetof(El2RunState, guest_sp_el0) == 592);
static_assert(__builtin_offsetof(El2RunState, guest_afsr1_el1) == 696);
struct Vcpu {
    u32 vcpu_id;
    u32 owner_vm_id;
    VcpuState state;
    u32 pad0;
    u64 entry_pc;
    u64 arg0;
    u64 arg1;
    u64 regs[31];
    u64 sp;
    u64 pc;
    u64 pstate;
    El2RunState el2;
    u64 flags;
    u64 run_count;
    u64 last_exit_reason;
    u64 virtual_mpidr;
    u64 virtual_midr;
    VgicState vgic;
};

struct VmChannel {
    u32 channel_id;
    u32 owner_vm_id;
    Ring* host_to_guest;
    Ring* guest_to_host;
    Doorbell* host_doorbell;
    Doorbell* guest_doorbell;
    u32 order;
    u32 flags;
    u64 messages_host_to_guest;
    u64 messages_guest_to_host;
    bool dead;
    u8 pad[7];
};

struct Vm {
    u32 vm_id;
    u32 owner_pid;
    VmState state;
    u32 pad0;
    VmMemory* memory_objects[kMaxVmMemoryObjects];
    u32 memory_count;
    Vcpu* vcpus[kMaxVcpusPerVm];
    u32 vcpu_count;
    VmChannel* channels[kMaxVmChannels];
    u32 channel_count;
    Stage2AddressSpace stage2;
    u64 guest_ram_bytes;
    u64 mapped_regions;
    u64 flags;
    u64 created_ticks;
};

struct VmInfo {
    u32 vm_id;
    u32 owner_pid;
    u32 state;
    u32 memory_count;
    u32 vcpu_count;
    u32 channel_count;
    u32 reserved;
    u64 guest_ram_bytes;
    u64 mapped_regions;
    u64 flags;
    u64 stage2_mapped_pages;
    u64 stage2_built;
    u64 stage2_active;
};

struct VmMemoryInfo {
    u32 mem_id;
    u32 owner_vm_id;
    u32 rights;
    u32 flags;
    PhysAddr host_phys_base;
    VirtAddr host_va;
    u64 size;
    u64 guest_pa_base;
    u64 mapped_size;
    u64 mapped_stage2;
};

struct VcpuInfo {
    u32 vcpu_id;
    u32 owner_vm_id;
    u32 state;
    u32 reserved;
    u64 entry_pc;
    u64 arg0;
    u64 arg1;
    u64 run_count;
    u64 last_exit_reason;
};

struct VmChannelInfo {
    u32 channel_id;
    u32 owner_vm_id;
    u32 order;
    u32 flags;
    u64 messages_host_to_guest;
    u64 messages_guest_to_host;
    u64 endpoint_present_mask;
};

void init();
VmRoot* root_object();
u64 debug_counter(u64 id);
void note_error();
void metric_inc(VmDebugCounter id);

CapEntry* require_cap(Process& caller, CapHandle handle, CapType type,
                      u32 need_rights, SysError* out_error);
Vm* vm_from_cap(Process& caller, CapHandle vm_cap, u32 need_rights, SysError* out_error);
Vcpu* vcpu_from_cap(Process& caller, CapHandle vcpu_cap, u32 need_rights, SysError* out_error);
VmMemory* memory_from_cap(Process& caller, CapHandle memory_cap,
                          u32 need_rights, SysError* out_error);
VmChannel* channel_from_cap(Process& caller, CapHandle channel_cap,
                            u32 need_rights, SysError* out_error);
Vm* find_vm_by_id(u32 vm_id);
void refresh_vm_state(Vm* vm);

SysError stage2_create(Stage2AddressSpace* s2);
SysError stage2_map_page(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights);
SysError stage2_unmap_page(Stage2AddressSpace* s2, u64 ipa);
SysError stage2_destroy(Stage2AddressSpace* s2);
SysError stage2_validate_no_overlap(Stage2AddressSpace* s2, u64 ipa, u64 size);
SysError stage2_activate(Stage2AddressSpace* s2);
PhysAddr stage2_lookup_page(Stage2AddressSpace* s2, u64 ipa, u32* out_rights);

CapHandle create_vm(Process& caller, CapHandle root_cap, u64 flags, SysError* out_error);
SysError destroy_vm(Process& caller, CapHandle vm_cap);
SysError inspect_vm(Process& caller, CapHandle vm_cap, VmInfo* out);

CapHandle create_memory(Process& caller, CapHandle vm_cap, u64 size, u64 flags,
                        SysError* out_error);
SysError map_memory(Process& caller, CapHandle vm_cap, CapHandle memory_cap,
                    u64 guest_pa, u64 size, u32 rights);
SysError unmap_memory(Process& caller, CapHandle vm_cap, u64 guest_pa, u64 size);
SysError inspect_memory(Process& caller, CapHandle memory_cap, VmMemoryInfo* out);
SysError write_memory(Process& caller, CapHandle memory_cap, u64 guest_offset,
                      const void* src, u64 len);
void destroy_memory_object(VmMemory* mem);

CapHandle create_vcpu(Process& caller, CapHandle vm_cap, u64 flags, SysError* out_error);
SysError set_vcpu_entry(Process& caller, CapHandle vcpu_cap, u64 entry_pc,
                        u64 arg0, u64 arg1);
SysError inspect_vcpu(Process& caller, CapHandle vcpu_cap, VcpuInfo* out);
SysError run_vcpu(Process& caller, CapHandle vcpu_cap, u64 budget_ticks);

CapHandle create_channel(Process& caller, CapHandle vm_cap, u32 order, u32 flags,
                         SysError* out_error);
CapHandle channel_endpoint(Process& caller, CapHandle channel_cap, u32 endpoint_kind,
                           u32 rights, SysError* out_error);
SysError inspect_channel(Process& caller, CapHandle channel_cap, VmChannelInfo* out);

} // namespace vm
