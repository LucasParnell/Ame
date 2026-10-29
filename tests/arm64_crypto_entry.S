.section .text.entry, "ax"
.global _start
_start:
    ldr x0, =__stack_top
    mov sp, x0
    ldr x0, =__bss_start
    ldr x1, =__bss_end
1:  cmp x0, x1
    b.hs 2f
    stp xzr, xzr, [x0], #16
    b 1b
2:  mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb
    // Exercise byte-structure loads with strict alignment enabled too.
    mrs x0, sctlr_el1
    orr x0, x0, #2
    msr sctlr_el1, x0
    isb
    bl crypto_probe
    mov x2, x0
    mov x0, #0x20
    ldr x9, =0x20026
    stp x9, x2, [sp, #-16]!
    mov x1, sp
    hlt #0xf000
3:  wfe
    b 3b

// Seed every vector register, close FP access, call the production kernel
// ownership adapter, then check CPACR/DAIF and expose all vectors to C++.
.text
.global crypto_state_probe
.type crypto_state_probe, %function
crypto_state_probe:
    stp x29, x30, [sp, #-48]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x21, x4
    mrs x19, cpacr_el1
    mrs x20, daif
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    movi v\n\().16b, #0x5a
.endr
    and x22, x19, #~(3 << 20)
    msr cpacr_el1, x22
    isb
    bl kernel_sha256_blocks
    mrs x9, cpacr_el1
    cmp x9, x22
    cset w10, ne
    mrs x9, daif
    cmp x9, x20
    cset w11, ne
    orr w10, w10, w11
    msr cpacr_el1, x19
    isb
.irp n,0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30
    str q\n, [x21], #16
    // The odd register name needs a separate pass below.
.endr
.irp n,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31
    str q\n, [x21], #16
.endr
    mov w0, w10
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    ret
.size crypto_state_probe, . - crypto_state_probe
