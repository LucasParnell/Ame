// boot/mmu/mmu_enable.S
//
// Low-level assembly stubs for MMU enable sequence.
//
// Must be position-independent / PC-relative only in the critical section
// between writing SCTLR_EL1.M and the ISB that makes the MMU active.
//
// TODO:
//   - mmu_write_regs(mair, tcr, ttbr0, ttbr1)
//       write MAIR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1, DSB ISB
//   - mmu_enable_trampoline()
//       set SCTLR_EL1.M | I | C, ISB, branch to virtual address

.section .text.mmu, "ax"
