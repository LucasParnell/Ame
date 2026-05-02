// boot/mmu/page_table.cpp
//
// Early page-table construction (4KB pages, 48-bit VA, 3-level walk).
//
// Two mappings are created before the MMU turns on:
//
//   1. Identity map  (phys == virt) covering the boot image + page tables
//      — needed so the PC-relative instructions keep working during the
//        "MMU enable" trampoline in mmu_enable.S
//
//   2. Kernel window  phys → KERNEL_VA_BASE + phys
//      — the permanent mapping the kernel will run from
//
// Page tables are allocated from boot::memory::early_alloc_pages().
//
// AArch64 descriptor format (stage 1, EL1):
//   Bits [1:0]  = 0b11  (table / page descriptor)
//   Bits [47:12] = output address
//   AttrIdx     = index into MAIR_EL1
//   AP, SH, AF  = access / shareability / access-flag bits

#include "boot/mmu.hpp"
#include "boot/memory.hpp"

namespace boot::mmu {

void setup(uint64_t /*id_map_phys*/, uint64_t /*ram_phys*/, uint64_t /*ram_size*/) {
    // TODO: allocate PGD/PUD/PMD/PTE pages via early_alloc_pages()
    // TODO: fill identity mapping
    // TODO: fill kernel-window mapping
    // TODO: write MAIR_EL1, TCR_EL1, SCTLR_EL1 (via mmu_enable.S helper)
    // TODO: set TTBR0_EL1 (identity) and TTBR1_EL1 (kernel window)
    // TODO: call asm stub to issue ISB + enable MMU bit in SCTLR_EL1
}

uint64_t post_enable_fixup(uint64_t /*phys_sp*/) {
    // TODO: translate phys_sp to virtual address and return it
    return 0;
}

} // namespace boot::mmu
