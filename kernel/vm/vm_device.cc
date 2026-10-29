// kernel/vm/vm_device.cc — VM device-assignment lifecycle (Phase D).
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §6.1 (assignment object), §6.2 (MMIO),
// §6.3 (filtered config, mediation), §8.11 (secure attach), §8.12 (DMA-safe
// revoke).
//
// A VmDeviceAssignment binds a set of host resources to exactly one VM and is
// the single lifetime/revocation point for them. It owns:
//   * the IommuDomain the assigned functions DMA through (Linux never receives
//     an IOMMU capability);
//   * the guest MMIO (BAR) regions mapped as stage-2 Device memory;
//   * the host IRQ routes, translated into vGIC pending bits;
//   * the synthetic/filtered guest PCI configuration view;
//   * a retained reference on every host object it names.
//
// The secure-attach state machine (§8.11) runs in the exact order of the spec,
// so a partially-built assignment can never be bus-mastering. On any failure the
// function is left non-bus-mastering and the assignment is FAILED. The revoke
// state machine (§8.12) tears translations down before any host page is released
// and quarantines (keeping pages pinned) when the backend cannot prove DMA
// stopped.

#include "anko/vm.h"
#include "anko/panic.h"

#include "anko/hw_info.h"
#include "anko/irq.h"
#include "anko/log.h"
#include "anko/smmuv3.h"
#include "anko/spinlock.h"
#include "libk/string.h"
#include "libk/printf.h"

namespace drivers::timer {
u64 tick_count();
}

namespace vm {

namespace {

// Identity an assignment's DMA domain is authorised against. Distinct per
// assignment so one assignment can never DMA into another's domain.
constexpr u64 kApplianceObjectTag = 0x414D'4544'4556'4943ull; // "AMEDEVIC"

struct DevicePool {
    VmDeviceAssignment slots[kMaxVmDevices];
    u32 next_id;
    u32 reserved;
    kernel::SpinLock lock;
};

DevicePool g_pool{};

// Synthetic config-space byte helpers. Only the bytes the mediator decides are
// harmless ever reach the image.
void space_store(u8* space, u32 offset, u8 lo, u8 hi) {
    if (offset + 1 >= pci::kCfgSpaceBytes) {
        return;
    }
    space[offset] = lo;
    space[offset + 1] = hi;
}

void space_store_sized(u8* space, u32 offset, u64 value, u32 size) {
    if (size == 0 || size > 4 || offset >= pci::kCfgSpaceBytes) {
        return;
    }
    if (offset + size > pci::kCfgSpaceBytes) {
        return;
    }
    for (u32 i = 0; i < size; ++i) {
        space[offset + i] = static_cast<u8>((value >> (i * 8u)) & 0xFFu);
    }
}

VmDeviceAssignment* alloc_slot() {
    for (u32 i = 0; i < kMaxVmDevices; ++i) {
        if (g_pool.slots[i].lifetime == nullptr && g_pool.slots[i].device_id == 0) {
            return &g_pool.slots[i];
        }
    }
    return nullptr;
}

// Page-owner query for the assignment's authorised DMA ranges (§8.3, §8.6).
// The manager's numeric PA is never treated as authority: a page is DMA-visible
// only if it falls in a range the assignment explicitly authorised.
bool appliance_owner_query(void* ctx, PhysAddr pa, iommu::PageOwnership* out) {
    auto* a = static_cast<VmDeviceAssignment*>(ctx);
    if (!a || !out) {
        return false;
    }
    for (u32 i = 0; i < a->ram_count; ++i) {
        const VmAssignRam& r = a->ram[i];
        if (pa < r.base || pa >= r.base + r.size) {
            continue;
        }
        out->object = a->authorized_object_;
        out->dma_class = iommu::VmDmaClass::ApplianceRam;
        libk::memset(out->pad, 0, sizeof(out->pad));
        return true;
    }
    return false;
}

void appliance_release(void*, PhysAddr, u64) {
    // Appliance RAM pages are pinned by the owning VM memory object for its
    // whole lifetime; the IOMMU domain only releases its translation. The
    // owner unpins the pages at VM teardown, after this domain is destroyed.
}

const iommu::IommuBackendOps* assignment_backend(const VmDeviceAssignment& assignment) {
    const iommu::IommuBackendOps* backend = nullptr;
    for (u32 i = 0; i < assignment.fn_count; ++i) {
        const auto& fn = assignment.fns[i].info;
        const auto* route = iommu::backend_for({static_cast<u32>(fn.segment), fn.requester_id});
        // One domain spans only one translation controller. Multiple routed
        // controllers need separate domains, not a shared root by assumption.
        if (!route || (backend && (backend->ctx != route->ctx ||
            backend->attach_stream != route->attach_stream))) return nullptr;
        backend = route;
    }
    return backend;
}

// Walk the host capability list for MSI (0x05) / MSI-X (0x11) and return the
// capability offset, or 0 when absent. Reads the ECAM window directly.
u32 find_msi_cap(const pci::PciFunctionInfo& fn, bool* out_msix) {
    if (!g_hw.has_pcie || g_hw.ecam_base == 0) {
        return 0;
    }
    const u32 status = pci::cfg_status_word(fn.bus, fn.device, fn.function);
    if ((status & pci::kStatusCapabilitiesList) == 0) {
        return 0; // capabilities list not present
    }
    u32 off = pci::cfg_read_byte(fn.bus, fn.device, fn.function, pci::kRegCapPtr) & pci::kCfgCapPtrMask;
    for (u32 guard = 0; guard < pci::kMaxCapabilityWalk && off >= pci::kCfgCapMin &&
                      off <= pci::kCfgCapMax;
         ++guard) {
        const u32 header = pci::cfg_read_byte(fn.bus, fn.device, fn.function, off);
        const u32 cap_id = header & 0xFFu;
        if (cap_id == pci::kCapIdMsi) {
            *out_msix = false;
            return off;
        }
        if (cap_id == pci::kCapIdMsix) {
            *out_msix = true;
            return off;
        }
        off = pci::cfg_read_byte(fn.bus, fn.device, fn.function, off + 1) & pci::kCfgCapPtrMask;
    }
    return 0;
}

// Populate the synthetic guest config view for one assigned function.
void expose_function(VmDeviceAssignment* a, u32 fn_index) {
    // Bound first: the record array is fixed, so an out-of-range index must not
    // touch a->fns before it is rejected.
    if (fn_index >= pci::kMaxAssignFns) {
        return;
    }
    const pci::PciFunctionInfo& fn = a->fns[fn_index].info;
    pci::GuestConfig& cfg = a->cfg;
    u8* space = cfg.space[fn_index];
    libk::memset(space, 0, pci::kCfgSpaceBytes);
    space[0x00] = static_cast<u8>(fn.vendor_id & 0xFFu);
    space[0x01] = static_cast<u8>(fn.vendor_id >> 8);
    space[0x02] = static_cast<u8>(fn.device_id & 0xFFu);
    space[0x03] = static_cast<u8>(fn.device_id >> 8);
    // Command: memory decode on, bus-master off until the guest asks and the
    // assignment is ACTIVE.
    space[pci::kRegCommand] = static_cast<u8>(pci::kPciCmdMemoryEnable);
    space[pci::kRegCommand + 1] = 0x00;
    space[pci::kRegRevisionId] = pci::cfg_read_byte(fn.bus, fn.device, fn.function,
                                                   pci::kRegRevisionId);
    space[pci::kRegClassCode] = static_cast<u8>(fn.class_code);
    space[pci::kRegClassCode + 1] = static_cast<u8>(fn.class_code >> 8);
    space[pci::kRegClassCode + 2] = static_cast<u8>(fn.class_code >> 16);
    space[pci::kRegHeaderType] = a->fn_count > 1 ? pci::kHeaderTypeMultiFunction : 0;
    for (u32 offset = pci::kRegSubsystemIds; offset < pci::kRegSubsystemIds + 4; ++offset)
        space[offset] = pci::cfg_read_byte(fn.bus, fn.device, fn.function, offset);
    // Guest INTx routing is installed before ECAM publication.
    space[pci::kRegIntPin] = pci::cfg_read_byte(fn.bus, fn.device, fn.function, pci::kRegIntPin);
    cfg.present[fn_index] = true;
}

SysError map_one_bar_page(VmDeviceAssignment* a, u64 ipa, PhysAddr pa, u32 rights) {
    if (!a->owner_vm) {
        return E_INVALID;
    }
    return stage2_map_page_ex(&a->owner_vm->stage2, ipa, pa, rights, Stage2MemType::Device);
}

// Authorise the owner VM's mapped RAM as the appliance DMA aperture (§8.2,
// §8.5). The authoritative source is the VM memory ownership structure; here we
// coalesce its backing pages into maximal physically-contiguous runs, each with
// IOVA == the run's guest physical address. Capacity is fixed (kMaxAssignRam),
// so an unusually fragmented VM logs and truncates rather than overflowing.
SysError collect_vm_ram(VmDeviceAssignment* a) {
    Vm* v = a->owner_vm;
    if (!v) {
        return E_INVALID;
    }
    for (u32 i = 0; i < v->memory_count; ++i) {
        VmMemory* mem = v->memory_objects[i];
        if (!mem || mem->dead || !mem->mapped_stage2 || !mem->pages) {
            continue;
        }
        if (mem->shared_staging) continue; // staging pages are never DMA-authorizable
        u64 p = 0;
        while (p < mem->page_count) {
            const PhysAddr start = mem->pages[p];
            if (start == 0) {
                ++p;
                continue;
            }
            u64 len = PAGE_SIZE;
            u64 q = p + 1;
            while (q < mem->page_count && mem->pages[q] == start + len) {
                len += PAGE_SIZE;
                ++q;
            }
            const u64 iova = mem->guest_pa_base + p * PAGE_SIZE;
            // Capacity exhaustion is the only failure here that means "too
            // fragmented"; every other failure (malformed range, bad IOVA) is
            // propagated as its real SysError instead of being collapsed.
            const SysError rc = device_authorize_ram(a, start, len, iova);
            if (rc != E_OK) {
                return rc;
            }
            p = q;
        }
    }
    return a->ram_count ? E_OK : E_INVALID;
}

} // namespace

// Drop the assignment's creator reference exactly once. Several teardown paths
// (explicit destroy, VM retire, owner death) can race to release it; the
// retained body keeps any pinned pages alive until every capability reference
// is gone.
static void release_body_once(VmDeviceAssignment* a) {
    if (!a || !a->lifetime || a->body_released) {
        return;
    }
    a->body_released = true;
    object::release(a->lifetime);
}

// ---------------------------------------------------------------------------
// Pool / lifetime
// ---------------------------------------------------------------------------

const char* device_state_name(u32 state) {
    switch (state) {
    case VM_DEV_EMPTY:
        return "empty";
    case VM_DEV_CLAIMED:
        return "claimed";
    case VM_DEV_CONTAINED:
        return "contained";
    case VM_DEV_QUIESCED:
        return "quiesced";
    case VM_DEV_DOMAIN_READY:
        return "domain-ready";
    case VM_DEV_RAM_MAPPED:
        return "ram-mapped";
    case VM_DEV_STREAMS_ATTACHED:
        return "streams-attached";
    case VM_DEV_SYNCED:
        return "synced";
    case VM_DEV_IRQS_INSTALLED:
        return "irqs-installed";
    case VM_DEV_EXPOSED:
        return "exposed";
    case VM_DEV_BME_ALLOWED:
        return "bme-allowed";
    case VM_DEV_ACTIVE:
        return "active";
    case VM_DEV_FAILED:
        return "failed";
    case VM_DEV_REVOKING:
        return "revoking";
    case VM_DEV_REVOKED:
        return "revoked";
    case VM_DEV_QUARANTINED:
        return "quarantined";
    default:
        return "unknown";
    }
}

void device_dispose(void* target) {
    auto* a = static_cast<VmDeviceAssignment*>(target);
    if (!a) {
        return;
    }
    for (u32 i = 0; i < a->retained_count; ++i) {
        object::release(a->retained[i]);
    }
    a->retained_count = 0;
    const u32 saved_id = a->device_id;
    libk::memset(a, 0, sizeof(*a));
    (void)saved_id;
}

VmDeviceAssignment* device_alloc(Vm* owner, u64 flags, SysError* out_error) {
    kernel::LockGuard guard(g_pool.lock);
    VmDeviceAssignment* a = alloc_slot();
    if (!a) {
        if (out_error) {
            *out_error = E_OVERFLOW;
        }
        note_error();
        return nullptr;
    }
    libk::memset(a, 0, sizeof(*a));
    a->device_id = g_pool.next_id++;
    if (a->device_id == 0) {
        a->device_id = g_pool.next_id++;
    }
    a->owner_vm = owner;
    a->owner_identity = owner ? owner->owner_identity : 0;
    a->owner_pid = owner ? owner->owner_pid : 0;
    a->kind = VmDeviceKind::Pci;
    a->flags = flags;
    a->state = VM_DEV_EMPTY;
    a->authorized_object_ = kApplianceObjectTag ^ static_cast<u64>(a->device_id);
    a->created_ticks = drivers::timer::tick_count();
    a->lifetime = object::create(a, device_dispose);
    if (!a->lifetime) {
        libk::memset(a, 0, sizeof(*a));
        if (out_error) {
            *out_error = E_NOMEM;
        }
        note_error();
        return nullptr;
    }
    metric_inc(VM_DEBUG_VM_DEVICE_CREATED);
    if (out_error) {
        *out_error = E_OK;
    }
    return a;
}

cap::Borrow<VmDeviceAssignment> assignment_from_cap(Process& caller, CapHandle cap, u32 rights,
                                                    SysError* out_error) {
    cap::Ref e = require_cap(caller, cap, CAP_VM_DEVICE, rights, out_error);
    auto* a = e ? static_cast<VmDeviceAssignment*>(e->target) : nullptr;
    if (!a || !a->lifetime || a->state == VM_DEV_REVOKED || a->state == VM_DEV_EMPTY) {
        if (out_error) {
            *out_error = E_DEAD;
        }
        note_error();
        return nullptr;
    }
    return cap::Borrow<VmDeviceAssignment>(e);
}

// ---------------------------------------------------------------------------
// Authorised RAM
// ---------------------------------------------------------------------------

SysError device_authorize_ram(VmDeviceAssignment* a, PhysAddr base, u64 size, u64 iova) {
    if (!a || size == 0 || (base & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0) {
        return E_INVALID;
    }
    if (iova >= (1ull << iommu::kIommuIovaBits)) {
        return E_INVALID;
    }
    if (a->ram_count >= kMaxAssignRam) {
        return E_OVERFLOW;
    }
    a->ram[a->ram_count++] = VmAssignRam{base, size, iova, false, {}};
    return E_OK;
}

SysError device_map_authorized_ram(VmDeviceAssignment* a) {
    if (!a || !a->dma_domain) {
        return E_INVALID;
    }
    for (u32 i = 0; i < a->ram_count; ++i) {
        VmAssignRam& r = a->ram[i];
        if (r.mapped) {
            continue;
        }
        iommu::IommuMap map{};
        map.iova = r.iova;
        map.pa = r.base;
        map.length = r.size;
        map.perms =
            static_cast<u32>(iommu::DmaPerm::Read) | static_cast<u32>(iommu::DmaPerm::Write);
        map.reserved = 0;
        const SysError rc = iommu::domain_map_batch(*a->dma_domain, &map, 1);
        if (rc != E_OK) {
            return rc;
        }
        r.mapped = true;
    }
    return E_OK;
}

// ---------------------------------------------------------------------------
// §8.11 secure-attach state machine
// ---------------------------------------------------------------------------

namespace {

SysError step_claim(VmDeviceAssignment* a, const DeviceAssignSpec& spec) {
    if (spec.fn_count == 0 || spec.fn_count > pci::kMaxAssignFns) {
        return E_INVALID;
    }
    for (u32 i = 0; i < spec.fn_count; ++i) {
        pci::PciFunctionInfo info{};
        if (!pci::find_function(g_hw.ecam_base, spec.fns[i].bus, spec.fns[i].device,
                                spec.fns[i].function, &info)) {
            return E_NOTFOUND;
        }
        if (info.requester_id != spec.fns[i].requester_id || info.segment != spec.fns[i].segment) {
            return E_INVALID; // inventory changed under us: refuse
        }
        a->fns[a->fn_count].info = info;
        a->fns[a->fn_count].primary = (i == 0);
        ++a->fn_count;
    }
    a->state = VM_DEV_CLAIMED;
    ANKO_LOGF_INFO("kernel/vm", "assign: claimed device=%u group=%u fns=%u", a->device_id,
                   spec.group_id, a->fn_count);
    return E_OK;
}

SysError step_contain(VmDeviceAssignment* a) {
    u32 contained = 0;
    for (u32 i = 0; i < a->fn_count; ++i) {
        const pci::PciFunctionInfo& fn = a->fns[i].info;
        pci::set_bus_master(fn.bus, fn.device, fn.function, false);
        if ((pci::read_command(fn.bus, fn.device, fn.function) & pci::kPciCmdBusMaster) == 0) {
            ++contained;
        }
    }
    a->state = VM_DEV_CONTAINED;
    ANKO_LOGF_INFO("kernel/vm", "assign: contained bm=0 fns=%u/%u", contained, a->fn_count);
    return (contained == a->fn_count) ? E_OK : E_FAULT;
}

SysError step_quiesce(VmDeviceAssignment* a) {
    // The QEMU reference functions (virtio-gpu-pci, iommu-testdev) require no
    // reset before assignment; a physical function that does would be reset
    // here via its own reset capability. Keeping this an explicit, ordered step
    // means the transition is covered by the "bus master clear" assertion above.
    a->state = VM_DEV_QUIESCED;
    return E_OK;
}

SysError step_domain(VmDeviceAssignment* a) {
    const auto* backend = assignment_backend(*a);
    if (!backend) return E_NOTIMPL;
    iommu::IommuOwnerOps owner{};
    owner.query = appliance_owner_query;
    owner.release = appliance_release;
    owner.ctx = a;
    const SysError rc =
        iommu::domain_create(a->authorized_object_, owner, *backend, &a->dma_domain);
    if (rc != E_OK) {
        return rc;
    }
    a->state = VM_DEV_DOMAIN_READY;
    ANKO_LOGF_INFO("kernel/vm", "assign: domain ready index=%u",
                   iommu::domain_index(*a->dma_domain));
    return E_OK;
}

SysError step_map_ram(VmDeviceAssignment* a) {
    const SysError rc = device_map_authorized_ram(a);
    if (rc != E_OK) {
        return rc;
    }
    a->state = VM_DEV_RAM_MAPPED;
    ANKO_LOGF_INFO("kernel/vm", "assign: ram mapped ranges=%u pages=%u", a->ram_count,
                   iommu::domain_pinned_pages(*a->dma_domain));
    return E_OK;
}

SysError step_attach_streams(VmDeviceAssignment* a) {
    for (u32 i = 0; i < a->fn_count; ++i) {
        const pci::PciFunctionInfo& fn = a->fns[i].info;
        const iommu::IommuDeviceId device{static_cast<u32>(fn.segment), fn.requester_id};
        const SysError rc = iommu::domain_attach(*a->dma_domain, device);
        if (rc != E_OK) {
            return rc;
        }
    }
    a->state = VM_DEV_STREAMS_ATTACHED;
    ANKO_LOGF_INFO("kernel/vm", "assign: streams attached n=%u", a->fn_count);
    return E_OK;
}

SysError step_sync(VmDeviceAssignment* a) {
    if (!a->dma_domain) {
        return E_INVALID;
    }
    const SysError rc = iommu::domain_sync(*a->dma_domain);
    if (rc != E_OK) {
        return rc;
    }
    a->state = VM_DEV_SYNCED;
    return E_OK;
}

SysError step_install_irqs(VmDeviceAssignment* a) {
    for (u32 i = 0; i < a->irq_count; ++i) {
        if (!a->irq[i].installed) {
            const SysError rc = device_irq_install(a, i);
            if (rc != E_OK) {
                return rc;
            }
        }
    }
    a->state = VM_DEV_IRQS_INSTALLED;
    ANKO_LOGF_INFO("kernel/vm", "assign: routes installed=%u count=%llu", a->irq_count,
                   static_cast<unsigned long long>(device_irq_route_count()));
    return E_OK;
}

SysError step_expose(VmDeviceAssignment* a) {
    a->cfg.fn_count = a->fn_count;
    for (u32 i = 0; i < a->fn_count; ++i) {
        expose_function(a, i);
        bool msix = false;
        const u32 cap = find_msi_cap(a->fns[i].info, &msix);
        a->msi[i].present = cap != 0;
        a->msi[i].msix = msix;
        a->msi[i].enabled = false;
        a->msi[i].cap_offset = cap;
        a->msi[i].nvectors = 1;
        a->msi[i].msg = pci::MsiMessage{};
        a->cfg.msi_cap_off[i] = static_cast<u16>(cap);
        // Enable host memory decoding so the assigned BAR is reachable.
        pci::set_memory_enable(a->fns[i].info.bus, a->fns[i].info.device, a->fns[i].info.function,
                               true);
    }
    a->state = VM_DEV_EXPOSED;
    ANKO_LOGF_INFO("kernel/vm", "assign: exposed fns=%u msi=%u", a->fn_count,
                   a->msi[0].present ? 1u : 0u);
    return E_OK;
}

SysError step_allow_bme(VmDeviceAssignment* a) {
    for (u32 i = 0; i < a->fn_count; ++i) {
        if (i < pci::kMaxAssignFns) {
            a->cfg.bme_allowed[i] = true;
        }
    }
    a->state = VM_DEV_BME_ALLOWED;
    return E_OK;
}

// Undo everything the attach machine installed, leaving the function
// non-bus-mastering and no translation reachable (§8.11 failure clause).
void abort_attach(VmDeviceAssignment* a) {
    for (u32 i = 0; i < a->irq_count; ++i) {
        device_irq_uninstall(a, i);
    }
    for (u32 i = 0; i < a->fn_count; ++i) {
        const pci::PciFunctionInfo& fn = a->fns[i].info;
        pci::set_bus_master(fn.bus, fn.device, fn.function, false);
        if (a->dma_domain) {
            (void)iommu::domain_detach(
                *a->dma_domain,
                iommu::IommuDeviceId{static_cast<u32>(fn.segment), fn.requester_id});
        }
    }
    if (a->dma_domain) {
        if (iommu::domain_destroy(a->dma_domain) != E_OK) {
            a->state = VM_DEV_QUARANTINED;
            metric_inc(VM_DEBUG_VM_DEVICE_QUARANTINE);
            return;
        }
        a->dma_domain = nullptr;
    }
    a->state = VM_DEV_FAILED;
}

} // namespace

// Direct -kernel boot has no firmware, so the assigned function's BARs read as 0
// and must be programmed from the PCIe MMIO window before anything can map them
// (Phase E.3).  This is the host-side BAR assignment the ECAM model then reports
// to the guest.
static SysError step_assign_bars(VmDeviceAssignment* a) {
    if ((a->flags & kAssignFlagAssignBars) == 0) {
        return E_OK; // firmware-assigned BARs (or not needed for this platform)
    }
    if (!g_hw.has_pci_mmio || g_hw.pci_mmio_size == 0) {
        return E_NOTFOUND;
    }
    // Never alias a BAR another function already owns.  Native devices (the xHCI)
    // keep firmware/native-assigned BARs inside the same window, so start the
    // allocator above the highest one the inventory reports.
    u64 cursor = 0;
    {
        static pci::PciFunctionInfo others[pci::kMaxFunctions];
        const u32 count = pci::enumerate(g_hw.ecam_base, others, pci::kMaxFunctions);
        for (u32 i = 0; i < count; ++i) {
            for (u32 b = 0; b < pci::kMaxBars; ++b) {
                const pci::PciBar& other = others[i].bars[b];
                if (other.size == 0 || other.base < g_hw.pci_mmio_base) {
                    continue;
                }
                const u64 end = (other.base - g_hw.pci_mmio_base) + other.size;
                if (end > cursor) {
                    cursor = end;
                }
            }
        }
    }
    for (u32 i = 0; i < a->fn_count; ++i) {
        const auto& fn = a->fns[i].info;
        pci::set_memory_enable(fn.bus, fn.device, fn.function, false);
        if (!pci::assign_bars(&a->fns[i].info, g_hw.pci_mmio_base, g_hw.pci_mmio_size, &cursor)) {
            return E_INVALID;
        }
    }
    ANKO_LOGF_INFO("kernel/vm", "assign: bars window=0x%lx size=0x%lx bar1=0x%lx bar4=0x%lx",
                   static_cast<unsigned long>(g_hw.pci_mmio_base),
                   static_cast<unsigned long>(g_hw.pci_mmio_size),
                   static_cast<unsigned long>(a->fns[0].info.bars[1].base),
                   static_cast<unsigned long>(a->fns[0].info.bars[4].base));
    return E_OK;
}

SysError device_attach(VmDeviceAssignment* a, const DeviceAssignSpec& spec) {
    // An assignment without an owner VM cannot map anything; fail before the
    // stage-2 headroom check dereferences a->owner_vm.
    if (!a || !a->owner_vm) {
        return E_INVALID;
    }
    SysError rc = step_claim(a, spec);
    if (rc == E_OK) rc = step_contain(a);
    if (rc == E_OK) rc = step_assign_bars(a);
    if (rc == E_OK) {
        rc = step_quiesce(a);
    }
    if (rc == E_OK) {
        rc = step_domain(a);
    }
    if (rc == E_OK) {
        rc = step_map_ram(a);
    }
    // Guest RAM coverage may be many pages per record. Reserve record slots
    // without unsigned subtraction at the caller (stage2_has_capacity guards it).
    if (rc == E_OK &&
        !stage2_has_capacity(&a->owner_vm->stage2, kMaxAssignMmio * kMaxAssignMmioSlackPages)) {
        ANKO_LOG_ERROR("kernel/vm",
                       "assign: guest RAM leaves no stage-2 headroom for device mappings");
        rc = E_OVERFLOW;
    }
    if (rc == E_OK) {
        rc = step_attach_streams(a);
    }
    if (rc == E_OK) {
        rc = step_sync(a);
    }
    if (rc == E_OK) {
        rc = step_install_irqs(a);
    }
    if (rc == E_OK) {
        rc = step_expose(a);
    }
    if (rc == E_OK) {
        rc = step_allow_bme(a);
    }

    if (rc != E_OK) {
        ANKO_LOGF_ERROR("kernel/vm", "assign: attach failed at %s rc=%d",
                        device_state_name(a->state), static_cast<int>(rc));
        abort_attach(a);
        return rc;
    }
    a->state = VM_DEV_ACTIVE;
    a->active = true;
    metric_inc(VM_DEBUG_VM_DEVICE_ACTIVE);
    ANKO_LOGF_INFO("kernel/vm", "assign: ACTIVE device=%u", a->device_id);
    return E_OK;
}

// ---------------------------------------------------------------------------
// Command / MSI mediation (§6.3, §7.3)
// ---------------------------------------------------------------------------

namespace {

SysError add_or_update_msi_route(VmDeviceAssignment* a, u32 fn_index, u32 vector) {
    if (fn_index >= a->fn_count) {
        return E_INVALID;
    }
    // Reuse an existing MSI route for this function; otherwise install one.
    for (u32 i = 0; i < a->irq_count; ++i) {
        if (a->irq[i].fn_index == fn_index && a->irq[i].msi) {
            a->irq[i].guest_irq = vector;
            if (a->state >= VM_DEV_EXPOSED && !a->irq[i].installed) {
                return device_irq_install(a, i);
            }
            return E_OK;
        }
    }
    if (a->irq_count >= kMaxAssignIrqs) {
        return E_OVERFLOW;
    }
    // Only reachable when the assignment owner has NOT reserved a host MSI line
    // for this function: the INTx bootstrap profile reserves INTx only, and both
    // the assignment selftest and the appliance profile take the reuse branch
    // above. Inventing a route with host_irq = 0 would accept a guest MSI
    // programming write that could never be delivered, so refuse by name instead
    // of counting it as a route. Host MSI/MSI-X line allocation ships with the
    // physical/AMD appliance work (spec §8.13, phases P–Q) and must bring a
    // delivery test with it.
    ANKO_LOG_ERROR("kernel/vm", "msi: no host MSI line reserved for this function");
    return E_NOTIMPL;
}

} // namespace

SysError device_mediate_command(VmDeviceAssignment* a, u32 fn_index, u16 value) {
    if (!a || fn_index >= a->fn_count || fn_index >= pci::kMaxAssignFns) {
        return E_INVALID;
    }
    pci::GuestConfig& cfg = a->cfg;
    // Store the benign bits the guest asked for, but only honour bus-master when
    // the assignment has reached the ACTIVE-ready state (§8.11).
    u16 requested = static_cast<u16>(value & pci::kPciCmdWritableMask);
    const bool want_bme = (requested & pci::kPciCmdBusMaster) != 0;
    if (want_bme && (!a->active || a->state != VM_DEV_ACTIVE || !a->dma_domain ||
                     iommu::domain_state(*a->dma_domain) != iommu::DomainState::Active ||
                     !cfg.bme_allowed[fn_index])) {
        // Reject the BME bit rather than silently dropping it.
        requested &= ~pci::kPciCmdBusMaster;
        metric_inc(VM_DEBUG_VM_DEVICE_MEDIATED_DENY);
    }
    const u8 lo = static_cast<u8>(requested & 0xFFu);
    const u8 hi = static_cast<u8>(requested >> 8);
    space_store(cfg.space[fn_index], pci::kRegCommand, lo, hi);

    const pci::PciFunctionInfo& fn = a->fns[fn_index].info;
    const bool enable_bme = (requested & pci::kPciCmdBusMaster) != 0;
    pci::set_bus_master(fn.bus, fn.device, fn.function, enable_bme);
    if (enable_bme) {
        metric_inc(VM_DEBUG_VM_DEVICE_BME_ENABLE);
    }
    return E_OK;
}

SysError device_mediate_config_write(VmDeviceAssignment* a, u32 fn_index, u32 offset, u64 value,
                                     u32 size) {
    if (!a || fn_index >= a->fn_count) return E_INVALID;
    if (!a->active || a->state != VM_DEV_ACTIVE) return E_DEAD;
    if (offset >= pci::kRegBar0 && offset < pci::kRegBarEnd &&
        pci::config_bar_write(a->cfg, fn_index, offset, static_cast<u32>(value), size)) return E_OK;
    const pci::CfgWriteAction action = pci::classify_config_write(a->cfg, fn_index, offset, size);
    switch (action) {
    case pci::CfgWriteAction::Reject:
        metric_inc(VM_DEBUG_VM_DEVICE_MEDIATED_DENY);
        return E_PERM;
    case pci::CfgWriteAction::Mediate:
        break;
    case pci::CfgWriteAction::Accept:
        space_store_sized(a->cfg.space[fn_index], offset, value, size);
        return E_OK;
    }

    if (offset >= pci::kRegCommand && offset < pci::kRegCommand + 4) {
        u16 command = static_cast<u16>(pci::config_read(a->cfg, fn_index, pci::kRegCommand, 2));
        for (u32 i = 0; i < size && offset + i < pci::kRegCommand + 2; ++i) {
            const u32 shift = (offset + i - pci::kRegCommand) * 8;
            command = static_cast<u16>((command & ~(0xffu << shift)) |
                      (((value >> (i * 8)) & 0xffu) << shift));
        }
        return device_mediate_command(a, fn_index, command);
    }
    // Otherwise this must be the function's MSI/MSI-X capability window.
    pci::MsiState& msi = a->msi[fn_index];
    if (!msi.present || !pci::msi_decode_write(msi, offset, static_cast<u32>(value), size)) {
        metric_inc(VM_DEBUG_VM_DEVICE_MEDIATED_DENY);
        return E_PERM;
    }
    if (msi.enabled && msi.msg.vector != 0) {
        return add_or_update_msi_route(a, fn_index, msi.msg.vector);
    }
    return E_OK;
}

// ---------------------------------------------------------------------------
// Public syscall-backed operations
// ---------------------------------------------------------------------------

SysError device_secure_attach(Process& caller, CapHandle vm_cap, const pci::HostFunction& host_fn,
                              u64 flags, CapHandle* out_cap) {
    if (!out_cap) {
        return E_INVALID;
    }
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_CONTROL, &rc);
    if (!v) {
        return rc;
    }

    if (!iommu::backend_for({static_cast<u32>(host_fn.info.segment), host_fn.info.requester_id}))
        return E_NOTIMPL;
    if (flags & ~kAssignFlagAssignBars) return E_INVALID;

    // Derive the isolation group from the inventory and require the named
    // function to be in it; assign the whole group or reject (§6.3).
    pci::PciIsolationGroup group{};
    if (!pci::isolation_group_of(g_hw.ecam_base, host_fn.info.bus, host_fn.info.device, &group)) {
        return E_NOTFOUND;
    }
    // CallGuard serializes claims; reject a live owner before touching host state.
    for (const auto& existing : g_pool.slots) {
        if (!existing.lifetime || existing.state == VM_DEV_REVOKED) continue;
        for (u32 i = 0; i < existing.fn_count; ++i) {
            const auto& fn = existing.fns[i].info;
            if (pci::isolation_group_id(fn.segment, fn.bus, fn.device) == group.group_id)
                return E_BUSY;
        }
    }
    DeviceAssignSpec spec{};
    spec.group_id = group.group_id;
    for (u32 fn = 0; fn < 32; ++fn) {
        if ((group.function_mask & (1u << fn)) == 0) {
            continue;
        }
        if (spec.fn_count >= pci::kMaxAssignFns) {
            return E_OVERFLOW;
        }
        pci::PciFunctionInfo info{};
        if (!pci::find_function(g_hw.ecam_base, group.bus, group.device, static_cast<u8>(fn),
                                &info)) {
            return E_NOTFOUND;
        }
        DeviceAssignSpec::Fn& out = spec.fns[spec.fn_count++];
        out.segment = info.segment;
        out.bus = info.bus;
        out.device = info.device;
        out.function = info.function;
        out.requester_id = info.requester_id;
    }

    VmDeviceAssignment* a = device_alloc(v, flags, &rc);
    if (!a) {
        return rc;
    }
    // Policy input for the attach machine: authorise the VM's mapped RAM as the
    // appliance DMA aperture before the domain is populated (§8.2, §8.5).
    rc = collect_vm_ram(a);
    if (rc == E_OK) rc = device_attach(a, spec);
    if (rc != E_OK) {
        if (a->state != VM_DEV_QUARANTINED) object::release(a->lifetime);
        return rc;
    }
    const CapHandle cap_h =
        cap::alloc(*caller.cap_table, CAP_VM_DEVICE, a,
                   RIGHT_VM_CONTROL | RIGHT_VM_MAP | RIGHT_VM_INSPECT | RIGHT_DUP);
    if (cap_h == kNullCap) {
        // Only drop the body on a clean revoke; a quarantined assignment keeps
        // its domain (and pinned pages) alive for a later provable teardown.
        if (device_revoke(a) == E_OK) {
            object::release(a->lifetime);
        }
        return E_OVERFLOW;
    }
    *out_cap = cap_h;
    return E_OK;
}

SysError device_map_bar_internal(VmDeviceAssignment* a, u32 bar_index, u32 bar_offset, u64 size,
                                 u64 guest_ipa, u32 rights) {
    if (!a || !a->owner_vm || bar_index >= pci::kMaxBars || a->fn_count == 0) {
        return E_INVALID;
    }
    if (!a->active || a->state != VM_DEV_ACTIVE || a->ecam_size) {
        ANKO_LOGF_INFO("kernel/vm", "assign: map_bar busy active=%u state=%u ecam=0x%lx",
                       a->active ? 1u : 0u, static_cast<unsigned>(a->state),
                       static_cast<unsigned long>(a->ecam_size));
        return E_BUSY;
    }
    if (size == 0 || (size & (PAGE_SIZE - 1)) != 0 || (guest_ipa & (PAGE_SIZE - 1)) != 0) {
        return E_INVALID;
    }
    if ((rights & ~(RIGHT_READ | RIGHT_WRITE)) != 0 || rights == 0) {
        return E_INVALID;
    }
    if (a->mmio_count >= kMaxAssignMmio) {
        return E_OVERFLOW;
    }
    const pci::PciFunctionInfo& fn = a->fns[0].info;
    const pci::PciBar& bar = fn.bars[bar_index];
    if (bar.size == 0 || bar.base == 0) {
        return E_NOTFOUND;
    }
    if ((bar.type != pci::BarType::Mmio32 && bar.type != pci::BarType::Mmio64) ||
        bar_offset >= bar.size || size > bar.size - bar_offset ||
        bar.base > ~0ull - bar.size) {
        return E_INVALID;
    }
    const PhysAddr host_base = bar.base + bar_offset;
    if ((host_base & (PAGE_SIZE - 1)) != 0) {
        return E_INVALID; // BAR subrange must be page aligned to be guest-mappable
    }
    // A published BAR (the whole aperture) must land at its own alignment: the
    // synthetic record and the manager both assume it, so enforce it here too.
    if (bar_offset == 0 && size == bar.size && (guest_ipa & (bar.size - 1)) != 0) {
        return E_INVALID;
    }
    if (guest_ipa < kMinDeviceGuestIpa || guest_ipa >= (1ull << a->owner_vm->stage2.ipa_bits) ||
        size > (1ull << a->owner_vm->stage2.ipa_bits) - guest_ipa) {
        return E_INVALID;
    }
    const SysError overlap = stage2_validate_no_overlap(&a->owner_vm->stage2, guest_ipa, size);
    if (overlap != E_OK) {
        return overlap;
    }
    u8 host[pci::kCfgSpaceBytes];
    for (u32 offset = 0; offset < sizeof(host); ++offset)
        host[offset] = pci::cfg_read_byte(fn.bus, fn.device, fn.function, offset);
    if (!pci::bar_range_safe(host, fn.bars, bar_index, bar_offset, size)) return E_PERM;
    // Publish a BAR only when its complete aperture is mapped. Commit the
    // synthetic record after all stage-2 mappings succeed.
    auto config = a->cfg;
    if (bar_offset == 0 && size == bar.size &&
        !pci::config_set_bar(config, 0, bar_index, guest_ipa, bar)) return E_INVALID;
    const u64 pages = size / PAGE_SIZE;
    u64 i = 0;
    while (i < pages) {
        const u64 ipa = guest_ipa + i * PAGE_SIZE;
        const PhysAddr pa = host_base + i * PAGE_SIZE;
        // Uniform aligned runs use blocks. Only geometry selects the page
        // path; a failed block map is a real error and must unwind the range.
        const bool block = ipa % kStage2BlockSize == 0 && pa % kStage2BlockSize == 0 &&
                           pages - i >= kStage2BlockPages;
        const SysError rc =
            block ? stage2_map_block(&a->owner_vm->stage2, ipa, pa, rights, Stage2MemType::Device)
                  : map_one_bar_page(a, ipa, pa, rights);
        if (rc != E_OK) {
            if (i) {
                const SysError undo =
                    stage2_unmap_range(&a->owner_vm->stage2, guest_ipa, i * PAGE_SIZE);
                ANKO_ASSERT(undo == E_OK);
            }
            return rc;
        }
        i += block ? kStage2BlockPages : 1;
    }
    VmAssignMmio& m = a->mmio[a->mmio_count++];
    libk::memset(&m, 0, sizeof(m));
    m.host_base = host_base;
    m.size = size;
    m.guest_ipa = guest_ipa;
    m.rights = rights;
    m.fn_index = 0;
    m.bar_index = bar_index;
    m.bar_offset = bar_offset;
    m.mapped = true;
    a->cfg = config;
    metric_inc(VM_DEBUG_VM_DEVICE_BAR_MAPPED);
    ANKO_LOGF_INFO("kernel/vm", "assign: bar%u mapped ipa=0x%llx host=0x%llx size=0x%llx",
                   bar_index, static_cast<unsigned long long>(guest_ipa),
                   static_cast<unsigned long long>(host_base),
                   static_cast<unsigned long long>(size));
    return E_OK;
}

SysError device_map_bar(Process& caller, CapHandle assign_cap, u32 bar_index, u32 bar_offset,
                        u64 size, u64 guest_ipa, u32 rights) {
    SysError rc = E_OK;
    auto a = assignment_from_cap(caller, assign_cap, RIGHT_VM_MAP, &rc);
    if (!a) {
        return rc;
    }
    return device_map_bar_internal(a, bar_index, bar_offset, size, guest_ipa, rights);
}

SysError device_add_route(VmDeviceAssignment* a, u32 fn_index, u32 host_irq, u32 guest_irq,
                          u32 trigger, Vcpu* target, bool msi) {
    if (!a || a->irq_count >= kMaxAssignIrqs) {
        return E_OVERFLOW;
    }
    VmAssignIrq& route = a->irq[a->irq_count];
    libk::memset(&route, 0, sizeof(route));
    route.host_irq = host_irq;
    route.guest_irq = guest_irq;
    route.trigger = trigger;
    route.fn_index = fn_index;
    route.vcpu_id = target ? target->vcpu_id : 0;
    route.target = target;
    route.msi = msi;
    ++a->irq_count;
    metric_inc(VM_DEBUG_VM_DEVICE_IRQ_ROUTES);
    // Install immediately when the machine is already past the route step.
    if (a->state >= VM_DEV_EXPOSED) {
        return device_irq_install(a, a->irq_count - 1);
    }
    return E_OK;
}

SysError device_route_irq(Process& caller, CapHandle assign_cap, CapHandle irq_cap,
                          CapHandle vcpu_cap, u32 guest_irq, u32 flags) {
    if (flags != 0) {
        return E_INVALID;
    }
    if (guest_irq < irq::kFirstSpi || guest_irq >= kVgicMaxIrqs) {
        return E_INVALID; // devices may target SPIs, never guest SGIs or timer PPIs
    }
    SysError rc = E_OK;
    auto a = assignment_from_cap(caller, assign_cap, RIGHT_VM_MAP, &rc);
    if (!a) {
        return rc;
    }
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) {
        return rc;
    }
    if (a->owner_vm && a->owner_vm->vm_id != vcpu->owner_vm_id) {
        return E_PERM; // route target must belong to the assignment's VM
    }
    cap::Ref irq_e = require_cap(caller, irq_cap, CAP_IRQ, RIGHT_READ, &rc);
    if (!irq_e) {
        return rc;
    }
    auto* line = static_cast<IrqLine*>(irq_e->target);
    if (!line) {
        return E_NOTFOUND;
    }
    return device_add_route(a, 0, line->irq_id, guest_irq, line->trigger, vcpu, false);
}

// ECAM is a trap-only aperture. CallGuard serializes construction, accesses and teardown.
SysError device_expose_ecam(Process& caller, CapHandle assign_cap, u64 ipa, u64 size) {
    SysError rc = E_OK;
    auto a = assignment_from_cap(caller, assign_cap, RIGHT_VM_CONTROL, &rc);
    if (!a) return rc;
    return device_expose_ecam_internal(a.get(), ipa, size);
}

SysError device_expose_ecam_internal(VmDeviceAssignment* a, u64 ipa, u64 size) {
    if (!a || !a->owner_vm || !a->active || a->state != VM_DEV_ACTIVE) return E_DEAD;
    if (a->ecam_size || size != pci::kGuestEcamSize || (ipa & (size - 1)) ||
        ipa < kMinDeviceGuestIpa || ipa >= (1ull << a->owner_vm->stage2.ipa_bits) ||
        size > (1ull << a->owner_vm->stage2.ipa_bits) - ipa)
        return E_INVALID;
    auto config = a->cfg;
    // The bootstrap transport is one synthetic device, with contiguous functions.
    for (u32 i = 0; i < a->fn_count; ++i) {
        if (a->fns[i].info.function != i) return E_NOTIMPL;
        u8 host[pci::kCfgSpaceBytes];
        const auto& fn = a->fns[i].info;
        for (u32 offset = 0; offset < sizeof(host); ++offset)
            host[offset] = pci::cfg_read_byte(fn.bus, fn.device, fn.function, offset);
        if (!pci::config_filter_virtio_caps(config, i, host)) return E_INVALID;
    }
    const auto rc = stage2_reserve_trap(&a->owner_vm->stage2, ipa, size);
    if (rc != E_OK) return rc;
    a->cfg = config;
    a->ecam_ipa = ipa;
    a->ecam_size = size;
    return E_OK;
}

bool device_emulate_ecam(Vm& owner, u64 ipa, u32 size, bool write, u64* value) {
    if (!value) return false;
    for (auto& a : g_pool.slots) {
        if (!a.lifetime || a.owner_vm != &owner || !a.ecam_size ||
            ipa < a.ecam_ipa || ipa - a.ecam_ipa >= a.ecam_size) continue;
        pci::EcamAddress address{};
        if (!pci::decode_ecam(ipa - a.ecam_ipa, size, &address)) return false;
        if (!a.active || a.state != VM_DEV_ACTIVE || !address.assigned_device ||
            address.function >= a.fn_count) {
            if (!write) *value = 0xffffffffu;
            return true;
        }
        if (write) {
            // PCI writes rejected by policy complete without changing host state.
            (void)device_mediate_config_write(&a, address.function, address.offset, *value, size);
        } else {
            *value = pci::config_read(a.cfg, address.function, address.offset, size);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// §8.12 DMA-safe revoke
// ---------------------------------------------------------------------------

static SysError device_revoke_impl(VmDeviceAssignment* a, bool faulting) {
    if (!a) {
        return E_INVALID;
    }
    if (a->state == VM_DEV_REVOKED) {
        return E_OK;
    }
    a->active = false;
    a->state = VM_DEV_REVOKING;
    a->revoke_step = 1; // STOP NEW GUEST REQUESTS

    // FREEZE/STOP APPLIANCE VCPUS AS REQUIRED: route delivery is masked below,
    // so the guest cannot observe new device interrupts while teardown runs.
    a->revoke_step = 2;

    // MASK HOST INTERRUPTS: remove every route so nothing can re-enter the vGIC.
    for (u32 i = 0; i < a->irq_count; ++i) {
        device_irq_uninstall(a, i);
    }
    a->revoke_step = 3;

    // CLEAR HOST BUS MASTER before any translation changes; a device that was
    // mastering must be quiesced first.
    for (u32 i = 0; i < a->fn_count; ++i) {
        const pci::PciFunctionInfo& fn = a->fns[i].info;
        pci::set_bus_master(fn.bus, fn.device, fn.function, false);
    }
    a->revoke_step = 4;
    ANKO_LOGF_INFO("kernel/vm", "revoke: bm cleared device=%u fns=%u", a->device_id, a->fn_count);

    // DETACH REQUESTER/STREAM IDs: each requester is repointed at the faulting
    // (quarantine) context, never a bypass.
    bool detach_failed = false;
    for (u32 i = 0; i < a->fn_count; ++i) {
        const pci::PciFunctionInfo& fn = a->fns[i].info;
        if (!a->dma_domain) {
            break;
        }
        const SysError rc = iommu::domain_detach(
            *a->dma_domain, iommu::IommuDeviceId{static_cast<u32>(fn.segment), fn.requester_id});
        if (rc != E_OK) {
            detach_failed = true;
        }
    }
    a->revoke_step = 5;

    // INVALIDATE + SYNC.
    if (a->dma_domain) {
        (void)iommu::domain_sync(*a->dma_domain);
    }
    a->revoke_step = 6;

    // REMOVE IOMMU MAPPINGS, then INVALIDATE + SYNC AGAIN. unmap_batch only
    // releases a page after the backend proves the range invalidated; a failure
    // quarantines the domain and the pages stay pinned (§8.12).
    // Appliance death (unclean): we cannot prove the requester stopped, so the
    // domain is quarantined below (pages pinned, never reused) even if the
    // unmap itself reported success. This is the "detach into a faulting domain
    // before any page is returned" ordering of §8.12.
    bool quarantine = detach_failed || (faulting && a->dma_domain != nullptr);
    if (a->dma_domain) {
        for (u32 i = 0; i < a->ram_count; ++i) {
            VmAssignRam& r = a->ram[i];
            if (!r.mapped) {
                continue;
            }
            iommu::IommuMap map{};
            map.iova = r.iova;
            map.pa = r.base;
            map.length = r.size;
            map.perms =
                static_cast<u32>(iommu::DmaPerm::Read) | static_cast<u32>(iommu::DmaPerm::Write);
            const SysError rc = iommu::domain_unmap_batch(*a->dma_domain, &map, 1);
            if (rc != E_OK) {
                quarantine = true;
                break;
            }
            r.mapped = false;
        }
        (void)iommu::domain_sync(*a->dma_domain);
    }
    a->revoke_step = 7;

    // Remove the guest BAR mappings from stage-2 (Device memory).
    if (a->owner_vm) {
        for (u32 i = 0; i < a->mmio_count; ++i) {
            VmAssignMmio& m = a->mmio[i];
            if (!m.mapped) {
                continue;
            }
            const SysError rc =
                stage2_revoke_device_range(&a->owner_vm->stage2, m.guest_ipa, m.size);
            if (rc != E_OK) {
                quarantine = true;
            } else {
                m.mapped = false;
            }
        }
    }

    if (quarantine) {
        a->state = VM_DEV_QUARANTINED;
        metric_inc(VM_DEBUG_VM_DEVICE_QUARANTINE);
        ANKO_LOGF_WARN("kernel/vm", "revoke: quarantined device=%u pages pinned=%u", a->device_id,
                       a->dma_domain ? iommu::domain_pinned_pages(*a->dma_domain) : 0);
        return E_FAULT; // keep the assignment (and the pinned pages) alive
    }

    if (a->dma_domain) {
        // Every translation is invalidated and synchronised; destroy now frees
        // the pinned page tables.
        if (iommu::domain_destroy(a->dma_domain) != E_OK) {
            a->state = VM_DEV_QUARANTINED;
            metric_inc(VM_DEBUG_VM_DEVICE_QUARANTINE);
            return E_FAULT;
        }
        a->dma_domain = nullptr;
    }
    a->state = VM_DEV_REVOKED;
    metric_inc(VM_DEBUG_VM_DEVICE_DESTROYED);
    ANKO_LOGF_INFO("kernel/vm", "revoke: complete device=%u", a->device_id);
    return E_OK;
}

SysError device_revoke(VmDeviceAssignment* a) {
    return device_revoke_impl(a, /*faulting=*/false);
}

SysError device_revoke_faulting(VmDeviceAssignment* a) {
    return device_revoke_impl(a, /*faulting=*/true);
}

SysError device_destroy(Process& caller, CapHandle assign_cap) {
    SysError rc = E_OK;
    auto a = assignment_from_cap(caller, assign_cap, RIGHT_VM_CONTROL, &rc);
    if (!a) {
        return rc;
    }
    VmDeviceAssignment* raw = a.get();
    const SysError rc2 = device_revoke(raw);
    // Drop the caller's capability and the assignment's own reference. A
    // quarantined assignment keeps its pages pinned and is not freed.
    sched::sweep_process_caps(raw);
    if (rc2 == E_OK) {
        release_body_once(raw);
    }
    return rc2;
}

void device_release_for_vm(Vm* v) {
    if (!v) {
        return;
    }
    for (u32 i = 0; i < kMaxVmDevices; ++i) {
        VmDeviceAssignment* a = &g_pool.slots[i];
        if (!a->lifetime || a->owner_vm != v) {
            continue;
        }
        // VM kill/retire is unclean: force the faulting (quarantine) teardown.
        // Only a clean revoke drops the creator reference; a quarantined body
        // keeps the domain (and its pinned pages) alive exactly as a failed
        // §8.12 revoke requires.
        if (device_revoke_faulting(a) == E_OK) {
            release_body_once(a);
        }
    }
}

void device_owner_died(u64 identity) {
    if (identity == 0) {
        return;
    }
    for (u32 i = 0; i < kMaxVmDevices; ++i) {
        VmDeviceAssignment* a = &g_pool.slots[i];
        if (!a->lifetime || a->owner_identity != identity) {
            continue;
        }
        // Owner-process death is unclean: quarantine rather than reuse pages.
        if (device_revoke_faulting(a) == E_OK) {
            release_body_once(a);
        }
    }
}

} // namespace vm
