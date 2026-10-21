// Synthetic firmware state before the production Anne entry path.
// x0 (the DTB) is preserved. EL2 translation and data caching stay disabled.
.text
.global _start
_start:
#ifdef ANNE_HANDOFF_EL1
    // EL2 exists but firmware hands off at EL1: Ame must not attempt HVC.
    mov     x9, #(1 << 31)
    msr     HCR_EL2, x9
    mov     x9, #0x33ff
    msr     CPTR_EL2, x9
    mov     x9, #3
    msr     CNTHCTL_EL2, x9
    msr     CNTVOFF_EL2, xzr
    mov     x9, #0x0800
    movk    x9, #0x30d0, lsl #16
    msr     SCTLR_EL1, x9
    mov     x9, #0x3c5
    msr     SPSR_EL2, x9
    ldr     x9, =0x40080040
    msr     ELR_EL2, x9
    isb
    eret
#else
    mov     x9, #(1 << 31)
    orr     x9, x9, #(1 << 27)         // Trap EL1 VM register accesses
    orr     x9, x9, #(1 << 18)         // Trap EL1 CPU feature reads
    msr     HCR_EL2, x9
    mov     x9, #0x33ff
    orr     x9, x9, #(1 << 31)         // Trap CPACR_EL1 accesses
    orr     x9, x9, #(1 << 10)         // Trap FP/SIMD
    msr     CPTR_EL2, x9
    msr     CNTHCTL_EL2, xzr
    mov     x9, #0x1234
    msr     CNTVOFF_EL2, x9
    msr     VPIDR_EL2, x9
    msr     VMPIDR_EL2, x9
    msr     SP_EL1, xzr
    isb
    // The Image header occupies 64 bytes; enter the production code after it.
    ldr     x9, =0x40080040
    br      x9
#endif
