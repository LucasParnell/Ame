// kernel/mm/vm_space.cc — per-process address space / page table operations.
//
// Spec ref: §5.2 Memory Management, §5.3 Address Spaces
//
// AArch64 4KB page, 48-bit VA, 4-level walk (L0–L3).
// TTBR0_EL1 → user space (lower half).
// TTBR1_EL1 → kernel (upper half, set once in entry.S, never per-process).
//
// V0 operations needed:
//   vm_space_create()           — allocate PGD, return VmSpace
//   vm_map(space, va, pa, rights) — map one page
//   vm_unmap(space, va)          — unmap one page
//   vm_map_shared(space, va, pa) — map a page shared between two spaces
//   vm_space_destroy(space)      — reclaim all page-table pages

#include "anko/mm.h"
#include "anko/sched.h"
#include "anko/cap.h"
#include "anko/hw_info.h"
#include "anko/platform.h"
#include "anko/panic.h"
#include "anko/boot_trace.h"
#if defined(ANKO_ANNE_DISPLAY_CAPTURE) || defined(ANKO_ANNE_FRAMEBUFFER)
#include "display_diagnostics.h"
#endif
#ifdef ANKO_BOARD_HUAWEI_ANNE
#include "debug_io.h"
#endif
#include "anko/spinlock.h"
#include "libk/string.h"

extern "C" char __text_start[];
extern "C" char __kernel_end[];

namespace mm {

namespace {

constexpr u64 kBootstrapRamMapSize = 512ull * 1024ull * 1024ull;
constexpr u64 kDeviceMapSize = 0x20000;
constexpr u64 kSmmuMapSize = 0x20000; // SMMUv3 register window (QEMU virt)
constexpr PhysAddr kTegraDc0Base = 0x5420'0000ull;
constexpr u64 kTegraDc0MapSize = 0x40000;
constexpr PhysAddr kTegraXusbBase = 0x7009'0000ull;
constexpr u64 kTegraXusbMapSize = 0x10000;

constexpr u64 PTE_VALID = 1ull << 0;
constexpr u64 PTE_TABLE = 1ull << 1;
constexpr u64 PTE_AF = 1ull << 10;
constexpr u64 PTE_NG = 1ull << 11;
constexpr u64 PTE_AP_EL0 = 1ull << 6;
constexpr u64 PTE_AP_RO = 1ull << 7;
constexpr u64 PTE_SH_INNER = 3ull << 8;
constexpr u64 PTE_PXN = 1ull << 53;
constexpr u64 PTE_UXN = 1ull << 54;
constexpr u64 PTE_SW_OWNED = 1ull << 55;
constexpr u64 PTE_ADDR_MASK = 0x0000'FFFF'FFFF'F000ull;
constexpr u64 DESC_TABLE = PTE_VALID | PTE_TABLE;
constexpr u64 DESC_PAGE = PTE_VALID | PTE_TABLE;

constexpr u64 ATTR_NORMAL = static_cast<u64>(platform::kMairNormal) << 2;
constexpr u64 ATTR_NORMAL_NC = static_cast<u64>(platform::kMairNormalNc) << 2;
constexpr u64 ATTR_DEVICE = static_cast<u64>(platform::kMairDevice) << 2;

static PhysAddr g_kernel_root = 0;
static kernel::SpinLock g_vm_lock{};
struct PhysicalRange { PhysAddr base; u64 size; };
static PhysicalRange g_noncached[8];
static u32 g_noncached_count;
static u64 normal_attrs(PhysAddr pa) {
#ifdef ANKO_BOARD_HUAWEI_ANNE
    if (pa >= anne::kDebugRamBase && pa - anne::kDebugRamBase < anne::kDebugRamBytes) return ATTR_NORMAL_NC;
    if (pa >= anne::kFirmwareFiqSource && pa - anne::kFirmwareFiqSource < PAGE_SIZE) return ATTR_NORMAL_NC;
#if defined(ANKO_ANNE_WATCHDOG) && defined(ANKO_ANNE_DIAG_TEST)
    if (pa >= anne::watchdog::kBase && pa - anne::watchdog::kBase < PAGE_SIZE) return ATTR_NORMAL_NC;
#endif
#endif
    if (g_hw.has_t210_dc && pa >= g_hw.t210_dc_fb_phys &&
        pa - g_hw.t210_dc_fb_phys < static_cast<u64>(g_hw.t210_dc_fb_width) * g_hw.t210_dc_fb_height * 4) return ATTR_NORMAL_NC;
    for (u32 i = 0; i < g_noncached_count; ++i)
        if (pa >= g_noncached[i].base && pa - g_noncached[i].base < g_noncached[i].size) return ATTR_NORMAL_NC;
    return ATTR_NORMAL;
}

u64* table_ptr(PhysAddr pa) {
    return reinterpret_cast<u64*>(pa);
}

u32 level_index(VirtAddr va, u32 level) {
    const u32 shift = 39 - (level * 9);
    return static_cast<u32>((va >> shift) & 0x1FFu);
}

bool is_table_desc(u64 desc) {
    return (desc & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE);
}

void tlbi_all();

u64 user_page_attrs(u32 rights, PhysAddr pa) {
    const bool writable = (rights & RIGHT_WRITE) != 0;
    const bool executable = (rights & RIGHT_EXEC) != 0;
    u64 attrs = normal_attrs(pa) | PTE_AF | PTE_SH_INNER | PTE_AP_EL0 | PTE_NG | PTE_PXN;
    if (!writable) attrs |= PTE_AP_RO;
    if (!executable) attrs |= PTE_UXN;
    return attrs;
}

u64 user_device_attrs(u32 rights) {
    const bool writable = (rights & RIGHT_WRITE) != 0;
    u64 attrs = ATTR_DEVICE | PTE_AF | PTE_AP_EL0 | PTE_NG | PTE_PXN | PTE_UXN;
    if (!writable) attrs |= PTE_AP_RO;
    return attrs;
}

u64 kernel_page_attrs(bool device, PhysAddr pa) {
#if defined(ANKO_BOARD_HUAWEI_ANNE) && defined(ANKO_ANNE_DIAG_TEST)
    if ((pa >= anne::kDebugPmicBase && pa - anne::kDebugPmicBase < PAGE_SIZE) ||
        (pa >= anne::kDebugSctrlBase && pa - anne::kDebugSctrlBase < PAGE_SIZE)) device = true;
#if defined(ANKO_ANNE_DISPLAY_CAPTURE) || defined(ANKO_ANNE_FRAMEBUFFER)
    if ((pa >= anne::native_display::kDssBase && pa - anne::native_display::kDssBase < anne::native_display::kDssBytes) ||
        (pa >= anne::native_display::kCrgBase && pa - anne::native_display::kCrgBase < PAGE_SIZE)) device = true;
#endif
#endif
    u64 attrs = (device ? ATTR_DEVICE : normal_attrs(pa)) | PTE_AF | PTE_UXN;
    if (!device) attrs |= PTE_SH_INNER;
    if (device) attrs |= PTE_PXN;
    return attrs;
}

bool map_page_raw(VmSpace& space, VirtAddr va, PhysAddr pa, u64 attrs) {
    if (!space.pgd_phys) return false;
    if ((va & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;
    if ((attrs & PTE_ADDR_MASK) != 0) return false;
    const bool user = (attrs & PTE_AP_EL0) != 0;
    const bool writable = (attrs & PTE_AP_RO) == 0;
    const bool executable = (attrs & PTE_UXN) == 0;
    if (user && writable && executable) return false;

    PhysAddr table_pa = space.pgd_phys;
    u64* created_links[3]{};
    PhysAddr created_pages[3]{};
    u32 created_count = 0;
    auto rollback = [&]() {
        // Remove only tables created by this walk. Existing mappings are intact.
        for (u32 i = 0; i < created_count; ++i) *created_links[i] = 0;
        if (created_count) tlbi_all();
        for (u32 i = 0; i < created_count; ++i) free_page(created_pages[i]);
        return false;
    };
    for (u32 level = 0; level < 3; ++level) {
        u64* table = table_ptr(table_pa);
        const u32 idx = level_index(va, level);
        const u64 desc = table[idx];
        if (is_table_desc(desc)) {
            table_pa = desc & PTE_ADDR_MASK;
            continue;
        }
        if (desc & PTE_VALID) return rollback();
        const PhysAddr child = alloc_page();
        if (!child) return rollback();
        libk::memset(reinterpret_cast<void*>(child), 0, PAGE_SIZE);
        created_links[created_count] = &table[idx];
        created_pages[created_count++] = child;
        table[idx] = (child & PTE_ADDR_MASK) | DESC_TABLE;
        table_pa = child;
    }

    u64* l3 = table_ptr(table_pa);
    const u32 idx = level_index(va, 3);
    if (l3[idx] & PTE_VALID) return rollback();
    l3[idx] = (pa & PTE_ADDR_MASK) | DESC_PAGE | attrs;
    return true;
}

bool map_identity_range(VmSpace& space, PhysAddr base, u64 size, bool device) {
    const PhysAddr start = base & ~(static_cast<PhysAddr>(PAGE_SIZE - 1));
    const PhysAddr end = (base + size + PAGE_SIZE - 1) & ~(static_cast<PhysAddr>(PAGE_SIZE - 1));
    for (PhysAddr pa = start; pa < end; pa += PAGE_SIZE) {
        const u64 attrs = kernel_page_attrs(device, pa);
        if (!map_page_raw(space, static_cast<VirtAddr>(pa), pa, attrs)) return false;
    }
    return true;
}

bool contains_range(PhysAddr outer_base, u64 outer_size, PhysAddr inner_base, u64 inner_size) {
    if (outer_size == 0 || inner_size == 0) return false;
    u64 outer_end = 0;
    u64 inner_end = 0;
    if (__builtin_add_overflow(outer_base, outer_size, &outer_end)) return false;
    if (__builtin_add_overflow(inner_base, inner_size, &inner_end)) return false;
    return inner_base >= outer_base && inner_end <= outer_end && inner_end >= inner_base;
}

bool map_identity_if_present(VmSpace& space, PhysAddr base, u64 size, bool device) {
    if (base == 0 || size == 0) return true;
    return map_identity_range(space, base, size, device);
}

bool map_bootstrap_identity(VmSpace& space) {
    const u64 ram_map_size =
        g_hw.ram_size < kBootstrapRamMapSize ? g_hw.ram_size : kBootstrapRamMapSize;
    if (!map_identity_if_present(space, g_hw.ram_base, ram_map_size, false)) return false;

    const PhysAddr kernel_base = reinterpret_cast<PhysAddr>(&__text_start[0]);
    const PhysAddr kernel_end = reinterpret_cast<PhysAddr>(&__kernel_end[0]);
    if (kernel_end > kernel_base &&
        !contains_range(g_hw.ram_base, ram_map_size, kernel_base, kernel_end - kernel_base)) {
        if (!map_identity_range(space, kernel_base, kernel_end - kernel_base, false)) return false;
    }

    if (!map_identity_if_present(space, g_hw.gic_dist_base, kDeviceMapSize, true)) return false;
    if (!contains_range(g_hw.gic_dist_base, kDeviceMapSize, g_hw.gic_cpu_base, PAGE_SIZE)) {
        if (!map_identity_if_present(space, g_hw.gic_cpu_base, PAGE_SIZE, true)) return false;
    }
    if (!map_identity_if_present(space, g_hw.console_base, PAGE_SIZE, true)) return false;
#ifdef ANKO_BOARD_HUAWEI_ANNE
    // Fixed device pages used by capability-scoped kernel board mechanisms.
#ifndef ANKO_ANNE_DIAG_TEST
    if (!map_identity_range(space, 0xfff35000, PAGE_SIZE, true) ||
        !map_identity_range(space, 0xe8a09000, PAGE_SIZE, true)) return false;
#endif
    // Persistent native console lies in reserved RAM outside the allocator bank.
#ifdef ANKO_ANNE_WATCHDOG
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::watchdog::kBase, PAGE_SIZE) &&
        !map_identity_range(space, anne::watchdog::kBase, PAGE_SIZE,
#ifdef ANKO_ANNE_DIAG_TEST
                            false
#else
                            true
#endif
                            )) return false;
#endif
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::kFirmwareFiqSource, PAGE_SIZE) &&
        !map_identity_range(space, anne::kFirmwareFiqSource, PAGE_SIZE, false)) return false;
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::kDebugRamBase, anne::kDebugRamBytes) &&
        !map_identity_range(space, anne::kDebugRamBase, anne::kDebugRamBytes, false)) return false;
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::kDebugPmicBase, PAGE_SIZE) &&
        !map_identity_range(space, anne::kDebugPmicBase, PAGE_SIZE, true)) return false;
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::kDebugSctrlBase, PAGE_SIZE) &&
        !map_identity_range(space, anne::kDebugSctrlBase, PAGE_SIZE, true)) return false;
#if defined(ANKO_ANNE_DISPLAY_CAPTURE) || defined(ANKO_ANNE_FRAMEBUFFER)
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::native_display::kDssBase, anne::native_display::kDssBytes) &&
        !map_identity_range(space, anne::native_display::kDssBase, anne::native_display::kDssBytes, true)) return false;
#ifdef ANKO_ANNE_DIAG_TEST
    // The synthetic display fixture has its own CRG page. Native CRG is shared
    // by display and USB and was already mapped with the fixed board mechanisms
    // above; mapping it twice rejects the entire bootstrap address space.
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::native_display::kCrgBase, PAGE_SIZE) &&
        !map_identity_range(space, anne::native_display::kCrgBase, PAGE_SIZE, true)) return false;
#endif
    if (!contains_range(g_hw.ram_base, ram_map_size, anne::native_display::kLogoBase, anne::native_display::kLogoBytes) &&
        !map_identity_range(space, anne::native_display::kLogoBase, anne::native_display::kLogoBytes, false)) return false;
#endif
#endif

    if (g_hw.has_fwcfg &&
        !map_identity_if_present(space, g_hw.fwcfg_base, PAGE_SIZE, true)) return false;
    if (g_hw.has_virtio_blk &&
        !map_identity_if_present(space, g_hw.virtio_base, platform::kVirtioMmioSize, true)) return false;
    if (g_hw.has_pcie &&
        !map_identity_if_present(space, g_hw.ecam_base, platform::kPciEcamProbeSize, true)) return false;

    if (g_hw.console_kind == ConsoleKind::Tegra16550) {
        if (!map_identity_range(space, kTegraDc0Base, kTegraDc0MapSize, true)) return false;
        if (!map_identity_range(space, kTegraXusbBase, kTegraXusbMapSize, true)) return false;
    }

    // SMMUv3 register window.  Phase C programs the SMMU from the kernel, so its
    // control/queue registers must be reachable before any assignable device
    // can bus-master.
    if (g_hw.has_smmu &&
        !map_identity_if_present(space, g_hw.smmu_base, kSmmuMapSize, true)) return false;

    if (g_hw.has_t210_dc) {
        const u64 fb_size = static_cast<u64>(g_hw.t210_dc_fb_width) *
                            static_cast<u64>(g_hw.t210_dc_fb_height) * 4ull;
        if (fb_size != 0 &&
            !contains_range(g_hw.ram_base, ram_map_size, g_hw.t210_dc_fb_phys, fb_size)) {
            if (!map_identity_range(space, g_hw.t210_dc_fb_phys, fb_size, false)) return false;
        }
    }

    return true;
}

void tlbi_all() {
    asm volatile(
        "dsb ishst\n"
        "tlbi vmalle1is\n"
        "dsb ish\n"
        "isb\n"
        :
        :
        : "memory");
}

} // namespace

void sync_icache_range(PhysAddr pa, usize len) {
    if (!pa || len == 0) return;

    u64 ctr = 0;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));

    const usize dline = 4ull << ((ctr >> 16) & 0xf);
    const PhysAddr start = pa & ~(static_cast<PhysAddr>(dline - 1));
    const PhysAddr end = (pa + len + dline - 1) & ~(static_cast<PhysAddr>(dline - 1));

    // `pa` is also a valid kernel VA in this kernel's identity map. Clean the
    // bytes just copied through the data side out to PoU, then invalidate all
    // instruction caches because the same physical page will be executed via a
    // different EL0 VA.
    for (PhysAddr p = start; p < end; p += dline) {
        asm volatile("dc cvau, %0" :: "r"(p) : "memory");
    }

    asm volatile(
        "dsb ish\n"
        "ic ialluis\n"
        "dsb ish\n"
        "isb\n"
        :
        :
        : "memory");
}

namespace {

void enable_mmu(PhysAddr root) {
    constexpr u64 mair =
        (0xFFull << (platform::kMairNormal * 8)) |
        (0x00ull << (platform::kMairDevice * 8)) |
        (0x44ull << (platform::kMairNormalNc * 8));
    constexpr u64 tcr =
        (16ull << 0) |      // T0SZ: 48-bit VA
        (1ull << 8) |       // IRGN0: WBWA
        (1ull << 10) |      // ORGN0: WBWA
        (3ull << 12) |      // SH0: inner-shareable
        (0ull << 14) |      // TG0: 4 KiB
        (2ull << 32);       // IPS: 40-bit PA, enough for QEMU virt PCI ECAM

    asm volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        "mrs x9, sctlr_el1\n"
        "orr x9, x9, #(1 << 0)\n"
        "orr x9, x9, #(1 << 2)\n"
        "orr x9, x9, #(1 << 12)\n"
        "msr sctlr_el1, x9\n"
        "isb\n"
        :
        : "r"(mair), "r"(tcr), "r"(root)
        : "x9", "memory");
}

bool valid_user_rights(u32 rights, bool device) {
    if (rights == 0) return false;
    if ((rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC)) != 0) return false;
    if ((rights & RIGHT_WRITE) && (rights & RIGHT_EXEC)) return false;
    if (device && (rights & RIGHT_EXEC)) return false;
    return true;
}

bool unmap_page_raw(VmSpace& space, VirtAddr va, bool free_owned) {
    if (!space.pgd_phys || (va & (PAGE_SIZE - 1))) return false;
    PhysAddr path[4]{space.pgd_phys, 0, 0, 0};
    for (u32 level = 0; level < 3; ++level) {
        const u64 desc = table_ptr(path[level])[level_index(va, level)];
        if (!is_table_desc(desc)) return false;
        path[level + 1] = desc & PTE_ADDR_MASK;
    }
    auto* leaf = table_ptr(path[3]);
    const u64 pte = leaf[level_index(va, 3)];
    if (!(pte & PTE_VALID)) return false;
    leaf[level_index(va, 3)] = 0;
    PhysAddr retired[3]{}; u32 count = 0;
    for (u32 level = 3; level > 0; --level) {
        bool empty = true;
        for (u32 i = 0; i < 512; ++i) if (table_ptr(path[level])[i] & PTE_VALID) { empty = false; break; }
        if (!empty) break;
        table_ptr(path[level - 1])[level_index(va, level - 1)] = 0;
        retired[count++] = path[level];
    }
    // Remote CPUs must discard cached translations before any backing or page
    // table can be returned to the allocator and used for a different object.
    tlbi_all();
    for (u32 i = 0; i < count; ++i) free_page(retired[i]);
    if (free_owned && (pte & PTE_SW_OWNED)) free_page_for(space.pid, pte & PTE_ADDR_MASK);
    return true;
}

void destroy_table(VmSpace& space, PhysAddr table_pa, u32 level) {
    if (!table_pa) return;
    u64* table = table_ptr(table_pa);
    for (u32 i = 0; i < 512; ++i) {
        const u64 desc = table[i];
        if ((desc & PTE_VALID) == 0) continue;
        if (level < 3 && is_table_desc(desc)) {
            destroy_table(space, desc & PTE_ADDR_MASK, level + 1);
        } else if (level == 3 && (desc & PTE_SW_OWNED)) {
            free_page_for(space.pid, desc & PTE_ADDR_MASK);
        }
        table[i] = 0;
    }
    free_page(table_pa);
}

} // namespace

bool map_kernel_device_mmio(PhysAddr base, u64 size) {
    if (!g_kernel_root || size == 0 || (base & (PAGE_SIZE - 1))) return false;
    VmSpace kernel{};
    kernel.pgd_phys = g_kernel_root;
    kernel.pid = 0;
    kernel::LockGuard guard(g_vm_lock);
    const bool ok = map_identity_range(kernel, base, size, true);
    if (ok) tlbi_all();
    return ok;
}

bool range_is_noncached(PhysAddr base, u64 size) {
    if (!size || ((base | size) & (PAGE_SIZE - 1)) || size > ~0ull - base) return false;
    for (u64 offset = 0; offset < size; offset += PAGE_SIZE)
        if (normal_attrs(base + offset) != ATTR_NORMAL_NC) return false;
    return true;
}

bool register_noncached_range(PhysAddr base, u64 size) {
    if (!size || ((base | size) & (PAGE_SIZE - 1)) || size > ~0ull - base) return false;
    kernel::LockGuard guard(g_vm_lock);
    if (g_noncached_count == 8) return false;
    // Called before userspace launch. Clean the identity mapping before changing
    // its cache attributes; every later alias consults this physical range map.
    u64 ctr = 0; asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (PhysAddr p = base; p < base + size; p += line) asm volatile("dc civac, %0" :: "r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
    g_noncached[g_noncached_count++] = {base, size};
    auto update = [&](PhysAddr root) {
        if (!root) return;
        for (PhysAddr pa = base; pa < base + size; pa += PAGE_SIZE) {
            PhysAddr table = root; bool found = true;
            for (u32 level = 0; level < 3; ++level) {
                u64 desc = table_ptr(table)[level_index(pa, level)];
                if (!is_table_desc(desc)) { found = false; break; }
                table = desc & PTE_ADDR_MASK;
            }
            if (!found) continue;
            auto& pte = table_ptr(table)[level_index(pa, 3)];
            if (!(pte & PTE_VALID)) continue;
            const u64 old = pte;
            pte = 0; tlbi_all();
            pte = (old & ~(7ull << 2)) | ATTR_NORMAL_NC;
        }
    };
    update(g_kernel_root);
    for (u32 pid = 1; pid <= 64; ++pid) if (auto p = sched::find_process(pid)) update(p->ttbr0);
    tlbi_all();
    return true;
}

void vm_init() {
    VmSpace kernel{};
    kernel.pid = 0;
    kernel.pgd_phys = alloc_page();
    if (!kernel.pgd_phys) panic("kernel page-table allocation failed");
    libk::memset(reinterpret_cast<void*>(kernel.pgd_phys), 0, PAGE_SIZE);

    if (!map_bootstrap_identity(kernel)) {
        destroy_table(kernel, kernel.pgd_phys, 0);
        panic("kernel identity mapping failed");
    }

    g_kernel_root = kernel.pgd_phys;
    ANKO_BOOT_CHECKPOINT(MmuEnable);
    enable_mmu(g_kernel_root);
}

void vm_enable_kernel() {
    if (g_kernel_root) enable_mmu(g_kernel_root);
}

VmSpace vm_space_create(u32 pid) {
    VmSpace s{};
    s.pid = pid;
    s.pgd_phys = alloc_page();
    if (s.pgd_phys) {
        libk::memset(reinterpret_cast<void*>(s.pgd_phys), 0, PAGE_SIZE);
        if (!map_bootstrap_identity(s)) { destroy_table(s, s.pgd_phys, 0); s.pgd_phys = 0; }
    }
    return s;
}

bool vm_map(VmSpace& space, VirtAddr va, PhysAddr pa, u32 rights) {
    if (!valid_user_rights(rights, false)) return false;
    kernel::LockGuard guard(g_vm_lock);
    const bool ok = map_page_raw(space, va, pa, user_page_attrs(rights, pa));
    if (ok) tlbi_all();
    return ok;
}

bool vm_map_owned(VmSpace& space, VirtAddr va, PhysAddr pa, u32 rights) {
    if (!valid_user_rights(rights, false)) return false;
    kernel::LockGuard guard(g_vm_lock);
    const bool ok = map_page_raw(space, va, pa, user_page_attrs(rights, pa) | PTE_SW_OWNED);
    if (ok) tlbi_all();
    return ok;
}

bool vm_map_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights) {
    if (!valid_user_rights(rights, false)) return false;
    if (size == 0 || (va & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;
    u64 rounded = 0;
    if (__builtin_add_overflow(size, static_cast<u64>(PAGE_SIZE - 1), &rounded)) return false;
    rounded &= ~(static_cast<u64>(PAGE_SIZE - 1));
    u64 end = 0;
    if (__builtin_add_overflow(va, rounded, &end)) return false;
    if (__builtin_add_overflow(pa, rounded, &end)) return false;
    kernel::LockGuard guard(g_vm_lock);
    u64 mapped = 0;
    for (u64 off = 0; off < rounded; off += PAGE_SIZE) {
        if (!map_page_raw(space, va + off, pa + off, user_page_attrs(rights, pa + off))) {
            for (u64 undo = 0; undo < mapped; undo += PAGE_SIZE) {
                (void)unmap_page_raw(space, va + undo, false);
            }
            if (mapped) tlbi_all();
            return false;
        }
        mapped += PAGE_SIZE;
    }
    tlbi_all();
    return true;
}

bool vm_map_device_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights) {
    if (!valid_user_rights(rights, true)) return false;
    if (size == 0 || (va & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;
    u64 rounded = 0;
    if (__builtin_add_overflow(size, static_cast<u64>(PAGE_SIZE - 1), &rounded)) return false;
    rounded &= ~(static_cast<u64>(PAGE_SIZE - 1));
    u64 end = 0;
    if (__builtin_add_overflow(va, rounded, &end) ||
        __builtin_add_overflow(pa, rounded, &end)) return false;
    const u64 attrs = user_device_attrs(rights);
    kernel::LockGuard guard(g_vm_lock);
    u64 mapped = 0;
    for (u64 off = 0; off < rounded; off += PAGE_SIZE) {
        if (!map_page_raw(space, va + off, pa + off, attrs)) {
            for (u64 undo = 0; undo < mapped; undo += PAGE_SIZE) {
                (void)unmap_page_raw(space, va + undo, false);
            }
            if (mapped) tlbi_all();
            return false;
        }
        mapped += PAGE_SIZE;
    }
    tlbi_all();
    return true;
}

void vm_unmap(VmSpace& space, VirtAddr va) {
    kernel::LockGuard guard(g_vm_lock);
    if (unmap_page_raw(space, va, true)) tlbi_all();
}

void vm_unmap_owned_range(VmSpace& space, VirtAddr va, u64 size) {
    if (!space.pgd_phys || size == 0 || (va & (PAGE_SIZE - 1)) != 0) return;
    u64 rounded = 0;
    if (__builtin_add_overflow(size, static_cast<u64>(PAGE_SIZE - 1), &rounded)) return;
    rounded &= ~(static_cast<u64>(PAGE_SIZE - 1));
    kernel::LockGuard guard(g_vm_lock);
    bool changed = false;
    for (u64 off = 0; off < rounded; off += PAGE_SIZE) {
        changed = unmap_page_raw(space, va + off, true) || changed;
    }
    if (changed) tlbi_all();
}

void vm_space_destroy(VmSpace& space) {
    kernel::LockGuard guard(g_vm_lock);
    if (!space.pgd_phys) return;
    destroy_table(space, space.pgd_phys, 0);
    space.pgd_phys = 0;
    tlbi_all();
}

void vm_activate(PhysAddr pgd_phys) {
    if (!pgd_phys) pgd_phys = g_kernel_root;
    asm volatile(
        "dsb ishst\n"
        "msr ttbr0_el1, %0\n"
        "isb\n"
        "tlbi vmalle1is\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(pgd_phys)
        : "memory");
}

bool user_range_mapped(const VmSpace& space, VirtAddr va, u64 size, u32 rights) {
    if (!space.pgd_phys) return false;
    if (size == 0) return true;
    u64 end = 0;
    if (__builtin_add_overflow(va, size, &end)) return false;
    if (end <= va) return false;

    const VirtAddr start = va & ~(static_cast<VirtAddr>(PAGE_SIZE - 1));
    u64 stop_u64 = 0;
    if (__builtin_add_overflow(end, static_cast<u64>(PAGE_SIZE - 1), &stop_u64)) return false;
    const VirtAddr stop = stop_u64 & ~(static_cast<VirtAddr>(PAGE_SIZE - 1));
    for (VirtAddr cur = start; cur < stop; cur += PAGE_SIZE) {
        PhysAddr table_pa = space.pgd_phys;
        for (u32 level = 0; level < 3; ++level) {
            const u64 desc = table_ptr(table_pa)[level_index(cur, level)];
            if (!is_table_desc(desc)) return false;
            table_pa = desc & PTE_ADDR_MASK;
        }
        const u64 pte = table_ptr(table_pa)[level_index(cur, 3)];
        if ((pte & PTE_VALID) == 0) return false;
        if ((pte & PTE_AP_EL0) == 0) return false;
        if ((rights & RIGHT_WRITE) && (pte & PTE_AP_RO)) return false;
        if ((rights & RIGHT_EXEC) && (pte & PTE_UXN)) return false;
    }
    return true;
}

bool user_va_to_pa(const VmSpace& space, VirtAddr va, u32 rights, PhysAddr* out_pa) {
    if (!space.pgd_phys || !out_pa) return false;

    PhysAddr table_pa = space.pgd_phys;
    for (u32 level = 0; level < 3; ++level) {
        const u64 desc = table_ptr(table_pa)[level_index(va, level)];
        if (!is_table_desc(desc)) return false;
        table_pa = desc & PTE_ADDR_MASK;
    }

    const u64 pte = table_ptr(table_pa)[level_index(va, 3)];
    if ((pte & PTE_VALID) == 0) return false;
    if ((pte & PTE_AP_EL0) == 0) return false;
    if ((rights & RIGHT_WRITE) && (pte & PTE_AP_RO)) return false;
    if ((rights & RIGHT_EXEC) && (pte & PTE_UXN)) return false;

    *out_pa = (pte & PTE_ADDR_MASK) | (va & (PAGE_SIZE - 1));
    return true;
}

u64 vm_debug_pte(const VmSpace& space, VirtAddr va) {
    if (!space.pgd_phys) return 0;
    PhysAddr table_pa = space.pgd_phys;
    for (u32 level = 0; level < 3; ++level) {
        const u64 desc = table_ptr(table_pa)[level_index(va, level)];
        if (!is_table_desc(desc)) return desc;
        table_pa = desc & PTE_ADDR_MASK;
    }
    return table_ptr(table_pa)[level_index(va, 3)];
}

} // namespace mm
