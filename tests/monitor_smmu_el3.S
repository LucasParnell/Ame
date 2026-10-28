// Bare EL3 harness: execute the production callback and recovery vector with
// real translation faults. QEMU virt secure boot enters flash at EL3.
.section .text.start, "ax"
.global _start, stock_unhandled
_start:
    msr daifset, #15
    mov x0, #0x40010000
    mov sp, x0
    msr spsel, #0
    mov sp, x0
    ldr x0, =vectors
    msr vbar_el3, x0
    // Three zeroed page-table pages in RAM.
    mov x0, #0x40100000
    mov x1, #0x3000
1:  str xzr, [x0], #8
    subs x1, x1, #8
    b.ne 1b
    mov x0, #0x40100000
    ldr x1, =0x40101003
    str x1, [x0]                // L1[0] -> L2, only flash's first 2 MiB
    ldr x1, =0x40000701
    str x1, [x0, #8]            // L1[1] -> RAM identity, 1 GiB block
    ldr x1, =0x40102003
    str x1, [x0, #24]           // L1[3] -> L2 for fixed candidate VA
    ldr x0, =0x40101000
    mov x1, #0x701
    str x1, [x0]                // flash block
    ldr x0, =0x40102000
    ldr x1, =0x40000701
    str x1, [x0, #2608]         // candidate 0xe8c00000 block -> RAM
    mov x0, #0x401c0000
    mov w1, #0x60000000
    str w1, [x0, #0x20]
    mov w1, #8
    str w1, [x0, #0x24]
    mov w1, #0x1032
    str w1, [x0, #0x28]
    mov x0, #0x40100000
    msr ttbr0_el3, x0
    mov x0, #0xff
    msr mair_el3, x0
    ldr x0, =0x80803520        // RES1, 4 GiB VA, WB inner-shareable, 4 GiB PA
    msr tcr_el3, x0
    dsb sy
    isb
    mrs x0, sctlr_el3
    orr x0, x0, #1
    msr sctlr_el3, x0
    isb
    // Successful reads must use 32-bit accesses and reject upper-bit aliases.
    mov x0, #1
    bl invoke
    mov w2, #4
    movk w2, #0x414d, lsl #16
    cmp w0, w2
    b.ne failed
    mov x0, #2
    bl invoke
    mov w2, #0x60000000
    cmp w0, w2
    b.ne failed
    mov x0, #3
    bl invoke
    cmp w0, #8
    b.ne failed
    mov x0, #4
    bl invoke
    mov w2, #0x1032
    cmp w0, w2
    b.ne failed
    mov x0, #1
    movk x0, #1, lsl #32
    bl invoke
    cmn w0, #1
    b.ne failed
    mov x0, #1
    mov x1, #0
    bl monitor_probe
    cmn w0, #1
    b.ne failed
    // Repeated real aborts must return to the callback, then its caller.
    mov x19, #4
2:  mov x0, #5
    bl invoke
    ldr w2, =0x96000006        // same-EL data abort, translation fault level 2
    cmp w0, w2
    b.ne failed
    subs x19, x19, #1
    b.ne 2b
    mov x0, #3
    bl invoke
    cmp w0, #8
    b.ne failed
#ifdef TEST_UNRELATED
    mov x4, #0x20000000
    ldr w0, [x4]              // same FAR and EC, different PC: must stay fatal
    b failed
#endif
    adr x1, passed_text
    b success
invoke:
    mov w1, #0x4552
    movk w1, #0x414d, lsl #16
    b monitor_probe
stock_unhandled:
#ifdef TEST_UNRELATED
    adr x1, unrelated_text
    b success
#endif
failed:
    adr x1, failed_text
    mov x20, #1
    b finish
success:
    mov x20, #0
finish:
    mov x0, #4
    hlt #0xf000
    sub sp, sp, #16
    ldr x0, =0x20026
    stp x0, x20, [sp]
    mov x1, sp
    mov x0, #0x20
    hlt #0xf000
    b .
passed_text: .asciz "secure SMMU reads and EL3 fault recovery passed\n"
failed_text: .asciz "EL3 probe test FAILED\n"
unrelated_text: .asciz "unrelated EL3 fault kept fatal\n"
    .ltorg
