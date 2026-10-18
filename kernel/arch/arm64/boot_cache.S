// Restore MMU-off boot after the temporary DT cache scope. This must stay
// stackless: a spill after cleaning the stack would lose a dirty cache line
// when SCTLR.C is cleared. Only caller-saved registers are used.
.text
.balign 16
.global boot_cache_restore
.type boot_cache_restore, %function
boot_cache_restore:
    ldp x9, x10, [x0]       // saved SCTLR, TCR
    ldp x11, x12, [x0, #16] // saved MAIR, TTBR0
    ldr x13, [x0, #32]      // saved TTBR1
    mrs x14, ctr_el0
    ubfx x14, x14, #16, #4
    mov x15, #4
    lsl x14, x15, x14       // actual D-cache line size
    sub x15, x14, #1
    bic x1, x1, x15
1:  dc civac, x1            // publish kernel/HwInfo/stack to PoC
    add x1, x1, x14
    cmp x1, x2
    b.lo 1b
    dsb sy
    msr sctlr_el1, x9       // original M=C=0, retain original I state
    isb
    // DC IVAC requires write permission even at EL1. The DTB's temporary
    // mapping is intentionally read-only, so invalidate only after restoring
    // physical, MMU-off access. QEMU does not model this permission check.
    // The blob has no dirty lines: no writer or writable alias existed.
    bic x3, x3, x15
2:  dc ivac, x3
    add x3, x3, x14
    cmp x3, x4
    b.lo 2b
    dsb sy
    tlbi vmalle1
    dsb sy
    msr tcr_el1, x10
    msr mair_el1, x11
    msr ttbr0_el1, x12
    msr ttbr1_el1, x13
    isb
    ret
.size boot_cache_restore, . - boot_cache_restore
