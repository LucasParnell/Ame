// kernel/vm/vm_device_selftest.cc — opt-in Phase D proof (spec §6, §7, §8.11,
// §8.12, §8.14).
//
// Runs only in a dev build, with the SMMUv3 up and a QEMU `iommu-testdev`
// present (the same topology the Phase C hostile-DMA test uses). It drives the
// real assignment object and its state machines directly, using the iommu-testdev
// little register file to issue DMA, and proves:
//
//   (i)   the host function is NOT bus-mastering when the secure-attach machine
//         completes: a forced BME bit is cleared and never re-enabled without a
//         mediated guest write (§8.11);
//   (ii)  after a mediated guest BME write, bus-master is enabled and DMA
//         through the assignment's IOMMU domain succeeds for a mapped IOVA while
//         an unmapped IOVA faults (§8.14 1-2);
//   (iii) at revoke, host bus-master is cleared and DMA is rejected before any
//         page is released, with the canary page intact (§8.12);
//   (iv)  a guest MSI/MSI-X programming request produces a vGIC pending
//         interrupt, and a level route suppresses-after-inject and re-arms on
//         guest EOI (§7.3, §7.4).
//
// The spec's "a Linux PCI test guest enumerates only the assigned function" is
// validated in Phase E: a Linux source tree is external and not available here,
// so it is deliberately NOT faked.

#include "anko/vm.h"

#ifdef ANKO_BUILD_DEV

#include "anko/hw_info.h"
#include "anko/log.h"
#include "anko/mm.h"
#include "anko/pci.h"
#include "anko/smmuv3.h"
#include "libk/string.h"

namespace vm {

namespace {

// --- QEMU iommu-testdev identity and register file (see Phase C) ------------
constexpr u16 kTestdevVendor = 0x1b36;
constexpr u16 kTestdevDevice = 0x0005;
constexpr u64 kTestdevBarBase = 0x1010'0000ull;
constexpr u64 kTestdevBarSize = 0x1000;

constexpr u32 kRegTrigger = 0x00;
constexpr u32 kRegGvaLo = 0x04;
constexpr u32 kRegGvaHi = 0x08;
constexpr u32 kRegLen = 0x0C;
constexpr u32 kRegResult = 0x10;
constexpr u32 kRegDbell = 0x14;
constexpr u32 kRegAttrs = 0x18;
constexpr u32 kRegGpaLo = 0x1C;
constexpr u32 kRegGpaHi = 0x20;

constexpr u32 kDbellArm = 0x1u;
constexpr u32 kResultOk = 0x0u;
constexpr u32 kResultErrTxFail = 0xDEAD'0002u;
constexpr u32 kWriteValue = 0x1234'5678u;

constexpr u32 kCfgCommand = 0x04;
constexpr u32 kCfgBar0 = 0x10;
constexpr u32 kCmdMemoryEnable = 1u << 1;

constexpr u64 kIovaMapped = 0x8000'0000ull;

constexpr u32 kCanaryWord = 0xA5A5'A5A5u;
constexpr u32 kPoisonWord = 0xBEEF'FACEu;

// Spare SPIs for the route tests (both wired to nothing on the boards).
constexpr u32 kMsiHostIrq = 99;
constexpr u32 kLevelHostIrq = 98;
constexpr u32 kMsiVector = 42;
constexpr u32 kLevelVector = 45;

// A synthetic appliance vCPU for the vGIC delivery proof. Static so it lands in
// .bss; the assignment route only needs its VgicState and identity.
Vcpu g_vcpu;

int g_checks;
int g_failures;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        ANKO_LOGF_ERROR("vm/selftest", "  check failed: %s", what);
    }
}

// --- ECAM / testdev MMIO ----------------------------------------------------
volatile u32* ecam_reg(u32 bus, u32 dev, u32 fn, u32 off) {
    const u64 addr = g_hw.ecam_base | (static_cast<u64>(bus & 0xFFu) << 20) |
                     (static_cast<u64>(dev & 0x1Fu) << 15) | (static_cast<u64>(fn & 0x7u) << 12) |
                     (off & 0xFFFu);
    return reinterpret_cast<volatile u32*>(addr);
}

u32 cfg_read(u32 bus, u32 dev, u32 fn, u32 off) {
    asm volatile("dsb sy" ::: "memory");
    return *ecam_reg(bus, dev, fn, off);
}

void cfg_write(u32 bus, u32 dev, u32 fn, u32 off, u32 value) {
    *ecam_reg(bus, dev, fn, off) = value;
    asm volatile("dsb sy" ::: "memory");
}

u32 td_read(u32 off) {
    asm volatile("dsb sy" ::: "memory");
    const u32 value = *reinterpret_cast<volatile u32*>(kTestdevBarBase + off);
    asm volatile("dsb sy" ::: "memory");
    return value;
}

void td_write(u32 off, u32 value) {
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile u32*>(kTestdevBarBase + off) = value;
    asm volatile("dsb sy" ::: "memory");
}

u32 td_dma(u64 gva, u64 gpa, u32 len) {
    (void)td_read(kRegResult);
    td_write(kRegGvaLo, static_cast<u32>(gva));
    td_write(kRegGvaHi, static_cast<u32>(gva >> 32));
    td_write(kRegGpaLo, static_cast<u32>(gpa));
    td_write(kRegGpaHi, static_cast<u32>(gpa >> 32));
    td_write(kRegLen, len);
    td_write(kRegAttrs, 0);
    td_write(kRegDbell, kDbellArm);
    (void)td_read(kRegTrigger);
    return td_read(kRegResult);
}

void cache_clean(PhysAddr pa, u64 len) {
    for (PhysAddr p = pa & ~63ull; p < pa + len; p += 64) {
        asm volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

void cache_invalidate(PhysAddr pa, u64 len) {
    for (PhysAddr p = pa & ~63ull; p < pa + len; p += 64) {
        asm volatile("dc ivac, %0" ::"r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}

u32 peek32(PhysAddr pa) {
    cache_invalidate(pa, 4);
    return *reinterpret_cast<volatile u32*>(pa);
}

bool find_testdev(pci::PciFunctionInfo* out) {
    static pci::PciFunctionInfo functions[pci::kMaxFunctions];
    const u32 count = pci::enumerate(g_hw.ecam_base, functions, pci::kMaxFunctions);
    for (u32 i = 0; i < count; ++i) {
        if (functions[i].vendor_id == kTestdevVendor && functions[i].device_id == kTestdevDevice) {
            *out = functions[i];
            return true;
        }
    }
    return false;
}

// Build a DeviceAssignSpec from a single function's isolation group.
bool build_spec(const pci::PciFunctionInfo& fn, DeviceAssignSpec* out) {
    pci::PciIsolationGroup group{};
    if (!pci::isolation_group_of(g_hw.ecam_base, fn.bus, fn.device, &group)) {
        return false;
    }
    libk::memset(out, 0, sizeof(*out));
    out->group_id = group.group_id;
    for (u32 f = 0; f < 32; ++f) {
        if ((group.function_mask & (1u << f)) == 0) {
            continue;
        }
        pci::PciFunctionInfo info{};
        if (!pci::find_function(g_hw.ecam_base, group.bus, group.device, static_cast<u8>(f),
                                &info)) {
            return false;
        }
        DeviceAssignSpec::Fn& slot = out->fns[out->fn_count++];
        slot.segment = info.segment;
        slot.bus = info.bus;
        slot.device = info.device;
        slot.function = info.function;
        slot.requester_id = info.requester_id;
    }
    return out->fn_count > 0;
}

} // namespace

void vm_device_selftest_if_testdev() {
    if (!iommu::smmuv3_ready() || !g_hw.has_pcie || g_hw.ecam_base == 0) {
        return;
    }
    pci::PciFunctionInfo fn{};
    if (!find_testdev(&fn)) {
        return; // no test device in this topology
    }
    // Assign BAR0 into the PCI MMIO aperture (the direct -kernel boot has no
    // firmware to do it) and enable memory decode, so the inventory record has a
    // real BAR to assign into the guest.
    const u32 bar_orig = cfg_read(fn.bus, fn.device, fn.function, kCfgBar0);
    cfg_write(fn.bus, fn.device, fn.function, kCfgBar0,
              static_cast<u32>(kTestdevBarBase) | (bar_orig & 0xFu));
    cfg_write(fn.bus, fn.device, fn.function, kCfgCommand,
              cfg_read(fn.bus, fn.device, fn.function, kCfgCommand) | kCmdMemoryEnable);
    if (!mm::map_kernel_device_mmio(kTestdevBarBase, kTestdevBarSize)) {
        // Phase C already mapped it in this topology; that is fine.
    }

    g_checks = 0;
    g_failures = 0;

    // Synthetic owner VM: heap-allocated because Vm carries the full stage-2
    // table image. Dev-only and one-shot, so the bump allocation is acceptable.
    auto* owner = static_cast<Vm*>(mm::heap_alloc(sizeof(Vm), alignof(Vm)));
    if (!owner) {
        ANKO_LOG_ERROR("vm/selftest", "TEST FAILED: no memory for synthetic VM");
        return;
    }
    libk::memset(owner, 0, sizeof(*owner));
    owner->vm_id = 0x7F00;
    owner->state = VM_CREATED;
    if (stage2_create(&owner->stage2) != E_OK) {
        ANKO_LOG_ERROR("vm/selftest", "TEST FAILED: stage2_create");
        return;
    }

    DeviceAssignSpec spec{};
    if (!build_spec(fn, &spec)) {
        ANKO_LOG_ERROR("vm/selftest", "TEST FAILED: isolation group");
        return;
    }

    SysError rc = E_OK;
    VmDeviceAssignment* a = device_alloc(owner, 0, &rc);
    if (!a) {
        ANKO_LOG_ERROR("vm/selftest", "TEST FAILED: device_alloc");
        return;
    }

    // Two contiguous pages: the DMA target (authorised) and a canary (never
    // authorised). A second pair holds the "unmapped" IOVA target.
    const PhysAddr pages = mm::alloc_pages_contiguous(4);
    if (!pages) {
        ANKO_LOG_ERROR("vm/selftest", "TEST FAILED: no test pages");
        return;
    }
    const PhysAddr target = pages;
    const PhysAddr canary = pages + PAGE_SIZE;
    const PhysAddr unmapped_target = pages + 2 * PAGE_SIZE;
    const PhysAddr spare = pages + 3 * PAGE_SIZE;
    for (u64 i = 0; i < (4u * PAGE_SIZE) / 4u; ++i) {
        reinterpret_cast<volatile u32*>(pages)[i] = kCanaryWord;
    }
    cache_clean(pages, 4u * PAGE_SIZE);

    // Authorise only the target page; the canary page is deliberately not in the
    // domain so an identity/bypass translation would corrupt it.
    check(device_authorize_ram(a, target, PAGE_SIZE, kIovaMapped) == E_OK, "authorize target");
    (void)unmapped_target;
    (void)spare;

    // (i) Force bus-master on, then run the secure-attach machine and prove the
    // contained step cleared it and nothing re-enabled it.
    pci::set_bus_master(fn.bus, fn.device, fn.function, true);
    check((pci::read_command(fn.bus, fn.device, fn.function) & pci::kPciCmdBusMaster) != 0,
          "(i) forced BME visible before attach");

    rc = device_attach(a, spec);
    check(rc == E_OK, "(i) secure attach succeeded");
    if (rc != E_OK) {
        ANKO_LOGF_ERROR("vm/selftest", "TEST FAILED: attach rc=%d", static_cast<int>(rc));
        return;
    }
    check((pci::read_command(fn.bus, fn.device, fn.function) & pci::kPciCmdBusMaster) == 0,
          "(i) host not bus-mastering after attach");
    ANKO_LOGF_INFO("vm/selftest", "(i) contained bm=0 state=%s dma_mapped_pages=%u",
                   device_state_name(a->state),
                   a->dma_domain ? iommu::domain_pinned_pages(*a->dma_domain) : 0);

    // BAR stage-2 Device mapping: map BAR0's first page into the guest.
    const u32 bar_lo = cfg_read(fn.bus, fn.device, fn.function, kCfgBar0) & ~0xFull;
    if (bar_lo != 0) {
        const u64 gipa = kGuestPhysMin;
        const SysError mrc =
            device_map_bar_internal(a, 0, 0, PAGE_SIZE, gipa, RIGHT_READ | RIGHT_WRITE);
        check(mrc == E_OK, "(ii) BAR stage-2 Device map");
        Stage2MemType mt = Stage2MemType::NormalWB;
        u32 rights = 0;
        const PhysAddr got = stage2_lookup_page(&owner->stage2, gipa, &rights, &mt);
        check(got == static_cast<PhysAddr>(bar_lo), "(ii) BAR map points at host BAR");
        check(mt == Stage2MemType::Device, "(ii) BAR map is Device memory");
        ANKO_LOGF_INFO("vm/selftest", "(ii) bar mapped ipa=0x%llx host=0x%llx device=%u",
                       static_cast<unsigned long long>(gipa), static_cast<unsigned long long>(got),
                       mt == Stage2MemType::Device ? 1u : 0u);
    }

    // (ii) Mediated guest BME write (memory decode + bus master), then DMA.
    a->cfg.present[0] = true; // expose already set this; keep it explicit
    SysError mrc = device_mediate_config_write(a, 0, kCfgCommand, 0x0006u, 4);
    check(mrc == E_OK, "(ii) mediated command write");
    check((pci::read_command(fn.bus, fn.device, fn.function) & pci::kPciCmdBusMaster) != 0,
          "(ii) host bus-master enabled after mediated write");
    {
        const u64 faults0 = iommu::debug_counter(iommu::IOMMU_FAULTS);
        const u32 result = td_dma(kIovaMapped, target, 4);
        check(result == kResultOk, "(ii) mapped DMA succeeded");
        check(peek32(target) == kWriteValue, "(ii) mapped DMA reached the backing page");
        check(iommu::debug_counter(iommu::IOMMU_FAULTS) == faults0, "(ii) mapped DMA no fault");
        ANKO_LOGF_INFO("vm/selftest", "(ii) mapped DMA ok result=0x%x", result);
    }
    {
        const u64 faults0 = iommu::debug_counter(iommu::IOMMU_FAULTS);
        const u32 result = td_dma(canary, canary, 4);
        (void)iommu::smmuv3_drain_events();
        check(result == kResultErrTxFail, "(ii) unmapped IOVA faulted");
        check(peek32(canary) == kCanaryWord, "(ii) canary page unchanged");
        check(iommu::debug_counter(iommu::IOMMU_FAULTS) > faults0, "(ii) fault counted");
        ANKO_LOGF_INFO("vm/selftest", "(ii) unmapped fault ok result=0x%x canary=0x%x", result,
                       peek32(canary));
    }

    // (iv) MSI/MSI-X virtualization into a vGIC pending bit, plus level re-arm.
    libk::memset(&g_vcpu, 0, sizeof(g_vcpu));
    g_vcpu.vcpu_id = 1;
    g_vcpu.owner_vm_id = owner->vm_id;
    // MSI route: reserve a host line, then let the guest MSI write pick the vector.
    check(device_add_route(a, 0, kMsiHostIrq, 0, /*trigger=edge*/ 1, &g_vcpu, /*msi=*/true) == E_OK,
          "(iv) reserve MSI route");
    a->msi[0].present = true;
    a->msi[0].msix = false;
    a->msi[0].cap_offset = 0x40;
    a->cfg.msi_cap_off[0] = 0x40;
    // Enable MSI (Message Control bit 0), then program the guest vector.
    check(device_mediate_config_write(a, 0, 0x40 + 2, 0x0001u, 2) == E_OK, "(iv) MSI enable write");
    check(device_mediate_config_write(a, 0, 0x40 + 12, kMsiVector, 2) == E_OK,
          "(iv) MSI data write");
    device_irq_deliver(kMsiHostIrq);
    check(vgic_pending_test(g_vcpu, kMsiVector), "(iv) MSI produced a vGIC pending bit");
    ANKO_LOGF_INFO("vm/selftest", "(iv) msi route injected vector=%u pending=%u", kMsiVector,
                   vgic_pending_test(g_vcpu, kMsiVector) ? 1u : 0u);

    // Level route: suppress-after-inject, re-arm on guest EOI (§7.4).
    check(device_add_route(a, 0, kLevelHostIrq, kLevelVector, /*trigger=level*/ 0, &g_vcpu,
                           false) == E_OK,
          "(iv) add level route");
    device_irq_deliver(kLevelHostIrq);
    const bool level_first = vgic_pending_test(g_vcpu, kLevelVector);
    device_irq_deliver(kLevelHostIrq); // suppressed: no new delivery
    u64 level_deliveries = 0;
    for (u32 i = 0; i < a->irq_count; ++i) {
        if (a->irq[i].host_irq == kLevelHostIrq) {
            level_deliveries = a->irq[i].deliveries;
        }
    }
    check(level_first, "(iv) level route injected");
    check(level_deliveries == 1, "(iv) level route suppressed after inject");
    device_irq_on_guest_eoi(g_vcpu, kLevelVector);
    device_irq_deliver(kLevelHostIrq);
    u64 level_deliveries2 = 0;
    for (u32 i = 0; i < a->irq_count; ++i) {
        if (a->irq[i].host_irq == kLevelHostIrq) {
            level_deliveries2 = a->irq[i].deliveries;
        }
    }
    check(level_deliveries2 == 2, "(iv) level route re-armed after guest EOI");
    ANKO_LOGF_INFO("vm/selftest", "(iv) level route ok deliveries=%llu->%llu",
                   static_cast<unsigned long long>(level_deliveries),
                   static_cast<unsigned long long>(level_deliveries2));

    // Mediation rejections: a BAR write and a ROM/power write must never reach
    // the host or the synthetic image.
    const u64 deny0 = debug_counter(VM_DEBUG_VM_DEVICE_MEDIATED_DENY);
    check(device_mediate_config_write(a, 0, pci::kRegBar0, 0x1234u, 4) == E_PERM,
          "(iv) BAR write rejected");
    check(device_mediate_config_write(a, 0, pci::kRegRomBar, 0x1u, 4) == E_PERM,
          "(iv) ROM write rejected");
    check(debug_counter(VM_DEBUG_VM_DEVICE_MEDIATED_DENY) >= deny0 + 2, "(iv) mediations counted");

    // (iii) DMA-safe revoke: BME cleared, DMA rejected, canary intact.
    *reinterpret_cast<volatile u32*>(target) = kPoisonWord;
    cache_clean(target, 4);
    check(device_revoke(a) == E_OK, "(iii) revoke completed");
    check((pci::read_command(fn.bus, fn.device, fn.function) & pci::kPciCmdBusMaster) == 0,
          "(iii) host bus-master cleared");
    const u64 faults0 = iommu::debug_counter(iommu::IOMMU_FAULTS);
    const u32 result = td_dma(kIovaMapped, target, 4);
    (void)iommu::smmuv3_drain_events();
    check(result == kResultErrTxFail, "(iii) revoked DMA rejected");
    check(iommu::debug_counter(iommu::IOMMU_FAULTS) > faults0, "(iii) revocation fault counted");
    check(peek32(target) == kPoisonWord, "(iii) target page intact after revoke");
    check(peek32(canary) == kCanaryWord, "(iii) canary page intact after revoke");
    ANKO_LOGF_INFO("vm/selftest", "(iii) revoke bm=0 dma=rejected canary=0x%x target=0x%x state=%s",
                   peek32(canary), peek32(target), device_state_name(a->state));

    // Release the revoked assignment body. A clean revoke leaves it in REVOKED;
    // a quarantined one keeps its pages pinned and is intentionally not freed.
    if (a->state == VM_DEV_REVOKED) {
        object::release(a->lifetime);
    }

    ANKO_LOGF_INFO("vm/selftest",
                   "counters assign_created=%llu active=%llu bar=%llu routes=%llu injected=%llu "
                   "deny=%llu bme=%llu iommu_faults=%llu",
                   static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_CREATED)),
                   static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_ACTIVE)),
                   static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_BAR_MAPPED)),
                   static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_IRQ_ROUTES)),
                   static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_IRQ_INJECTED)),
                   static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_MEDIATED_DENY)),
                   static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_BME_ENABLE)),
                   static_cast<unsigned long long>(iommu::debug_counter(iommu::IOMMU_FAULTS)));

    if (g_failures == 0) {
        ANKO_LOGF_INFO("vm/selftest", "device assign test passed (%d checks)", g_checks);
    } else {
        ANKO_LOGF_ERROR("vm/selftest", "TEST FAILED: %d/%d checks failed", g_failures, g_checks);
    }
}

} // namespace vm

#else // !ANKO_BUILD_DEV

namespace vm {
void vm_device_selftest_if_testdev() {}
} // namespace vm

#endif
