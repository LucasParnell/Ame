// Final stock LK handoff, before BL31 executes. Preserve x0/x1/x2 and LR.
.section .text, "ax"
.global monitor_preload
monitor_preload:
    ldr x9, =0x03000000
    ldr x10, =0x35e00000
    ldr x11, =monitor_bytes
1:
    ldp x12, x13, [x9], #16
    stp x12, x13, [x10], #16
    subs x11, x11, #16
    b.ne 1b
    // Use the actual D/I cache line sizes, independent of inherited SCTLR.
    mrs x9, ctr_el0
    ubfx x10, x9, #16, #4
    mov x11, #4
    lsl x10, x11, x10
    and x9, x9, #15
    lsl x9, x11, x9
    ldr x11, =0x35e00000
    ldr x12, =0x35e00000 + monitor_bytes
2:
    dc cvau, x11
    add x11, x11, x10
    cmp x11, x12
    b.lo 2b
    dsb sy
    ldr x11, =0x35e00000
3:
    ic ivau, x11
    add x11, x11, x9
    cmp x11, x12
    b.lo 3b
    dsb sy
    isb
    br x2
