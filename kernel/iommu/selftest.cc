// kernel/iommu/selftest.cc — opt-in hostile-DMA verification (Phase C).
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.14 (verification strategy),
// §8.7 (QEMU backend).
//
// Under QEMU's `iommu-testdev` this proves, with real SMMUv3 translations and
// `default-bus-bypass-iommu=off`, that:
//
//   (a) DMA to a mapped appliance IOVA succeeds;
//   (b) DMA to an unmapped IOVA faults, IOMMU_FAULTS advances, and the canary
//       page an identity/bypass translation would have hit is unchanged;
//   (c) a read-only mapping rejects a device write;
//   (d) a requester detached from its domain faults rather than bypassing;
//   (e) repeated faults cannot overflow a queue and quarantine the stream.
//
// The device is driven straight from its BAR by the kernel: BAR0 is assigned
// into the QEMU virt PCI MMIO aperture, the 4 KiB window is mapped into the
// kernel address space, and the kernel uses the testdev's little register file
// (see hw/misc/iommu-testdev.c) to ask the device to write ITD_DMA_WRITE_VAL to
// an IOVA and then read the same bytes back from a physical address.  The
// readback is deliberately a *physical* access, so the only translation under
// test is the one the SMMU performs for the device write.
//
// Everything here is dev-build-only and only runs when an iommu-testdev
// function is actually present, so the default verification topology is
// untouched.

#include "anko/smmuv3.h"

#ifdef ANKO_BUILD_DEV

#include "anko/hw_info.h"
#include "anko/log.h"
#include "anko/mm.h"
#include "anko/pci.h"
#include "libk/string.h"

namespace iommu {

namespace {

// --- QEMU iommu-testdev identity and programming model ----------------------
constexpr u16 kTestdevVendor = 0x1b36;
constexpr u16 kTestdevDevice = 0x0005;

// BAR0 aperture.  Kept clear of the native xHCI's 0x10000000 so both can be
// assigned by the direct -kernel boot (no firmware does it for us).
constexpr u64 kTestdevBarBase = 0x1010'0000ull;
constexpr u64 kTestdevBarSize = 0x1000;

// BAR0 register offsets.
constexpr u32 kRegTrigger = 0x00; // read triggers the DMA
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

// PCI config-space offsets.
constexpr u32 kCfgCommand = 0x04;
constexpr u32 kCfgBar0 = 0x10;
constexpr u32 kCmdMemoryEnable = 1u << 1;

// IOVAs used by the tests (all well below the 39-bit core limit).
constexpr u64 kIovaMapped = 0x8000'0000ull;
constexpr u64 kIovaReadOnly = 0x8000'1000ull;

constexpr u32 kCanaryWord = 0xA5A5'A5A5u;
constexpr u32 kPoisonWord = 0xBEEF'FACEu;
constexpr u32 kFaultInjections = 16;
constexpr u64 kTestObject = 0x5445'5354'4445'5649ull; // "TESTDEVI"

// --- PCI config access (the ECAM window is bootstrap-identity mapped) --------
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

// --- testdev MMIO -----------------------------------------------------------
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

// Ask the device to write kWriteValue to `gva` (translated by the SMMU) and
// read it back from `gpa` (a physical address).  Returns the result register.
u32 td_dma(u64 gva, u64 gpa, u32 len) {
    (void)td_read(kRegResult); // settle
    td_write(kRegGvaLo, static_cast<u32>(gva));
    td_write(kRegGvaHi, static_cast<u32>(gva >> 32));
    td_write(kRegGpaLo, static_cast<u32>(gpa));
    td_write(kRegGpaHi, static_cast<u32>(gpa >> 32));
    td_write(kRegLen, len);
    td_write(kRegAttrs, 0); // non-secure, no explicit space
    td_write(kRegDbell, kDbellArm);
    (void)td_read(kRegTrigger); // consume the armed request
    return td_read(kRegResult);
}

// --- cache maintenance between CPU and device writes ------------------------
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

// --- page-owner model for the test domain -----------------------------------
struct TestArena {
    PhysAddr base;
    u64 size;
    u64 object;
};
TestArena g_test_arena;

bool test_owner_query(void* ctx, PhysAddr pa, PageOwnership* out) {
    auto* arena = static_cast<TestArena*>(ctx);
    if (pa < arena->base || pa >= arena->base + arena->size) {
        return false;
    }
    out->object = arena->object;
    out->dma_class = VmDmaClass::ApplianceRam;
    libk::memset(out->pad, 0, sizeof(out->pad));
    return true;
}

void test_owner_release(void*, PhysAddr, u64) {
    // The test pages stay allocated for the lifetime of the boot.
}

// --- helpers ----------------------------------------------------------------
bool find_testdev(u32* out_sid, u32* out_bus, u32* out_dev, u32* out_fn, u64* out_bar_size) {
    static pci::PciFunctionInfo functions[pci::kMaxFunctions];
    const u32 count = pci::enumerate(g_hw.ecam_base, functions, pci::kMaxFunctions);
    for (u32 i = 0; i < count; ++i) {
        const pci::PciFunctionInfo& f = functions[i];
        if (f.vendor_id != kTestdevVendor || f.device_id != kTestdevDevice) {
            continue;
        }
        *out_sid = f.requester_id;
        *out_bus = f.bus;
        *out_dev = f.device;
        *out_fn = f.function;
        *out_bar_size = f.bars[0].size ? f.bars[0].size : kTestdevBarSize;
        return true;
    }
    return false;
}

int g_checks;
int g_failures;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        ANKO_LOGF_ERROR("iommu/selftest", "  check failed: %s", what);
    }
}

u32 peek32(PhysAddr pa) {
    cache_invalidate(pa, 4);
    return *reinterpret_cast<volatile u32*>(pa);
}

} // namespace

void smmuv3_selftest_if_testdev() {
    if (!smmuv3_ready() || !g_hw.has_pcie || g_hw.ecam_base == 0) {
        return;
    }

    u32 sid = 0;
    u32 bus = 0;
    u32 dev = 0;
    u32 fn = 0;
    u64 bar_size = 0;
    if (!find_testdev(&sid, &bus, &dev, &fn, &bar_size)) {
        return; // no test device in this topology
    }
    if (bar_size > kTestdevBarSize) {
        ANKO_LOGF_WARN("iommu/selftest", "testdev BAR0 larger than expected: 0x%llx",
                       static_cast<unsigned long long>(bar_size));
    }

    // Assign BAR0 into the PCI MMIO aperture and enable memory decoding.  The
    // test device does not require bus mastering in QEMU, and the inventory
    // deliberately leaves it disabled.
    const u32 bar_orig = cfg_read(bus, dev, fn, kCfgBar0);
    cfg_write(bus, dev, fn, kCfgBar0, static_cast<u32>(kTestdevBarBase) | (bar_orig & 0xFu));
    cfg_write(bus, dev, fn, kCfgCommand, cfg_read(bus, dev, fn, kCfgCommand) | kCmdMemoryEnable);

    if (!mm::map_kernel_device_mmio(kTestdevBarBase, kTestdevBarSize)) {
        ANKO_LOG_ERROR("iommu/selftest", "TEST FAILED: cannot map testdev BAR0");
        return;
    }

    // Total register sanity: the device reads back the address it was given.
    td_write(kRegGvaLo, 0x1122'3344u);
    if (td_read(kRegGvaLo) != 0x1122'3344u) {
        ANKO_LOG_ERROR("iommu/selftest", "TEST FAILED: testdev register readback mismatch");
        return;
    }

    ANKO_LOGF_INFO("iommu/selftest", "testdev sid=%u bdf=%u:%u.%u bar=0x%llx", sid, bus, dev, fn,
                   static_cast<unsigned long long>(kTestdevBarBase));

    g_checks = 0;
    g_failures = 0;

    // Four contiguous pages: canary_lo, target, canary_hi, read-only target.
    const PhysAddr pages = mm::alloc_pages_contiguous(4);
    if (!pages) {
        ANKO_LOG_ERROR("iommu/selftest", "TEST FAILED: no memory for test pages");
        return;
    }
    const PhysAddr target = pages + PAGE_SIZE;
    const PhysAddr canary = pages + 2 * PAGE_SIZE;
    const PhysAddr readonly_page = pages + 3 * PAGE_SIZE;

    auto* fill = reinterpret_cast<volatile u32*>(pages);
    for (u64 i = 0; i < (4u * PAGE_SIZE) / 4u; ++i) {
        fill[i] = kCanaryWord;
    }
    cache_clean(pages, 4u * PAGE_SIZE);

    g_test_arena = TestArena{pages, 4u * PAGE_SIZE, kTestObject};
    IommuOwnerOps owner{};
    owner.query = test_owner_query;
    owner.release = test_owner_release;
    owner.ctx = &g_test_arena;

    IommuDomain* domain = nullptr;
    if (domain_create(kTestObject, owner, smmuv3_backend(), &domain) != E_OK) {
        ANKO_LOG_ERROR("iommu/selftest", "TEST FAILED: domain_create");
        return;
    }
    const IommuDeviceId device{pci::kSegment, sid};

    IommuMap mapped{};
    mapped.iova = kIovaMapped;
    mapped.pa = target;
    mapped.length = PAGE_SIZE;
    mapped.perms = static_cast<u32>(DmaPerm::Read) | static_cast<u32>(DmaPerm::Write);

    IommuMap readonly_map{};
    readonly_map.iova = kIovaReadOnly;
    readonly_map.pa = readonly_page;
    readonly_map.length = PAGE_SIZE;
    readonly_map.perms = static_cast<u32>(DmaPerm::Read);

    IommuMap maps[2] = {mapped, readonly_map};
    if (domain_map_batch(*domain, maps, 2) != E_OK || domain_attach(*domain, device) != E_OK) {
        ANKO_LOG_ERROR("iommu/selftest", "TEST FAILED: map/attach");
        return;
    }

    {
        u32 ste[16] = {};
        (void)smmuv3_debug_ste(sid, ste);
        ANKO_LOGF_INFO("iommu/selftest",
                       "ste sid=%u valid_config=0x%x s2cfg=0x%x s2ttb=0x%x/0x%x root=0x%llx", sid,
                       ste[0], ste[5], ste[6], ste[7],
                       static_cast<unsigned long long>(domain->root_table_));
    }

    // (a) DMA to a mapped IOVA succeeds and lands on the backing page.
    {
        const u64 faults0 = debug_counter(IOMMU_FAULTS);
        const u32 result = td_dma(kIovaMapped, target, 4);
        check(result == kResultOk, "(a) mapped DMA result");
        check(peek32(target) == kWriteValue, "(a) mapped DMA reached the backing page");
        check(debug_counter(IOMMU_FAULTS) == faults0, "(a) mapped DMA produced no fault");
        ANKO_LOGF_INFO("iommu/selftest", "(a) mapped DMA ok result=0x%x", result);
    }

    // (f) An IOVA that has been unmapped and synchronised can no longer be
    // reached: the range-scoped TLBI must have dropped the cached translation,
    // so the device's write faults and the released page is untouched.
    {
        const u64 tlbi0 = debug_counter(IOMMU_TLBI_RANGE);
        const u64 faults0 = debug_counter(IOMMU_FAULTS);
        IommuMap unmap{};
        unmap.iova = kIovaMapped;
        unmap.pa = target;
        unmap.length = PAGE_SIZE;
        check(domain_unmap_batch(*domain, &unmap, 1) == E_OK, "(f) unmap");
        check(debug_counter(IOMMU_TLBI_RANGE) > tlbi0, "(f) range invalidation issued");
        *reinterpret_cast<volatile u32*>(target) = kPoisonWord;
        cache_clean(target, 4);
        const u32 result = td_dma(kIovaMapped, target, 4);
        (void)smmuv3_drain_events();
        check(result == kResultErrTxFail, "(f) DMA to the unmapped IOVA faulted");
        check(peek32(target) == kPoisonWord, "(f) the released page was not written");
        check(debug_counter(IOMMU_FAULTS) > faults0, "(f) unmap fault recorded");
        ANKO_LOGF_INFO("iommu/selftest", "(f) unmap + scoped TLBI ok result=0x%x tlbi_range=%llu",
                       result, static_cast<unsigned long long>(debug_counter(IOMMU_TLBI_RANGE)));
    }

    // (b) DMA to an unmapped IOVA faults and cannot touch the canary page.  The
    // unmapped IOVA is deliberately the canary's *physical* address, so an
    // identity (bypass) translation would have corrupted exactly that page.
    {
        const u64 faults0 = debug_counter(IOMMU_FAULTS);
        const u32 result = td_dma(canary, canary, 4);
        (void)smmuv3_drain_events();
        check(result == kResultErrTxFail, "(b) unmapped IOVA faulted");
        check(peek32(canary) == kCanaryWord, "(b) canary page unchanged");
        check(debug_counter(IOMMU_FAULTS) > faults0, "(b) IOMMU_FAULTS advanced");
        ANKO_LOGF_INFO(
            "iommu/selftest", "(b) unmapped fault ok result=0x%x faults=%llu canary=0x%x", result,
            static_cast<unsigned long long>(debug_counter(IOMMU_FAULTS)), peek32(canary));
    }

    // (c) A read-only mapping rejects a device write.
    {
        const u64 faults0 = debug_counter(IOMMU_FAULTS);
        const u32 result = td_dma(kIovaReadOnly, readonly_page, 4);
        (void)smmuv3_drain_events();
        check(result == kResultErrTxFail, "(c) read-only mapping rejected the write");
        check(peek32(readonly_page) == kCanaryWord, "(c) read-only page unchanged");
        check(debug_counter(IOMMU_FAULTS) > faults0, "(c) write rejection recorded a fault");
        ANKO_LOGF_INFO("iommu/selftest", "(c) read-only rejection ok result=0x%x", result);
    }

    // (d) A detached requester faults rather than bypassing translation.
    {
        *reinterpret_cast<volatile u32*>(target) = kPoisonWord;
        cache_clean(target, 4);
        check(domain_detach(*domain, device) == E_OK, "(d) detach");
        const u64 faults0 = debug_counter(IOMMU_FAULTS);
        const u32 result = td_dma(kIovaMapped, target, 4);
        (void)smmuv3_drain_events();
        check(result == kResultErrTxFail, "(d) detached requester faulted");
        check(peek32(target) == kPoisonWord, "(d) detached requester wrote nothing");
        check(debug_counter(IOMMU_FAULTS) > faults0, "(d) detach fault recorded");
        ANKO_LOGF_INFO("iommu/selftest", "(d) detached fault ok result=0x%x", result);
    }

    // (e) Repeated faults cannot overflow the event queue and move the stream to
    // quarantine (STE faulting, bus-master cleared).
    {
        const u64 quarantines0 = debug_counter(IOMMU_QUARANTINES);
        check(domain_attach(*domain, device) == E_OK, "(e) re-attach");
        for (u32 i = 0; i < kFaultInjections; ++i) {
            (void)td_dma(canary, canary, 4);
            (void)smmuv3_drain_events();
        }
        const u32 state = smmuv3_stream_state(sid);
        const bool over = (smmuv3_gerror() & smmuv3::kGerrorEventqAbtErr) != 0;
        check(state == 3, "(e) stream quarantined");
        check(debug_counter(IOMMU_QUARANTINES) > quarantines0, "(e) quarantine counted");
        check(!over, "(e) event queue did not overflow");
        ANKO_LOGF_INFO("iommu/selftest",
                       "(e) repeated faults ok injections=%u stream_faults=%u state=%u "
                       "quarantines=%llu gerror=0x%x",
                       kFaultInjections, smmuv3_stream_faults(sid), state,
                       static_cast<unsigned long long>(debug_counter(IOMMU_QUARANTINES)),
                       smmuv3_gerror());
    }

    ANKO_LOGF_INFO("iommu/selftest",
                   "counters IOMMU_FAULTS=%llu IOMMU_QUARANTINES=%llu IOMMU_TLBI_RANGE=%llu "
                   "events=%llu",
                   static_cast<unsigned long long>(debug_counter(IOMMU_FAULTS)),
                   static_cast<unsigned long long>(debug_counter(IOMMU_QUARANTINES)),
                   static_cast<unsigned long long>(debug_counter(IOMMU_TLBI_RANGE)),
                   static_cast<unsigned long long>(smmuv3_events_drained()));

    if (g_failures == 0) {
        ANKO_LOGF_INFO("iommu/selftest", "iommu dma test passed (%d checks)", g_checks);
    } else {
        ANKO_LOGF_ERROR("iommu/selftest", "TEST FAILED: %d/%d checks failed", g_failures, g_checks);
    }
}

} // namespace iommu

#else // !ANKO_BUILD_DEV

namespace iommu {
void smmuv3_selftest_if_testdev() {}
} // namespace iommu

#endif
