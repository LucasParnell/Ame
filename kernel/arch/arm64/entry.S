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
#include "midix/abi/cpu_limits.h"
.global _start
_start:
    // AArch64 Linux Image header. QEMU -kernel and U-Boot both enter at the
    // first instruction, so keep this as a branch into the real entry path.
    b       .Lprimary_entry
    .long   0                           // reserved
    .quad   ANKO_IMAGE_TEXT_OFFSET      // text_offset from DRAM base
    .quad   __kernel_end - _start       // image_size
    .quad   0                           // flags
    .quad   0                           // reserved
    .quad   0                           // reserved
    .quad   0                           // reserved
    .long   0x644d5241                  // "ARM\x64"
    .long   0                           // reserved

.Lprimary_entry:
    msr     DAIFSet, #15
    msr     SPSel, #1
    msr     TPIDR_EL1, xzr
    // Save DTB pointer in callee-saved register across BSS clear.
    mov     x19, x0
    mrs     x20, CurrentEL
    lsr     x20, x20, #2

#ifdef ANKO_BOARD_HUAWEI_ANNE
    cmp     x20, #1
    b.eq    .Lanne_el1_state
    cmp     x20, #2
    b.ne    .Lanne_unsupported
    mrs     x21, SCTLR_EL2
    b       .Lanne_state_ready
.Lanne_el1_state:
    mrs     x21, SCTLR_EL1
.Lanne_state_ready:
    tst     x21, #1                   // Require the verified MMU/D-cache-off ABI.
    b.ne    .Lanne_unsupported
    tst     x21, #4
    b.ne    .Lanne_unsupported
    adrp    x0, __boot_stack_top
    add     x0, x0, :lo12:__boot_stack_top
    mov     sp, x0
    bl      anne_diag_install_early_vectors
    mov     x0, x20
    mov     x1, x19
    mov     x2, x21
    bl      anne_diag_init            // .data state survives the following BSS clear.
    b       .Lanne_entry_ready
.Lanne_unsupported:
    wfe
    b       .Lanne_unsupported
.Lanne_entry_ready:
#endif

    // -----------------------------------------------------------------------
    // (T210) No framebuffer paint here. The previous sign-of-life wrote a full
    // 1280x720 frame at a *hardcoded* 0xC0000000 before anything was validated:
    // that address is a guess, and if it is not the live DC0 window it scribbles
    // ~3.7 MiB over whatever happens to be there. The real address is only known
    // after platform::probe() reads the TegraDC0 window registers, so the first
    // framebuffer paint is now probe()'s (green) and kernel_main's (blue).
    // -----------------------------------------------------------------------

    // Drop from EL2 to EL1h if entered at EL2.
    cmp     x20, #2
    b.ne    1f

    // EL2 -> EL1: configure HCR/SCTLR/SPSR, then ERET.
    // Per-CPU EL2 stack.  EL2 can be entered concurrently on SMP guests,
    // especially while Dango vCPU runner threads are active.
    adrp    x0, g_el2_stacks
    add     x0, x0, :lo12:g_el2_stacks
    mov     x1, xzr
    msr     TPIDR_EL2, xzr
0:
    add     x1, x1, #1
    lsl     x1, x1, #14
    add     x0, x0, x1
    mov     sp, x0
#ifndef ANKO_BOARD_HUAWEI_ANNE
    adrp    x0, el2_vectors
    add     x0, x0, :lo12:el2_vectors
    msr     VBAR_EL2, x0
#endif
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
#ifdef ANKO_BOARD_HUAWEI_ANNE
    bl      anne_diag_install_early_vectors
#endif
    // Ensure MMU, caches, and alignment check are disabled (paranoia).
    // Clear bit 1 (A) too — U-Boot may have set alignment checking, which
    // would cause faults on the unaligned stack accesses we make before MMU.
    mrs     x0, SCTLR_EL1
    bic     x0, x0, #(1 << 0)           // M (MMU)
    bic     x0, x0, #(1 << 1)           // A (alignment check)
    bic     x0, x0, #(1 << 2)           // C (data cache)
    bic     x0, x0, #(1 << 12)          // I (instruction cache)
    msr     SCTLR_EL1, x0
    isb

    // Set up the boot stack.
    adrp    x0, __boot_stack_top
    add     x0, x0, :lo12:__boot_stack_top
    mov     sp, x0

#ifdef ANKO_BOARD_HUAWEI_ANNE
    mov     x0, #2                    // BssBegin
    bl      anne_diag_stage
#endif

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

#ifdef ANKO_BOARD_HUAWEI_ANNE
    mov     x0, #3                    // BssDone
    bl      anne_diag_stage
    mov     x0, #4                    // ConstructorsBegin
    bl      anne_diag_stage
#else
    // Catch faults in constructors and board-specific early logging as well.
    adrp    x0, vectors
    add     x0, x0, :lo12:vectors
    msr     VBAR_EL1, x0
    isb
#endif

    // Save DTB pointer now that BSS is clean.
    adrp    x0, g_dtb_phys
    add     x0, x0, :lo12:g_dtb_phys
    str     x19, [x0]

    adrp    x0, g_boot_el
    add     x0, x0, :lo12:g_boot_el
    str     x20, [x0]

    // Run global constructors (.init_array) so non-trivial globals like g_hw
    // are initialised before kernel_main touches them.
    adrp    x21, __init_array_start
    add     x21, x21, :lo12:__init_array_start
    adrp    x22, __init_array_end
    add     x22, x22, :lo12:__init_array_end
4:  cmp     x21, x22
    b.hs    5f
#ifdef ANKO_BOARD_HUAWEI_ANNE
    ldr     x23, [x21], #8
    cbz     x23, 4b
    mov     x0, x23
    bl      anne_diag_constructor
    blr     x23
#else
    ldr     x0, [x21], #8
    cbz     x0, 4b          // skip NULL slots (alignment padding)
    blr     x0
#endif
    b       4b
5:
#ifdef ANKO_BOARD_HUAWEI_ANNE
    mov     x0, #5                    // ConstructorsDone
    bl      anne_diag_stage
#endif

    // Into C++.
    bl      kernel_main

    // Should never return; halt if it does.
4:  wfe
    b       4b

.global secondary_start
secondary_start:
    msr     DAIFSet, #15
    msr     SPSel, #1
    // Resolve the full physical affinity; legacy firmware need not retain x0.
    mrs     x1, MPIDR_EL1
    movz    x2, #0xffff
    movk    x2, #0x00ff, lsl #16
    movk    x2, #0x00ff, lsl #32
    and     x1, x1, x2
    adrp    x2, g_cpu_mpidrs
    add     x2, x2, :lo12:g_cpu_mpidrs
    mov     x19, xzr
.Lsecondary_find:
    ldr     x3, [x2, x19, lsl #3]
    cmp     x3, x1
    b.eq    .Lsecondary_found
    add     x19, x19, #1
    cmp     x19, #MX_HOST_MAX_CPUS
    b.lo    .Lsecondary_find
    b       6f
.Lsecondary_found:
    msr     TPIDR_EL1, x19

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    5f

    // Per-CPU EL2 stack for secondary CPUs too.  Do not share EL2 SP.
    msr     TPIDR_EL2, x19
    adrp    x1, g_el2_stacks
    add     x1, x1, :lo12:g_el2_stacks
    mov     x2, x19
7:
    add     x2, x2, #1
    lsl     x2, x2, #14
    add     x1, x1, x2
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
    bic     x0, x0, #(1 << 1)
    bic     x0, x0, #(1 << 2)
    bic     x0, x0, #(1 << 12)
    msr     SCTLR_EL1, x0
    isb
    ic      iallu
    dsb     sy
    isb

    adrp    x0, g_cpu_stacks
    add     x0, x0, :lo12:g_cpu_stacks
    add     x1, x19, #1
    lsl     x1, x1, #14
    add     x0, x0, x1
    mov     sp, x0

    mov     x0, x19
#ifdef ANKO_BOARD_HUAWEI_ANNE
    bl      anne_diag_install_early_vectors
    mov     x0, x19
#endif
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
    .skip (MX_HOST_MAX_CPUS * 16384)
