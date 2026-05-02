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
#include "anko/cap.h"
#include "anko/platform.h"
#include "libk/string.h"

extern "C" char __text_start[];
extern "C" char __kernel_end[];

namespace mm {

namespace {

constexpr u64 kUserArenaMin = 0x4000'0000ull;
constexpr u64 kUserArenaMax = 0x8000'0000ull;
constexpr u64 kRamMapSize = 512ull * 1024ull * 1024ull;
constexpr u64 kDeviceMapSize = 0x20000;

constexpr u64 PTE_VALID = 1ull << 0;
constexpr u64 PTE_TABLE = 1ull << 1;
constexpr u64 PTE_AF = 1ull << 10;
constexpr u64 PTE_NG = 1ull << 11;
constexpr u64 PTE_AP_EL0 = 1ull << 6;
constexpr u64 PTE_AP_RO = 1ull << 7;
constexpr u64 PTE_SH_INNER = 3ull << 8;
constexpr u64 PTE_PXN = 1ull << 53;
constexpr u64 PTE_UXN = 1ull << 54;
constexpr u64 PTE_ADDR_MASK = 0x0000'FFFF'FFFF'F000ull;
constexpr u64 DESC_TABLE = PTE_VALID | PTE_TABLE;
constexpr u64 DESC_PAGE = PTE_VALID | PTE_TABLE;

constexpr u64 ATTR_NORMAL = static_cast<u64>(platform::kMairNormal) << 2;
constexpr u64 ATTR_DEVICE = static_cast<u64>(platform::kMairDevice) << 2;

static PhysAddr g_kernel_root = 0;

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

bool ensure_table(u64* table, u32 idx, PhysAddr* out_pa) {
    u64 desc = table[idx];
    if (is_table_desc(desc)) {
        *out_pa = desc & PTE_ADDR_MASK;
        return true;
    }
    if (desc & PTE_VALID) return false;

    PhysAddr child = alloc_page();
    if (!child) return false;
    libk::memset(reinterpret_cast<void*>(child), 0, PAGE_SIZE);
    table[idx] = (child & PTE_ADDR_MASK) | DESC_TABLE;
    *out_pa = child;
    return true;
}

u64 user_page_attrs(u32 rights) {
    const bool writable = (rights & RIGHT_WRITE) != 0;
    const bool executable = (rights & RIGHT_EXEC) != 0;
    u64 attrs = ATTR_NORMAL | PTE_AF | PTE_SH_INNER | PTE_AP_EL0 | PTE_NG | PTE_PXN;
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

u64 kernel_page_attrs(bool device) {
    u64 attrs = (device ? ATTR_DEVICE : ATTR_NORMAL) | PTE_AF | PTE_UXN;
    if (!device) attrs |= PTE_SH_INNER;
    if (device) attrs |= PTE_PXN;
    return attrs;
}

bool map_page_raw(VmSpace& space, VirtAddr va, PhysAddr pa, u64 attrs) {
    if (!space.pgd_phys) return false;
    if ((va & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;

    PhysAddr table_pa = space.pgd_phys;
    for (u32 level = 0; level < 3; ++level) {
        u64* table = table_ptr(table_pa);
        const u32 idx = level_index(va, level);
        if (!ensure_table(table, idx, &table_pa)) return false;
    }

    u64* l3 = table_ptr(table_pa);
    l3[level_index(va, 3)] = (pa & PTE_ADDR_MASK) | DESC_PAGE | attrs;
    return true;
}

bool map_identity_range(VmSpace& space, PhysAddr base, u64 size, bool device) {
    const PhysAddr start = base & ~(static_cast<PhysAddr>(PAGE_SIZE - 1));
    const PhysAddr end = (base + size + PAGE_SIZE - 1) & ~(static_cast<PhysAddr>(PAGE_SIZE - 1));
    const u64 attrs = kernel_page_attrs(device);
    for (PhysAddr pa = start; pa < end; pa += PAGE_SIZE) {
        if (!map_page_raw(space, static_cast<VirtAddr>(pa), pa, attrs)) return false;
    }
    return true;
}

void tlbi_all() {
    asm volatile(
        "dsb ishst\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :
        :
        : "memory");
}

void enable_mmu(PhysAddr root) {
    constexpr u64 mair =
        (0xFFull << (platform::kMairNormal * 8)) |
        (0x00ull << (platform::kMairDevice * 8));
    constexpr u64 tcr =
        (16ull << 0) |      // T0SZ: 48-bit VA
        (1ull << 8) |       // IRGN0: WBWA
        (1ull << 10) |      // ORGN0: WBWA
        (3ull << 12) |      // SH0: inner-shareable
        (0ull << 14) |      // TG0: 4 KiB
        (0ull << 32);       // IPS: 32-bit PA for QEMU virt V0

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

bool range_overlaps(u64 a_base, u64 a_end, u64 b_base, u64 b_end) {
    return a_base < b_end && a_end > b_base;
}

bool arena_range_valid(VirtAddr base, u64 size) {
    if (size == 0) return false;
    if ((base & (PAGE_SIZE - 1)) != 0) return false;
    if ((size & (PAGE_SIZE - 1)) != 0) return false;

    u64 end = 0;
    if (__builtin_add_overflow(base, size, &end)) return false;
    if (end <= base) return false;
    if (base < kUserArenaMin || end > kUserArenaMax) return false;

    const u64 kbase = reinterpret_cast<u64>(&__text_start[0]);
    const u64 kend = reinterpret_cast<u64>(&__kernel_end[0]);
    if (range_overlaps(base, end, kbase, kend)) return false;
    return true;
}

} // namespace

void vm_init() {
    VmSpace kernel{};
    kernel.pid = 0;
    kernel.pgd_phys = alloc_page();
    if (!kernel.pgd_phys) return;
    libk::memset(reinterpret_cast<void*>(kernel.pgd_phys), 0, PAGE_SIZE);

    if (!map_identity_range(kernel, platform::kRamBase, kRamMapSize, false)) return;
    if (!map_identity_range(kernel, platform::kGicDistBase, kDeviceMapSize, true)) return;
    if (!map_identity_range(kernel, platform::kUartBase, PAGE_SIZE, true)) return;
    if (!map_identity_range(kernel, platform::kGuestUartBase, PAGE_SIZE, true)) return;

    g_kernel_root = kernel.pgd_phys;
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
        map_identity_range(s, platform::kRamBase, kRamMapSize, false);
        map_identity_range(s, platform::kGicDistBase, kDeviceMapSize, true);
        map_identity_range(s, platform::kUartBase, PAGE_SIZE, true);
        map_identity_range(s, platform::kGuestUartBase, PAGE_SIZE, true);
    }
    return s;
}

bool vm_map(VmSpace& space, VirtAddr va, PhysAddr pa, u32 rights) {
    if ((rights & RIGHT_WRITE) && (rights & RIGHT_EXEC)) return false;
    const bool ok = map_page_raw(space, va, pa, user_page_attrs(rights));
    if (ok) tlbi_all();
    return ok;
}

bool vm_map_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights) {
    if (size == 0) return false;
    if ((va & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;
    u64 rounded = 0;
    if (__builtin_add_overflow(size, static_cast<u64>(PAGE_SIZE - 1), &rounded)) return false;
    rounded &= ~(static_cast<u64>(PAGE_SIZE - 1));
    u64 end = 0;
    if (__builtin_add_overflow(va, rounded, &end)) return false;
    for (u64 off = 0; off < rounded; off += PAGE_SIZE) {
        if (!vm_map(space, va + off, pa + off, rights)) return false;
    }
    return true;
}

bool vm_map_device_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights) {
    if (size == 0) return false;
    if ((rights & RIGHT_EXEC) != 0) return false;
    if ((va & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;
    u64 rounded = 0;
    if (__builtin_add_overflow(size, static_cast<u64>(PAGE_SIZE - 1), &rounded)) return false;
    rounded &= ~(static_cast<u64>(PAGE_SIZE - 1));
    const u64 attrs = user_device_attrs(rights);
    for (u64 off = 0; off < rounded; off += PAGE_SIZE) {
        if (!map_page_raw(space, va + off, pa + off, attrs)) return false;
    }
    tlbi_all();
    return true;
}

void vm_unmap(VmSpace& space, VirtAddr va) {
    if (!space.pgd_phys || (va & (PAGE_SIZE - 1)) != 0) return;
    PhysAddr table_pa = space.pgd_phys;
    for (u32 level = 0; level < 3; ++level) {
        u64* table = table_ptr(table_pa);
        const u64 desc = table[level_index(va, level)];
        if (!is_table_desc(desc)) return;
        table_pa = desc & PTE_ADDR_MASK;
    }
    table_ptr(table_pa)[level_index(va, 3)] = 0;
    tlbi_all();
}

void vm_space_destroy(VmSpace& space) {
    if (space.pgd_phys) {
        free_page(space.pgd_phys);
        space.pgd_phys = 0;
    }
}

void vm_activate(PhysAddr pgd_phys) {
    if (!pgd_phys) pgd_phys = g_kernel_root;
    asm volatile(
        "dsb ishst\n"
        "msr ttbr0_el1, %0\n"
        "isb\n"
        "tlbi vmalle1\n"
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
    const VirtAddr stop = (end + PAGE_SIZE - 1) & ~(static_cast<VirtAddr>(PAGE_SIZE - 1));
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

bool arena_register(ArenaTable& table, VirtAddr base, u64 size, u32 owner_pid,
                    u32 rights, u32 flags, u32* out_index) {
    if (table.count >= kArenaTableSize) return false;
    if (!arena_range_valid(base, size)) return false;

    const u64 end = base + size;
    for (u32 i = 0; i < table.count; ++i) {
        const Arena& existing = table.entries[i];
        if (existing.size == 0) continue;
        if (range_overlaps(base, end, existing.base_va, existing.base_va + existing.size)) {
            return false;
        }
    }

    const u32 idx = table.count++;
    Arena& a = table.entries[idx];
    a.base_va = base;
    a.size = size;
    a.owner_pid = owner_pid;
    a.rights = rights;
    a.flags = flags;
    libk::memset(a.pad, 0, sizeof(a.pad));

    if (out_index) *out_index = idx;
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
