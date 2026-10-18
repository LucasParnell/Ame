// Minimal Linux Image entry for testing the production cache scope in QEMU.
.section .text.entry, "ax"
.global _start
_start:
    b 1f
    .long 0
    .quad 0x80000
    .quad __kernel_end - _start
    .quad 0, 0, 0, 0
    .long 0x644d5241, 0
1:  msr daifset, #15
    mov x19, x0
    mrs x1, CurrentEL
    cmp x1, #8
    b.ne 2f
    mov x1, #(1 << 31)
    msr hcr_el2, x1
    mov x1, #3
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    mov x1, #0x3c5
    msr spsr_el2, x1
    adr x1, 2f
    msr elr_el2, x1
    eret
2:  msr spsel, #1
    ldr x1, =test_stack_top
    mov sp, x1
    ldr x1, =__bss_start
    ldr x2, =__bss_end
3:  cmp x1, x2
    b.hs 4f
    str xzr, [x1], #8
    b 3b
4:  mrs x1, sctlr_el1
    bic x1, x1, #1
    bic x1, x1, #4
    bic x1, x1, #2
    orr x1, x1, #(1 << 12)
    msr sctlr_el1, x1
    isb
    mov x0, x19
    bl boot_cache_test
5:  wfe
    b 5b
.section .bss
.balign 16
    .skip 16384
test_stack_top:
