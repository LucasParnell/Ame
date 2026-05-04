// kernel/arch/arm64/entry.S — _start and early setup.
//
// Entered from QEMU (-kernel) or U-Boot with the AArch64 Linux boot protocol:
//   x0 = physical address of DTB blob
//   x1-x3 = 0
//   EL: EL2 (QEMU default) or EL1
//
// Phase 0 responsibilities (spec §12 Phase 0):
//   1. Save DTB pointer (x0) into x19 across BSS clear.
//   2. Drop from EL2 -> EL1h if needed.
//   3. Disable MMU/caches (clean slate; should already be off at boot).
//   4. Set up early stack.
//   5. Zero BSS.
//   6. Store DTB pointer into g_dtb_phys.
//   7. Branch to kernel_main.
//
// Phase 1 will add: MMU + page tables, VBAR_EL1 install, EL1 SP_EL0 setup.

.section .text.entry, "ax"
.global _start
_start:
    // Save DTB pointer in callee-saved register across BSS clear.
    mov     x19, x0
    mrs     x20, CurrentEL
    lsr     x20, x20, #2

    // Drop from EL2 to EL1h if entered at EL2.
    cmp     x20, #2
    b.ne    1f

    // EL2 -> EL1: configure HCR/SCTLR/SPSR, then ERET.
    adrp    x0, __el2_stack_top
    add     x0, x0, :lo12:__el2_stack_top
    mov     sp, x0
    adrp    x0, el2_vectors
    add     x0, x0, :lo12:el2_vectors
    msr     VBAR_EL2, x0
    mov     x0, #(1 << 31)              // HCR_EL2.RW = 1 (EL1 is AArch64)
    msr     HCR_EL2, x0
    mov     x0, #0x33ff
    msr     CPTR_EL2, x0                // disable trapping of FP/SIMD at EL2
    mov     x0, #3
    msr     CNTHCTL_EL2, x0
    msr     CNTVOFF_EL2, xzr
    mov     x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     SCTLR_EL1, x0               // EL1 reset value (MMU off)
    mov     x0, #0x3c5                  // DAIF masked, EL1h
    msr     SPSR_EL2, x0
    adr     x0, 1f
    msr     ELR_EL2, x0
    eret
1:
    // Ensure MMU and caches are disabled (paranoia).
    mrs     x0, SCTLR_EL1
    bic     x0, x0, #(1 << 0)           // M (MMU)
    bic     x0, x0, #(1 << 2)           // C (data cache)
    bic     x0, x0, #(1 << 12)          // I (instruction cache)
    msr     SCTLR_EL1, x0
    isb

    // Set up the boot stack.
    adrp    x0, __boot_stack_top
    add     x0, x0, :lo12:__boot_stack_top
    mov     sp, x0

    // Zero BSS [__bss_start, __bss_end).
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
2:  cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b
3:

    // Save DTB pointer now that BSS is clean.
    adrp    x0, g_dtb_phys
    add     x0, x0, :lo12:g_dtb_phys
    str     x19, [x0]

    adrp    x0, g_boot_el
    add     x0, x0, :lo12:g_boot_el
    str     x20, [x0]

    // Into C++.
    bl      kernel_main

    // Should never return; halt if it does.
4:  wfe
    b       4b

.global secondary_start
secondary_start:
    // PSCI CPU_ON enters here with x0 = context_id (logical CPU id).
    mov     x19, x0

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    5f

    adrp    x1, __el2_stack_top
    add     x1, x1, :lo12:__el2_stack_top
    mov     sp, x1
    adrp    x1, el2_vectors
    add     x1, x1, :lo12:el2_vectors
    msr     VBAR_EL2, x1
    mov     x0, #(1 << 31)
    msr     HCR_EL2, x0
    mov     x0, #0x33ff
    msr     CPTR_EL2, x0
    mov     x0, #3
    msr     CNTHCTL_EL2, x0
    msr     CNTVOFF_EL2, xzr
    mov     x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     SCTLR_EL1, x0
    mov     x0, #0x3c5
    msr     SPSR_EL2, x0
    adr     x0, 5f
    msr     ELR_EL2, x0
    eret

5:
    mrs     x0, SCTLR_EL1
    bic     x0, x0, #(1 << 0)
    bic     x0, x0, #(1 << 2)
    bic     x0, x0, #(1 << 12)
    msr     SCTLR_EL1, x0
    isb

    adrp    x0, g_cpu_stacks
    add     x0, x0, :lo12:g_cpu_stacks
    add     x1, x19, #1
    lsl     x1, x1, #14
    add     x0, x0, x1
    mov     sp, x0

    mov     x0, x19
    bl      secondary_kernel_main

6:  wfe
    b       6b

// DTB physical address (set above, read by kernel_main).
.section .data
.align 8
.global g_dtb_phys
g_dtb_phys:
    .quad 0
.global g_boot_el
g_boot_el:
    .quad 0

// Boot stack — 16 KiB, 16-byte aligned bottom, top is what we load into SP.
.section .bss
.align 4
__boot_stack_bottom:
    .skip 16384
.global __boot_stack_top
__boot_stack_top:

.align 14
.global g_cpu_stacks
g_cpu_stacks:
    .skip 65536
