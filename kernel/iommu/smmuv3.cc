// kernel/iommu/smmuv3.cc — ARM SMMUv3 backend (GPU appliance Phase C).
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.7, §8.8, §8.12, §8.14.
//
// This file owns the SMMUv3 hardware: discovery from the outer hardware
// description, register programming, the linear stream table, the command and
// event queues, stage-2 context programming, and the generic core's backend
// hooks. It is deliberately small and fixed-capacity: every queue and table is
// a static, cache-aligned array in .bss, so there is no hot-path allocation
// and the physical addresses are simply the array addresses (the kernel is
// identity mapped).
//
// Translation model
// -----------------
// Each Ame IOMMU domain is a set of stage-2 page tables built by the generic
// core (kernel/iommu/core.cc). A requester's STE points at its domain's stage-2
// root and carries a domain-scoped VMID, so invalidations can be scoped to one
// domain. The IOVA the device presents is the stage-2 input address, exactly
// the "guest physical address == IOVA" model of §8.2. There is no stage-1
// table, no identity map, and no bypass STE: an invalid, unbound or detached
// stream faults.

#include "anko/smmuv3.h"

#include "anko/hw_info.h"
#include "anko/log.h"
#include "anko/mm.h"
#include "anko/pci.h"
#include "libk/string.h"

#include "smmuv3_internal.h"

namespace iommu {

namespace {

using namespace iommu::smmuv3;

// ---------------------------------------------------------------------------
// Geometry.  Everything is compile-time fixed.
// ---------------------------------------------------------------------------
constexpr u32 kCmdqLog2 = 8;   // 256 commands * 16 B = 4 KiB
constexpr u32 kEventqLog2 = 8; // 256 events * 32 B = 8 KiB
constexpr u32 kStreamLog2 = 8; // 256 STEs * 64 B = 16 KiB, 16 KiB aligned

constexpr u32 kCmdqEntries = 1u << kCmdqLog2;
constexpr u32 kEventqEntries = 1u << kEventqLog2;
constexpr u32 kStreamEntries = 1u << kStreamLog2;

// IOVA geometry: the core builds a 3-level, 4 KiB-granule, 39-bit table, so
// the stage-2 input size is 39 bits and the walk starts at level 1.
constexpr u32 kT0Sz = 64u - kIommuIovaBits; // 25
constexpr u16 kDenyVmid = 0;                // real domains use index 1..7

constexpr u32 kSyncSpins = 1'000'000;
constexpr u32 kMaxDrainPerCall = kEventqEntries;

// ---------------------------------------------------------------------------
// Fixed storage (.bss).  Physical address == kernel virtual address here, and
// the kernel maps RAM (and kernel .bss) identity, so the array addresses are
// valid addresses for the SMMU to fetch from.
// ---------------------------------------------------------------------------
alignas(1u << (kStreamLog2 + 6)) u8 g_stream_table[kStreamEntries * kSteBytes];
alignas(kCmdqEntries* kCmdBytes) Cmd g_cmdq[kCmdqEntries];
alignas(kEventqEntries* kEvtBytes) Evt g_evq[kEventqEntries];
alignas(PAGE_SIZE) u8 g_deny_table[PAGE_SIZE]; // empty level-1 table: all misses

enum class State : u8 { Uninitialised, Ready, Failed };

State g_state = State::Uninitialised;
PhysAddr g_base = 0;
u32 g_sid_limit = 0;
u32 g_cmdq_prod = 0; // monotonic; register value is the low log2size+1 bits
u32 g_evq_cons = 0;
u64 g_events_drained = 0;
kernel::SpinLock g_lock;

// ---------------------------------------------------------------------------
// MMIO
// ---------------------------------------------------------------------------
volatile u32* mmio(u32 off) {
    return reinterpret_cast<volatile u32*>(g_base + off);
}

u32 readl(u32 off) {
    const u32 value = *mmio(off);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

void writel(u32 off, u32 value) {
    asm volatile("dsb sy" ::: "memory");
    *mmio(off) = value;
    asm volatile("dsb sy" ::: "memory");
}

// Clean a physical range to the point of coherency so descriptors, STEs and
// queue records written by the CPU are visible to the IOMMU.  On a coherent
// platform this is redundant but harmless; §8.8 requires the operation to
// exist so a non-coherent design can hook the same path.
void clean_to_poc(PhysAddr pa, u64 len) {
    constexpr u64 kLine = 64;
    const PhysAddr start = pa & ~(kLine - 1);
    for (PhysAddr p = start; p < pa + len; p += kLine) {
        asm volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------
SysError cq_submit_locked(const Cmd& cmd) {
    Cmd& slot = g_cmdq[q_index(g_cmdq_prod, kCmdqLog2)];
    slot.w[0] = cmd.w[0];
    slot.w[1] = cmd.w[1];
    slot.w[2] = cmd.w[2];
    slot.w[3] = cmd.w[3];
    clean_to_poc(reinterpret_cast<PhysAddr>(&slot), kCmdBytes);
    ++g_cmdq_prod;
    writel(kRegCmdqProd, q_wrap_index(g_cmdq_prod, kCmdqLog2));
    return E_OK;
}

// Submit a CMD_SYNC and wait for the consumer to catch up.  The architecture
// guarantees that when SYNC completes, every earlier command has completed.
SysError cq_sync_locked() {
    cq_submit_locked(make_cmd_sync());
    const u32 want = q_wrap_index(g_cmdq_prod, kCmdqLog2);
    const u32 wrap_mask = (1u << (kCmdqLog2 + 1)) - 1u;
    for (u32 spin = 0; spin < kSyncSpins; ++spin) {
        const u32 cons = readl(kRegCmdqCons);
        if ((cons & wrap_mask) != want) {
            continue;
        }
        if ((cons & (kCmdqConsErrMask << kCmdqConsErrShift)) != 0) {
            ANKO_LOGF_ERROR("iommu/smmuv3", "command queue error: cons=0x%x", cons);
            return E_FAULT;
        }
        return E_OK;
    }
    ANKO_LOG_ERROR("iommu/smmuv3", "command queue sync timed out");
    return E_FAULT;
}

// ---------------------------------------------------------------------------
// Stream table
// ---------------------------------------------------------------------------
SysError program_ste_locked(u32 sid, const Ste& ste) {
    if (sid >= g_sid_limit) {
        return E_INVALID; // out-of-range stream id: never silently bypassed
    }
    Ste& entry = *reinterpret_cast<Ste*>(g_stream_table + sid * kSteBytes);
    libk::memcpy(entry.w, ste.w, sizeof(entry.w));
    clean_to_poc(reinterpret_cast<PhysAddr>(&entry), kSteBytes);
    // A configuration STE change is only observed after CFGI_STE; without it a
    // cached stream configuration could keep translating with the old context.
    cq_submit_locked(make_cmd_cfgi_ste(sid));
    return cq_sync_locked();
}

// STE for a requester that must not translate: stage-2 enabled but pointed at
// an all-zero root table, so every lookup misses and is reported as a fault.
Ste deny_ste() {
    return make_ste_stage2(reinterpret_cast<PhysAddr>(g_deny_table), kDenyVmid, kT0Sz, kS2Sl0Level1,
                           kS2Ps44, true);
}

SysError program_deny_locked(u32 sid) {
    return program_ste_locked(sid, deny_ste());
}

// ---------------------------------------------------------------------------
// PCI configuration-space access (for bus-master containment)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Backend hooks
// ---------------------------------------------------------------------------
SysError hook_publish(void*, PhysAddr table_pa, u64 length) {
    if (!table_pa || length == 0) {
        return E_INVALID;
    }
    clean_to_poc(table_pa, length);
    return E_OK;
}

SysError hook_invalidate(void*, IommuDomain* domain, u64 iova, u64 length) {
    if (g_state != State::Ready || !domain) {
        return E_FAULT;
    }
    const u16 vmid = static_cast<u16>(domain_index(*domain));
    kernel::LockGuard guard(g_lock);
    if (length == ~0ull) {
        // Domain-scoped invalidation (used by teardown).
        cq_submit_locked(make_cmd_tlbi_s12_vmall(vmid));
    } else {
        u32 pages = static_cast<u32>((length + PAGE_SIZE - 1) / PAGE_SIZE);
        if (pages == 0) {
            pages = 1;
        }
        cq_submit_locked(make_cmd_tlbi_s2_ipa(vmid, iova, pages));
    }
    return E_OK;
}

SysError hook_sync(void*) {
    if (g_state != State::Ready) {
        return E_FAULT;
    }
    kernel::LockGuard guard(g_lock);
    return cq_sync_locked();
}

SysError hook_attach(void*, IommuDeviceId device, IommuDomain* domain) {
    return smmuv3_program_stream(device, domain);
}

SysError hook_detach(void*, IommuDeviceId device) {
    if (device.segment != 0) {
        return E_INVALID;
    }
    return smmuv3_program_deny(device);
}

bool hook_ready(void*) {
    return g_state == State::Ready;
}

const IommuBackendOps g_backend = {
    hook_publish,        hook_invalidate, hook_sync, hook_attach, hook_detach,
    /* ctx = */ nullptr,
    /* table_format = */ &kArmStage2Tables,
    /* iova_bits = */ static_cast<u8>(kIommuIovaBits),
    /* pa_bits = */ 48,
    /* hardware = */ true,
    /* ready = */ hook_ready,
};

} // namespace

// ---------------------------------------------------------------------------
// smmuv3.h API
// ---------------------------------------------------------------------------

bool smmuv3_ready() {
    return g_state == State::Ready;
}

const IommuBackendOps& smmuv3_backend() {
    return g_backend;
}

PhysAddr smmuv3_base() {
    return g_base;
}

u32 smmuv3_gerror() {
    if (g_state != State::Ready) {
        return 0;
    }
    return readl(kRegGerror);
}

bool smmuv3_debug_ste(u32 sid, u32 out[16]) {
    if (g_state != State::Ready || sid >= g_sid_limit) {
        return false;
    }
    const Ste* entry = reinterpret_cast<const Ste*>(g_stream_table + sid * kSteBytes);
    for (u32 i = 0; i < 16; ++i) {
        out[i] = entry->w[i];
    }
    return true;
}

SysError smmuv3_bring_up() {
    if (g_state == State::Ready) {
        return E_OK;
    }
    if (g_state == State::Failed) {
        return E_FAULT;
    }
    // No SMMU: the core stays on the null backend.  This is the default
    // topology, so the default verification gate is unaffected.
    if (!g_hw.has_smmu || g_hw.smmu_base == 0) {
        return E_OK;
    }

    kernel::LockGuard guard(g_lock);
    g_base = g_hw.smmu_base;

    // Fail closed first: while the SMMU is disabled GBPA.ABORT makes every
    // stream terminate rather than bypass, so if anything below fails to reach
    // an enabled, deny-by-default state the platform still does not bypass.
    writel(kRegGbpa, kGbpaUpdate | kGbpaAbort);

    const u32 idr0 = readl(kRegIdr0);
    const u32 idr1 = readl(kRegIdr1);
    const u32 idr5 = readl(kRegIdr5);
    if ((idr0 & kIdr0S2P) == 0) {
        // Stage-2 translation is the construct this backend relies on.  Without
        // it the Phase C backend cannot enforce a default-deny IOVA, so it
        // refuses to enable and leaves the SMMU in global-abort (no bypass).
        ANKO_LOG_ERROR("iommu/smmuv3",
                       "no stage-2 translation support; Phase C cannot enforce isolation, "
                       "leaving the SMMU in global-abort (no bypass)");
        g_state = State::Failed;
        return E_NOTIMPL;
    }
    const u32 hw_sid_size = idr1_sid_size(idr1);
    g_sid_limit = 1u << (hw_sid_size < kStreamLog2 ? hw_sid_size : kStreamLog2);

    ANKO_LOGF_INFO("iommu/smmuv3",
                   "idr0=0x%x idr1=0x%x idr5=0x%x sid_bits=%u cmdq_bits=%u evtq_bits=%u oas=%u",
                   idr0, idr1, idr5, hw_sid_size, idr1_cmdq_size(idr1), idr1_eventq_size(idr1),
                   idr5_oas(idr5));

    // Linear stream table, all entries invalid (=> fault).
    libk::memset(g_stream_table, 0, sizeof(g_stream_table));
    clean_to_poc(reinterpret_cast<PhysAddr>(g_stream_table), sizeof(g_stream_table));
    writel(kRegStrTabBase, static_cast<u32>(reinterpret_cast<PhysAddr>(g_stream_table)));
    writel(kRegStrTabBase + 4, static_cast<u32>(reinterpret_cast<PhysAddr>(g_stream_table) >> 32));
    writel(kRegStrTabBaseCfg, make_strtab_base_cfg_linear(kStreamLog2));

    // Command queue.
    libk::memset(g_cmdq, 0, sizeof(g_cmdq));
    g_cmdq_prod = 0;
    const PhysAddr cq_pa = reinterpret_cast<PhysAddr>(g_cmdq);
    writel(kRegCmdqBase, static_cast<u32>(make_queue_base(cq_pa, kCmdqLog2)));
    writel(kRegCmdqBase + 4, static_cast<u32>(make_queue_base(cq_pa, kCmdqLog2) >> 32));
    writel(kRegCmdqProd, 0);
    writel(kRegCmdqCons, 0);

    // Event queue.
    libk::memset(g_evq, 0, sizeof(g_evq));
    g_evq_cons = 0;
    const PhysAddr evq_pa = reinterpret_cast<PhysAddr>(g_evq);
    writel(kRegEventqBase, static_cast<u32>(make_queue_base(evq_pa, kEventqLog2)));
    writel(kRegEventqBase + 4, static_cast<u32>(make_queue_base(evq_pa, kEventqLog2) >> 32));
    writel(kRegEventqProd, 0);
    writel(kRegEventqCons, 0);

    // Poll the queues; no SMMU interrupts are enabled.  CR2 configures MSI and
    // stream matching, neither of which this mode uses, so it stays at its reset
    // value and IRQ_CTRL is explicitly cleared.
    writel(kRegIrqCtrl, 0);
    writel(kRegCr2, 0);

    // Enable.  From this point an unmapped/unbound stream faults.
    const u32 cr0 = kCr0SmmuEn | kCr0CmdqEn | kCr0EventqEn;
    writel(kRegCr0, cr0);
    for (u32 spin = 0; spin < kSyncSpins; ++spin) {
        if ((readl(kRegCr0Ack) & cr0) == cr0) {
            break;
        }
        if (spin + 1 == kSyncSpins) {
            ANKO_LOG_ERROR("iommu/smmuv3", "CR0 did not acknowledge");
            g_state = State::Failed;
            return E_FAULT;
        }
    }

    // Confirm the command stream is live before anything can DMA.
    if (cq_sync_locked() != E_OK) {
        g_state = State::Failed;
        return E_FAULT;
    }

    g_state = State::Ready;
    // Route every stream on this platform's single PCI segment through the
    // verified backend. register_backend() validates the ops table (hardware
    // backends supply every hook and explicit geometry) and rejects overlap;
    // a registration failure means the routing core would deny every stream,
    // so fail bring-up rather than run unrouted.
    {
        const IommuRoute route{pci::kSegment, 0, g_sid_limit};
        const SysError rc = register_backend(route, g_backend);
        if (rc != E_OK) {
            g_state = State::Failed;
            ANKO_LOGF_ERROR("iommu/smmuv3", "backend registration failed rc=%d", static_cast<int>(rc));
            return rc;
        }
    }
    ANKO_LOGF_INFO("iommu/smmuv3", "enabled base=0x%llx sid_limit=%u cr0ack=0x%x routes=0..%u",
                   static_cast<unsigned long long>(g_base), g_sid_limit, readl(kRegCr0Ack),
                   g_sid_limit - 1);
    return E_OK;
}

SysError smmuv3_program_stream(IommuDeviceId device, IommuDomain* domain) {
    if (g_state != State::Ready) {
        return E_FAULT;
    }
    if (device.segment != 0) {
        return E_INVALID; // single SMMU instance: only segment 0 is addressable
    }
    kernel::LockGuard guard(g_lock);
    if (!domain || domain->root_table_ == 0) {
        return program_deny_locked(device.requester_or_stream_id);
    }
    const u16 vmid = static_cast<u16>(domain_index(*domain));
    return program_ste_locked(
        device.requester_or_stream_id,
        make_ste_stage2(domain->root_table_, vmid, kT0Sz, kS2Sl0Level1, kS2Ps44, true));
}

SysError smmuv3_program_deny(IommuDeviceId device) {
    if (g_state != State::Ready) {
        return E_FAULT;
    }
    if (device.segment != 0) {
        return E_INVALID;
    }
    kernel::LockGuard guard(g_lock);
    return program_deny_locked(device.requester_or_stream_id);
}

u32 smmuv3_drain_events() {
    if (g_state != State::Ready) {
        return 0;
    }
    // Take the event record (and advance the consumer index) under the SMMU
    // lock, then decode it without the lock: the decoding path may program a
    // new STE (quarantine), which takes the same lock.
    u32 drained = 0;
    while (drained < kMaxDrainPerCall) {
        Evt evt;
        {
            kernel::LockGuard guard(g_lock);
            const u32 prod = readl(kRegEventqProd);
            if (q_wrap_index(g_evq_cons, kEventqLog2) == q_wrap_index(prod, kEventqLog2)) {
                break;
            }
            libk::memcpy(evt.w, g_evq[q_index(g_evq_cons, kEventqLog2)].w, sizeof(evt.w));
            ++g_evq_cons;
            ++g_events_drained;
            writel(kRegEventqCons, q_wrap_index(g_evq_cons, kEventqLog2));
        }
        faults::on_event(evt);
        ++drained;
    }
    return drained;
}

u64 smmuv3_events_drained() {
    return g_events_drained;
}

// ---------------------------------------------------------------------------
// smmuv3_internal.h: hardware primitives used by faults.cc
// ---------------------------------------------------------------------------
namespace smmuv3 {

bool ready() {
    return g_state == State::Ready;
}

u32 sid_limit() {
    return g_sid_limit;
}

SysError program_deny(u32 sid) {
    if (g_state != State::Ready) {
        return E_FAULT;
    }
    kernel::LockGuard guard(g_lock);
    return program_deny_locked(sid);
}

bool clear_pci_bus_master(u32 sid) {
    if (!g_hw.has_pcie || g_hw.ecam_base == 0) {
        return false;
    }
    u8 bus = 0;
    u8 dev = 0;
    u8 fn = 0;
    pci::bdf_from_requester_id(sid, bus, dev, fn);
    // Halfword access at the command register: the adjacent status register
    // holds W1C bits and must not be written back.
    volatile u8* cmd = reinterpret_cast<volatile u8*>(
        pci::ecam_offset(g_hw.ecam_base, bus, dev, fn, pci::kRegCommand));
    asm volatile("dsb sy" ::: "memory");
    const u16 value = static_cast<u16>(pci::cfg_read_word(cmd, 0) & ~pci::kPciCmdBusMaster);
    asm volatile("dsb sy" ::: "memory");
    pci::cfg_write_word(cmd, 0, value);
    asm volatile("dsb sy" ::: "memory");
    return true;
}

} // namespace smmuv3

} // namespace iommu
