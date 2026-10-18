#pragma once
#include "midix/abi/vm.h"
// anko/vm.h — Dango VM scaffolding.
//
// Dango creates capability-mediated VM objects, memory objects, stage-2
// mappings, dormant vCPUs, and paravirt channels.

#include "anko/types.h"
#include "anko/cap.h"
#include "anko/ipc.h"
#include "anko/iommu.h"
#include "anko/mm.h"
#include "anko/pci.h"
#include "midix/abi/dango_channel_guest.h"
#include "anko/sched.h"
#include "anko/syscall.h"

namespace vm {

constexpr u32 kMaxVms = 4;
constexpr u32 kMaxVcpusPerVm = 4;
constexpr u32 kMaxVmMemoryObjects = 32;
constexpr u32 kMaxVmChannels = 16;
// Phase L: attachments of an existing pinned page set (CAP_ARENA) into a VM.
constexpr u32 kMaxSharedArenas = 16;
// Stage-2 mapping table: defined below, after kDangoRamPages, because its size
// is derived from the guest RAM size and the block granularity.

// Stage-2 block mappings. A level-2 block descriptor covers 2 MiB with a single
// table entry and a single mapping record, which is how guest RAM is meant to be
// mapped; the backing range must be 2 MiB aligned guest-physical, so the caller
// uses mm::alloc_pages_contiguous().
constexpr u32 kStage2PageShift = 12;
constexpr u32 kStage2BlockShift = 21;
constexpr u64 kStage2BlockSize = 1ull << kStage2BlockShift;
constexpr u32 kStage2BlockPages = 1u << (kStage2BlockShift - kStage2PageShift);
constexpr u32 kVgicMaxIrqs = 64;
constexpr u32 kVgicBitmapWords = kVgicMaxIrqs / 32;

constexpr u64 kDefaultGuestMidr = 0x410FD034ull; // ARM Cortex-A53 MIDR.


constexpr u64 kGuestPhysMin = DANGO_RAM_BASE;
constexpr u64 kGuestPhysMax = DANGO_RAM_BASE + DANGO_RAM_SIZE;
constexpr u64 kDangoRamBase = DANGO_RAM_BASE;
constexpr u64 kDangoRamSize = DANGO_RAM_SIZE;
constexpr u64 kDangoRamPages = kDangoRamSize / PAGE_SIZE;

// Stage-2 mapping table sizing. Guest RAM and the assigned MMIO window are both
// block-mapped (kStage2BlockPages pages per record), so RAM costs
// kDangoRamPages / kStage2BlockPages records and a fully mapped BAR costs one per
// 2 MiB. The rest of the budget is the page-granular windows -- staging
// (3 MiB = 768), the shared-region window (1 MiB = 256), ECAM (256) and the
// channel window (32) -- plus headroom for splitting a block when a page inside
// it needs its own mapping (up to kStage2BlockPages - 1 records each).
constexpr u32 kMaxStage2DeviceRecords = 2048;
constexpr u32 kMaxStage2Mappings =
    static_cast<u32>(kDangoRamPages / kStage2BlockPages) + kMaxStage2DeviceRecords;
// The whole reason the block form exists: page-per-record RAM cannot fit the
// fixed table, so a regression to it must fail the build, not the boot.
static_assert(kDangoRamPages > kMaxStage2Mappings,
              "guest RAM must be block-mapped: it does not fit page-per-record");

// Stage-2 input address width: the guest runs with a 39-bit IPA.
constexpr u32 kVmIpaBits = 39;
// Lowest guest IPA a device BAR / synthetic ECAM mapping may be placed at; the
// low 256 MiB window is reserved for the appliance's own layout.
constexpr u64 kMinDeviceGuestIpa = 0x10000000ull;

// Shared staging window (SYS_VM_MEMORY_CREATE with MX_VM_MEMORY_FLAG_SHARED_STAGING).
// The staging object is host-writable and guest-mappable, but its pages are
// deliberately kept out of every DMA domain (see vm::collect_vm_ram) until a
// device assignment registers them explicitly. The window is a device-side guest
// IPA range above RAM, single-sourced from the shared Dango binding so the
// kernel, the guest DTS and the Linux driver cannot disagree.
constexpr u64 kStagingWindowBase = DANGO_GPU_STAGING_BASE;
constexpr u64 kStagingWindowSize = DANGO_GPU_STAGING_SIZE;
static_assert(kStagingWindowBase >= kDangoRamBase + kDangoRamSize,
              "staging window must sit above guest RAM");
static_assert(kStagingWindowBase % PAGE_SIZE == 0 && kStagingWindowSize % PAGE_SIZE == 0,
              "staging window must be page aligned");
static_assert(kStagingWindowSize != 0 && kStagingWindowSize <= 16ull * 1024ull * 1024ull,
              "staging window must be bounded");

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
    VCPU_BLOCKED_WFI = 6,
    VCPU_BLOCKED_OFF = 7,
    VCPU_OFF = 8,
};

enum VcpuExitReason : u32 {
    VCPU_EXIT_QUANTUM_EXPIRED = 0,
    VCPU_EXIT_WFI_BLOCKED = 1,
    VCPU_EXIT_VIRTUAL_IRQ_PENDING = 2,
    VCPU_EXIT_MMIO_BATCH_READY = 3,
    VCPU_EXIT_UNSUPPORTED_TRAP = 4,
    VCPU_EXIT_SHUTDOWN = 5,
    VCPU_EXIT_HOST_PREEMPTED = 6,
};

enum VcpuBlockReason : u32 {
    VCPU_BLOCK_NONE = 0,
    VCPU_BLOCK_GUEST_IDLE = 1,
    VCPU_BLOCK_GUEST_OFF = 2,
};

using VcpuRunBudget = MxVcpuRunBudget;
static_assert(__is_trivially_constructible(VcpuRunBudget));

using VcpuRunResult = MxVcpuRunResult;
static_assert(__is_trivially_constructible(VcpuRunResult));

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
    // Phase D device assignment (§6, §7, §8.11, §8.12).
    VM_DEBUG_VM_DEVICE_CREATED = 55,
    VM_DEBUG_VM_DEVICE_ACTIVE = 56,
    VM_DEBUG_VM_DEVICE_DESTROYED = 57,
    VM_DEBUG_VM_DEVICE_BAR_MAPPED = 58,
    VM_DEBUG_VM_DEVICE_IRQ_ROUTES = 59,
    VM_DEBUG_VM_DEVICE_IRQ_INJECTED = 60,
    VM_DEBUG_VM_DEVICE_MEDIATED_DENY = 61,
    VM_DEBUG_VM_DEVICE_BME_ENABLE = 62,
    VM_DEBUG_VM_DEVICE_QUARANTINE = 63,
    VM_DEBUG_VM_DEVICE_MAX = 63,
};

struct VmRoot {
    u32 magic;
};

// Stage-2 memory attribute class.
//
// Guest RAM uses NormalWB.  NormalNC exists for host-visible non-cacheable
// buffers the guest shares with a DMA master.  Device is the MMIO class: it
// selects Device-nGnRE, is always execute-never, and is what the GPU spec
// (§6.2) requires for aperture/register windows.  The class is orthogonal to
// the S2AP access rights carried in `rights`.
enum class Stage2MemType : u32 {
    NormalWB = 0,
    NormalNC = 1,
    Device = 2,
};

struct Stage2Mapping {
    u64 ipa;
    PhysAddr pa;
    u32 rights;
    bool valid;
    Stage2MemType mem_type;
    // log2 of the mapped unit: kStage2PageShift for a 4 KiB page,
    // kStage2BlockShift for a 2 MiB block. One block record describes
    // kStage2BlockPages pages, so a compact table can still cover a large
    // address space.
    u8 pages_log2;
    u8 pad[2];
};

struct Stage2AddressSpace {
    PhysAddr root_table_phys;
    VirtAddr root_table_va;
    PhysAddr next_level_tables[96];
    u32 next_level_table_count;
    u32 reserved0;
    u64 ipa_bits;
    u64 page_size;
    // Pages covered by the mappings above (sum of each record's unit in pages):
    // that is what "is all of this region mapped" means, and what the manager
    // checks after mapping guest RAM. Distinct from mapped_records, the
    // fixed-table budget -- one 2 MiB block record covers kStage2BlockPages.
    u64 mapped_pages;
    // Mapping records in use, bounded by kMaxStage2Mappings.
    u64 mapped_records;
    bool built;
    bool active;
    u8 pad[6];
    struct TrapRange {
        u64 base;
        u64 size;
    };
    TrapRange traps[8];
    Stage2Mapping mappings[kMaxStage2Mappings];
};

// Capacity is measured in records, never covered pages. Check before subtracting
// so a corrupt/overfull counter cannot wrap into apparent free space.
inline bool stage2_has_capacity(const Stage2AddressSpace* s2, u64 records) {
    return s2 && s2->mapped_records <= kMaxStage2Mappings &&
           records <= kMaxStage2Mappings - s2->mapped_records;
}

struct VmMemory {
    object::Record* lifetime;
    object::Record* owner_lifetime;
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
    // Set from MX_VM_MEMORY_FLAG_SHARED_STAGING. Staging pages are host-writable
    // and guest-mappable but must never become DMA-authorizable; collect_vm_ram()
    // skips any VmMemory with this set.
    bool shared_staging;
    u8 pad[5];
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
    fpsimd::State host_fpsimd;
    fpsimd::State guest_fpsimd;
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
static_assert(__builtin_offsetof(El2RunState, host_fpsimd) == 704);
static_assert(__builtin_offsetof(El2RunState, guest_fpsimd) == 1232);
struct VcpuSched {
    object::Record* context_lifetime;
    u32 sched_context_id;
    u32 runner_tid;

    u32 home_cpu;
    u32 blocked_reason;
    u64 core_mask;

    u64 budget_ns;
    u64 period_ns;
    u64 deadline_ns;
    u64 used_ns;

    u64 last_entry_ns;
    u64 last_exit_ns;

    u64 next_guest_timer_cnt;
    bool timer_armed;
    bool blocked_wfi;
    bool virtual_irq_pending;
    u8 _pad0;

    u32 wake_reason;
    u32 exit_batch_count;
};
static_assert(__is_trivially_constructible(VcpuSched));

struct Vcpu {
    object::Record* lifetime;
    object::Record* owner_lifetime;
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
    VcpuSched sched;
    VgicState vgic;
};

struct VmChannel {
    object::Record* lifetime;
    object::Record* owner_lifetime;
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
    // Appliance channel boot generation captured at create_channel(). Every
    // remote object the guest sees is scoped to this value; when the appliance
    // (re)connects it advances and every previously captured reference is
    // stale (spec §13.3). Published to the guest in the descriptor page.
    u64 boot_generation;
    PhysAddr descriptor_page;
    Vcpu* target; // owned by the same VM; accessed only under CallGuard
    u64 guest_base;
    u32 guest_irq;
    bool armed;
    bool dead;
    u8 pad[2];
    // Shared staging window recorded by stage_channel() and published in the
    // descriptor page. staging_base is the guest IPA; both are zero until the
    // channel is staged.
    PhysAddr staging_base;
    u64 staging_size;
};

// ---------------------------------------------------------------------------
// Phase L: dynamic shared arenas (spec §12.3).
//
// A native owner attaches an existing pinned page set (a kernel-backed
// CAP_ARENA) into a VM's guest address space without rebooting it. The
// attachment is a single capability-bearing object that couples the host arena
// lifetime and the guest stage-2 mapping lifetime: `arena_lifetime` retains the
// arena object so its pages cannot be freed under the guest, `owner_lifetime`
// retains the VM record so the stage-2 mapping can never outlive the VM, and
// releasing the attachment's last reference unmaps the guest range and drops
// both. There is no path that lets one lifetime outlive the other.
//
// DMA stays explicit and separate: an attached region enters stage 2 as
// iommu::VmDmaClass::None and is therefore absent from every DMA domain. It
// becomes reachable by assigned devices only when the owner explicitly
// registers it SharedGpu through shared_arena_register_dma(), the sole writer
// of `dma_class`.
// ---------------------------------------------------------------------------

// Attach flags (a5). No flag may grant DMA authority today, so `None` is the
// only accepted value: a future flag cannot silently become a DMA upgrade.
constexpr u32 kSharedArenaFlagNone = 0;

struct Vm; // defined below; SharedArena holds only a borrowed back-pointer

struct SharedArena {
    object::Record* lifetime;       // this attachment's own reference
    object::Record* arena_lifetime; // retained host arena (pinned page set)
    object::Record* owner_lifetime; // retained owner VM (guest mapping lifetime)
    Arena* arena;                   // host page set backing the guest range
    Vm* owner_vm;                   // VM whose stage 2 holds the mapping
    u64 object_tag;                 // DMA owner identity reported to the IOMMU
    u32 shared_id;
    u32 owner_vm_id;
    PhysAddr host_phys_base;
    u64 size;
    u64 guest_pa_base;
    u32 rights;                     // stage-2 access rights (READ/WRITE only)
    u32 flags;
    u32 dma_perms;                  // device perms captured at registration
    iommu::VmDmaClass dma_class;    // None until explicitly registered SharedGpu
    bool mapped_stage2;
    bool dead;
    u8 pad[1];
};

// Guest-visible geometry of an attachment (SYS_VM_SHARED_MEMORY_INSPECT).
using SharedArenaInfo = MxSharedArenaInfo;

struct Vm {
    object::Record* lifetime;
    u64 owner_identity;
    u32 vm_id;
    u32 owner_pid;
    VmState state;
    u32 pad0;
    // Phase L: one zeroed page mapped read-only across the DANGO_SHM_GUEST_*
    // window (the shared-arena descriptor). Owned by the VM, mapped on the first
    // channel map and freed with the VM. Zero until then.
    PhysAddr shm_page;
    VmMemory* memory_objects[kMaxVmMemoryObjects];
    u32 memory_count;
    Vcpu* vcpus[kMaxVcpusPerVm];
    u32 vcpu_count;
    VmChannel* channels[kMaxVmChannels];
    u32 channel_count;
    SharedArena* shared_arenas[kMaxSharedArenas];
    u32 shared_arena_count;
    Stage2AddressSpace stage2;
    u64 guest_ram_bytes;
    u64 mapped_regions;
    u64 flags;
    u64 created_ticks;
    // Current appliance channel boot generation. Advances every time the
    // appliance (re)establishes its channel; channels captured under an older
    // value are stale and rejected (see channel_generation_current).
    u64 channel_generation;
};

using VmInfo = MxVmInfo;

using VmMemoryInfo = MxVmMemoryInfo;

using VcpuInfo = MxVcpuInfo;

using VmChannelInfo = MxVmChannelInfo;

void init();
VmRoot* root_object();
u64 debug_counter(u64 id);
void note_error();
void metric_inc(VmDebugCounter id);

// Serialize VM mutation and guest entry. Host timer exits bound guest runs.
class CallGuard {
    bool active_;
    u64 irq_;
public:
    explicit CallGuard(bool active);
    ~CallGuard();
    CallGuard(const CallGuard&) = delete;
};
void destroy_owned_process(u64 identity);
void dispose_memory(void* target);
void dispose_vcpu(void* target);
void dispose_channel(void* target);
void dispose_shared_arena(void* target);

cap::Ref require_cap(Process& caller, CapHandle handle, CapType type,
                      u32 need_rights, SysError* out_error);
cap::Borrow<Vm> vm_from_cap(Process& caller, CapHandle vm_cap, u32 need_rights, SysError* out_error);
cap::Borrow<Vcpu> vcpu_from_cap(Process& caller, CapHandle vcpu_cap, u32 need_rights, SysError* out_error);
cap::Borrow<VmMemory> memory_from_cap(Process& caller, CapHandle memory_cap,
                          u32 need_rights, SysError* out_error);
cap::Borrow<VmChannel> channel_from_cap(Process& caller, CapHandle channel_cap,
                            u32 need_rights, SysError* out_error);
Vm* find_vm_by_id(u32 vm_id);
Vm* owner_for_vcpu(Vcpu* vcpu);
void refresh_vm_state(Vm* vm);
void poll_blocked_vcpu_timers();
void poll_vcpu_timer(Vcpu& vcpu);

SysError stage2_create(Stage2AddressSpace* s2);
SysError stage2_map_page(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights);
SysError stage2_map_block(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights,
                          Stage2MemType mem_type);
SysError stage2_map_page_ex(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights,
                            Stage2MemType mem_type);
SysError stage2_unmap_page(Stage2AddressSpace* s2, u64 ipa);
// Allocation-free teardown of whole mapping units. Holes are allowed; a range
// cutting a block is rejected before mutation (use unmap_page for a partial split).
SysError stage2_unmap_range(Stage2AddressSpace* s2, u64 ipa, u64 size);
SysError stage2_revoke_device_range(Stage2AddressSpace* s2, u64 ipa, u64 size);
// CallGuard required: revoke a Device leaf even after the VM has first run.
SysError stage2_revoke_device_page(Stage2AddressSpace* s2, u64 ipa);
SysError stage2_reserve_trap(Stage2AddressSpace* s2, u64 ipa, u64 size);
void stage2_release_trap(Stage2AddressSpace* s2, u64 ipa, u64 size);
SysError stage2_destroy(Stage2AddressSpace* s2);
SysError stage2_validate_no_overlap(Stage2AddressSpace* s2, u64 ipa, u64 size);
SysError stage2_activate(Stage2AddressSpace* s2);
PhysAddr stage2_lookup_page(Stage2AddressSpace* s2, u64 ipa, u32* out_rights,
                            Stage2MemType* out_mem_type = nullptr);

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

// --- Phase L: dynamic shared arenas (vm_shared.cc) -------------------------
// Core attach/teardown, free of capability and process machinery so the host
// probe can drive it directly. `arena_record` is the CAP_ARENA entry's object
// record (retained on success); `size` must equal the whole arena extent. On
// success *out names the new attachment with one creator reference.
SysError shared_arena_create(Vm* vm, Arena* arena, object::Record* arena_record, u64 guest_pa,
                             u64 size, u32 rights, u32 flags, SharedArena** out);
// Unmap the guest range and drop the VM list entry. Idempotent; safe on a
// retired VM (whose stage 2 is already gone). Does not free the record.
SysError shared_arena_teardown(SharedArena* shared);
// Transition the region to SharedGpu. This is the only writer of `dma_class`;
// it refuses unless the region is live and guest-mapped, and never widens an
// already-registered permission set.
SysError shared_arena_register_dma(SharedArena* shared, u32 perms);
// Page-owner query for the IOMMU core (iommu::PageOwnerQuery shape, ctx is a
// SharedArena*). Reports ownership only for a region that has been explicitly
// registered; an unregistered (None) region is owned by nobody.
bool shared_arena_dma_query(void* ctx, PhysAddr pa, iommu::PageOwnership* out);
// DMA class of an attachment, for diagnostics and the host probe.
iommu::VmDmaClass shared_arena_dma_class(const SharedArena& shared);

// Capability-facing operations. CallGuard is held by the dispatcher, like every
// other VM call, so these serialize with VM mutation and guest entry.
cap::Borrow<SharedArena> shared_arena_from_cap(Process& caller, CapHandle shared_cap,
                                               u32 need_rights, SysError* out_error);
CapHandle attach_shared_memory(Process& caller, CapHandle vm_cap, CapHandle arena_cap,
                               u64 guest_pa, u64 size, u32 rights, u32 flags,
                               SysError* out_error);
SysError register_shared_memory_dma(Process& caller, CapHandle shared_cap, u32 perms);
SysError inspect_shared_memory(Process& caller, CapHandle shared_cap, SharedArenaInfo* out);
// VM retire / owner death: tear down every attachment bound to `vm` before its
// stage-2 tables and RAM are freed. (Called from retire_vm once that one-line
// hook lands; the host probe exercises it directly.)
void shared_arena_release_for_vm(Vm* vm);

// Aggregate attachment counters (diagnostics).
struct SharedArenaStats {
    u64 attached;
    u64 teardowns;
    u64 dma_registered;
};
SharedArenaStats shared_arena_stats();

CapHandle create_vcpu(Process& caller, CapHandle vm_cap, u64 flags, SysError* out_error);
SysError set_vcpu_entry(Process& caller, CapHandle vcpu_cap, u64 entry_pc,
                        u64 arg0, u64 arg1);
SysError inspect_vcpu(Process& caller, CapHandle vcpu_cap, VcpuInfo* out);
SysError vcpu_bind_sched_context(Process& caller, CapHandle vcpu_cap, CapHandle sched_context_cap);
SysError vcpu_set_affinity(Process& caller, CapHandle vcpu_cap, u64 core_mask, u32 home_cpu);
SysError vcpu_kick(Process& caller, CapHandle vcpu_cap, u32 reason);
SysError vcpu_inject_irq(Process& caller, CapHandle vcpu_cap, u32 irq);
SysError vcpu_block_until_irq_or_timer(Process& caller, CapHandle vcpu_cap);
SysError run_vcpu(Process& caller, CapHandle vcpu_cap, const VcpuRunBudget& budget, VcpuRunResult* out);

CapHandle create_channel(Process& caller, CapHandle vm_cap, u32 order, u32 flags,
                         SysError* out_error);
CapHandle channel_endpoint(Process& caller, CapHandle channel_cap, u32 endpoint_kind,
                           u32 rights, SysError* out_error);
SysError inspect_channel(Process& caller, CapHandle channel_cap, VmChannelInfo* out);
SysError map_channel(Process& caller, CapHandle channel_cap, CapHandle vcpu_cap,
                     u64 ipa, u64 reserved_size, u32 guest_irq);
// Record the VM's shared staging window (guest IPA + size) in the channel
// descriptor. Validates alignment, size (against the VM's staging memory) and
// non-overlap; no-op-safe E_INVALID until map_channel() has run.
SysError stage_channel(Process& caller, CapHandle channel_cap, u64 ipa, u64 size);
SysError notify_channel(Process& caller, CapHandle channel_cap);
bool channel_hvc(Vcpu& vcpu, Vm& owner);
void channel_publish(VmChannel& channel); // internal, CallGuard held

// A channel reference is current only while it still carries the appliance's
// live boot generation. Both fields are zero for a not-yet-created channel, so
// the host self-test fixture (which builds objects by hand) stays current.
inline bool channel_generation_current(const Vm& owner, const VmChannel& channel) {
    return owner.channel_generation == channel.boot_generation;
}

// SPSC ring back-pressure: a full producer ring must never be spammed or
// silently dropped. head is producer-owned (release-published) and tail is
// consumer-owned; both are free-running counters, so occupancy is their
// difference and capacity is 2^order slots. Callers must pass a non-null ring.
inline bool channel_ring_full(const Ring& ring) {
    const auto* header = reinterpret_cast<const RingHeader*>(ring.phys_base);
    const u64 head = __atomic_load_n(&header->head.head, __ATOMIC_ACQUIRE);
    const u64 tail = __atomic_load_n(&header->tail.tail, __ATOMIC_RELAXED);
    return (head - tail) >= (1ull << ring.order);
}

// Appliance-death propagation: detach every channel bound to a dying vCPU
// before its storage is released, and clear the guest vector's vGIC pending
// bit. CallGuard held by the caller. Safe to call for a vCPU with no owner VM.
void channel_detach_vcpu(Vcpu& vcpu);


// ===========================================================================
// Phase D: VM device assignment (§6 device-assignment model, §7 interrupt
// routing, §8.11 secure attach, §8.12 DMA-safe revoke).
//
// A VmDeviceAssignment binds a set of host resources to exactly one VM and is
// the single lifetime/revocation point for them: it owns the IommuDomain the
// assigned functions DMA through, the guest MMIO (BAR) regions, and the IRQ
// routes. It retains every host object it references and tears them down
// together.
// ===========================================================================
constexpr u32 kMaxVmDevices = 8;
constexpr u32 kMaxAssignMmio = 8;

// Each assigned MMIO region can cost a few stage-2 entries beyond its own
// pages (subrange bookkeeping / alignment padding). Reserve that slack on top of
// the guest RAM so the fixed table can never be exhausted by device mappings.
constexpr u32 kMaxAssignMmioSlackPages = 4;
static_assert(kMaxAssignMmio * kMaxAssignMmioSlackPages <= kMaxStage2DeviceRecords,
              "the device budget must cover the assignment windows");

// Assignment flags (SYS_VM_ASSIGN_DEVICE a2).  BAR assignment is opt-in so the
// default path (and the assignment self-test) is unchanged on platforms where
// firmware has already programmed the BARs.
constexpr u64 kAssignFlagAssignBars = 1ull << 0;
constexpr u32 kMaxAssignIrqs = 8;
constexpr u32 kMaxAssignRam = 16;
constexpr u32 kMaxAssignRetained = 24;

enum class VmDeviceKind : u32 { Pci = 1 };

// Secure-attach state machine (§8.11) plus the revoke states (§8.12). The
// numeric order of the attach states is the exact order of the spec's steps, so
// a stuck assignment reports precisely how far it got.
enum VmDeviceState : u32 {
    VM_DEV_EMPTY = 0,
    VM_DEV_CLAIMED = 1,          // CLAIM FUNCTION / ISOLATION GROUP
    VM_DEV_CONTAINED = 2,        // CLEAR HOST BUS MASTER + MASK INTERRUPTS
    VM_DEV_QUIESCED = 3,         // RESET/QUIESCE DEVICE IF REQUIRED
    VM_DEV_DOMAIN_READY = 4,     // CREATE EMPTY IOMMU DOMAIN
    VM_DEV_RAM_MAPPED = 5,       // MAP AUTHORIZED APPLIANCE RAM
    VM_DEV_STREAMS_ATTACHED = 6, // ATTACH ALL REQUESTER/STREAM IDs
    VM_DEV_SYNCED = 7,           // IOMMU INVALIDATE + SYNC
    VM_DEV_IRQS_INSTALLED = 8,   // INSTALL HOST IRQ ROUTES
    VM_DEV_EXPOSED = 9,          // EXPOSE SYNTHETIC/FILTERED PCI FUNCTION
    VM_DEV_BME_ALLOWED = 10,     // ALLOW GUEST BUS-MASTER ENABLE
    VM_DEV_ACTIVE = 11,          // ACTIVE
    VM_DEV_FAILED = 12,          // aborted; function left non-bus-mastering
    VM_DEV_REVOKING = 13,        // revoke in progress (§8.12)
    VM_DEV_REVOKED = 14,         // torn down cleanly
    VM_DEV_QUARANTINED = 15,     // could not prove DMA stopped; pages pinned
};

// One host PCI function in the assignment's isolation group.
struct VmAssignFn {
    pci::PciFunctionInfo info; // inventory record (BDF, ids, BARs, requester id)
    bool primary;              // the function named by the caller
    u8 pad[3];
};

// An assigned BAR subrange mapped into the guest as a stage-2 Device mapping.
struct VmAssignMmio {
    PhysAddr host_base; // host physical base of the subrange
    u64 size;
    u64 guest_ipa;
    u32 rights;
    u32 fn_index;
    u32 bar_index;
    u32 bar_offset; // byte offset within the BAR
    bool mapped;
    u8 pad[3];
};

// A host IRQ route bound to a guest vGIC vector.
struct VmAssignIrq {
    u32 host_irq;
    u32 guest_irq;
    u32 trigger; // 0 = level, 1 = edge (§7.4)
    u32 fn_index;
    u32 vcpu_id;
    bool installed;
    bool suppressed; // level route: host delivery paused until guest EOI
    bool msi;        // installed via the MSI/MSI-X virtualization path
    u8 pad;
    u64 deliveries;
    Vcpu* target; // resolved appliance vCPU (owned by owner_vm)
};

// An authorised appliance-RAM range. Guest IPA == device IOVA (§8.2).
struct VmAssignRam {
    PhysAddr base;
    u64 size;
    u64 iova;
    bool mapped;
    u8 pad[7];
};

struct VmDeviceAssignment {
    object::Record* lifetime;
    Vm* owner_vm;
    u32 device_id;
    u32 state; // VmDeviceState
    VmDeviceKind kind;
    u32 flags;
    bool active;
    u8 pad0[3];
    u64 owner_identity;
    u32 owner_pid;

    u32 fn_count;
    VmAssignFn fns[pci::kMaxAssignFns];

    u32 mmio_count;
    VmAssignMmio mmio[kMaxAssignMmio];

    u32 irq_count;
    VmAssignIrq irq[kMaxAssignIrqs];

    u32 ram_count;
    VmAssignRam ram[kMaxAssignRam];

    iommu::IommuDomain* dma_domain;
    // Identity the DMA domain is authorised against; the page-owner query only
    // reports pages inside `ram` as owned by this object (§8.3, §8.6).
    u64 authorized_object_;

    // Retained references on every host object the assignment names (host
    // function objects, MMIO ranges, IRQ lines). Released exactly once at
    // destroy, so a revoked assignment cannot leave a dangling grant behind.
    u32 retained_count;
    object::Record* retained[kMaxAssignRetained];

    // Synthetic / filtered guest config view (§6.3): only assigned functions.
    pci::GuestConfig cfg;
    pci::MsiState msi[pci::kMaxAssignFns];
    u64 ecam_ipa;
    u64 ecam_size;

    u64 created_ticks;
    u64 revoke_step;
    bool body_released; // creator reference already dropped (idempotent teardown)
    u8 pad1[7];
};

// Host-side description of the isolation group to assign. Filled from the host
// function capability; every field is kernel-trusted.
struct DeviceAssignSpec {
    u32 group_id;
    u32 fn_count;
    struct Fn {
        u8 segment;
        u8 bus;
        u8 device;
        u8 function;
        u32 requester_id;
    } fns[pci::kMaxAssignFns];
};

// Human-readable state name, for logs and tests.
const char* device_state_name(u32 state);

cap::Borrow<VmDeviceAssignment> assignment_from_cap(Process& caller, CapHandle cap, u32 rights,
                                                    SysError* out_error);

// §8.11 secure attach, driven from a VM cap + host function cap. On success the
// assignment is ACTIVE and its capability is returned. On any failure the host
// function is left non-bus-mastering and the assignment is FAILED.
SysError device_secure_attach(Process& caller, CapHandle vm_cap, const pci::HostFunction& host_fn,
                              u64 flags, CapHandle* out_cap);

// Assigned BAR subrange -> guest IPA as a stage-2 Device mapping (§6.2).
SysError device_map_bar(Process& caller, CapHandle assign_cap, u32 bar_index, u32 bar_offset,
                        u64 size, u64 guest_ipa, u32 rights);

// Host IRQ cap -> vCPU / guest irq route (§7.2).
SysError device_route_irq(Process& caller, CapHandle assign_cap, CapHandle irq_cap,
                          CapHandle vcpu_cap, u32 guest_irq, u32 flags);

// DMA-safe revoke then release (§8.12).
SysError device_destroy(Process& caller, CapHandle assign_cap);
// Publish a synthetic ECAM window after BARs/routes exist; no host ECAM mapping.
SysError device_expose_ecam_internal(VmDeviceAssignment* a, u64 ipa, u64 size);
SysError device_expose_ecam(Process& caller, CapHandle assign_cap, u64 ipa, u64 size);
bool device_emulate_ecam(Vm& owner, u64 ipa, u32 size, bool write, u64* value);

// --- kernel-internal entry points (boot path + native proof) ---------------
// Allocate an assignment bound to `owner` (may be null in the native proof).
VmDeviceAssignment* device_alloc(Vm* owner, u64 flags, SysError* out_error);

// Authorise a host RAM range for DMA and map it into the domain (Iova rule of
// §8.2). The native proof registers its test pages this way.
SysError device_authorize_ram(VmDeviceAssignment* a, PhysAddr base, u64 size, u64 iova);

// Run the full §8.11 state machine against an already-claimed assignment.
SysError device_attach(VmDeviceAssignment* a, const DeviceAssignSpec& spec);

// Run the full §8.12 revoke state machine and release the assignment body.
SysError device_revoke(VmDeviceAssignment* a);

// §8.12 revoke for an unclean appliance death (vCPU destroyed / VM killed /
// owner process died). Requesters are always detached into the faulting
// (quarantine) context and the domain is left quarantined with its pages pinned
// rather than destroyed, because DMA stoppage cannot be proven on this path.
SysError device_revoke_faulting(VmDeviceAssignment* a);

// Mediate a guest config-space write through the §6.3 policy. Returns E_PERM
// when the write is rejected, E_OK otherwise.
SysError device_mediate_config_write(VmDeviceAssignment* a, u32 fn_index, u32 offset, u64 value,
                                     u32 size);
// Assigned BAR subrange -> guest IPA as a stage-2 Device mapping (internal).
SysError device_map_bar_internal(VmDeviceAssignment* a, u32 bar_index, u32 bar_offset, u64 size,
                                 u64 guest_ipa, u32 rights);
// Record (and, once past the route step, install) a host IRQ route (internal).
SysError device_add_route(VmDeviceAssignment* a, u32 fn_index, u32 host_irq, u32 guest_irq,
                          u32 trigger, Vcpu* target, bool msi);
// Map every authorised RAM range into the domain (internal step 5).
SysError device_map_authorized_ram(VmDeviceAssignment* a);

// Opt-in Phase D proof (§8.11/§8.12/§7). Dev-build only; a no-op otherwise.
void vm_device_selftest_if_testdev();

void device_dispose(void* target);
void device_release_for_vm(Vm* v);
void device_owner_died(u64 identity);

// --- interrupt virtualization (vm_device_irq.cc) ---------------------------
// Install the delivery hook once; the IRQ vector then routes assigned-device
// lines into the vGIC via device_irq_deliver().
void device_irq_init();
void device_irq_deliver(u32 host_irq);
// Re-arm level routes for a guest EOI (§7.4).
void device_irq_on_guest_eoi(Vcpu& vcpu, u32 guest_irq);
// Number of live routes (diagnostics/tests).
u64 device_irq_route_count();
bool device_irq_vector_used(const Vcpu& vcpu, u32 guest_irq);
// Register / deregister a route for host-IRQ delivery. Called by the §8.11
// "INSTALL HOST IRQ ROUTES" and §8.12 "remove interrupt routes" steps.
SysError device_irq_install(VmDeviceAssignment* a, u32 route_index);
void device_irq_uninstall(VmDeviceAssignment* a, u32 route_index);
// Inject a guest vector into a vCPU (used by route delivery and tests).
void device_irq_inject(Vcpu& vcpu, u32 guest_irq, u32 trigger);

// --- vGIC pending-bit accessors (vcpu.cc) ----------------------------------
void vgic_pending_set(Vcpu& vcpu, u32 irq);
void vgic_pending_clear(Vcpu& vcpu, u32 irq);
bool vgic_pending_test(const Vcpu& vcpu, u32 irq);
// Wake a vCPU's runner because a virtual IRQ was made pending.
void vcpu_wake_irq(Vcpu& vcpu);

#ifdef ANKO_BUILD_DEV
// Boot-time opt-in guest probe; runs the same EL2 entry and MMIO handler as run_vcpu.
bool vcpu_ecam_probe(Vcpu& vcpu, Vm& owner);
#endif

} // namespace vm
