.section .text.entry, "ax"
.global _start
_start:
    b start
    .long 0
    .quad 0x80000
    .quad image_end - _start
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .long 0x644d5241
    .long 0
start:
    msr daifset, #15
    mrs x19, CurrentEL
    lsr x19, x19, #2
    cmp x19, #1
    b.eq el1
    cmp x19, #2
    b.ne park
    mrs x1, sctlr_el2
    b check
el1:
    mrs x1, sctlr_el1
check:
    // Never turn off an unknown mapping: require the boot protocol's MMU-off ABI.
    tst x1, #1
    b.ne park
    tst x1, #4
    b.ne park
    msr spsel, #1
    adrp x1, stack_end
    add x1, x1, :lo12:stack_end
    mov sp, x1
    mov x0, x19
    bl anne_entry_probe
park:
    wfe
    b park
