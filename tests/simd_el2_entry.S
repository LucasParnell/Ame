// Minimal Linux Image entry running the production EL2 monitor.
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
    mrs x0, CurrentEL
    cmp x0, #8
    b.ne 9f
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    msr cptr_el2, xzr
    msr tpidr_el2, xzr
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    ldr x0, =g_el2_stacks
    add x0, x0, #16384
    mov sp, x0
    ldr x0, =el2_vectors
    msr vbar_el2, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    adr x0, 2f
    msr elr_el2, x0
    eret
2:  msr spsel, #1
    ldr x0, =test_stack_top
    mov sp, x0
    ldr x0, =__bss_start
    ldr x1, =__bss_end
3:  cmp x0, x1
    b.hs 4f
    str xzr, [x0], #8
    b 3b
4:  bl simd_el2_test
9:  wfe
    b 9b

.global simd_guest_no_fp
simd_guest_no_fp:
    hvc #0
    b simd_guest_no_fp

.global timer_guest_disable
timer_guest_disable:
    msr cntp_ctl_el0, xzr
    hvc #0
    b timer_guest_disable

.global timer_guest_busy
timer_guest_busy:
    b timer_guest_busy

.section .bss
.balign 16
    .skip 16384
test_stack_top:
