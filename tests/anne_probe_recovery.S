// Execute the production early and runtime vector recovery paths at EL1.
.section .text.start, "ax"
.global _start
_start:
    ldr x0, =stack_end
    mov sp, x0
    msr daifset, #15
    msr tpidr_el1, xzr
    ldr x0, =page_table
    ldr x1, =0x40000701 // identity 1GiB block at 0x40000000, AF, inner-shareable
    str x1, [x0, #8]
    msr ttbr0_el1, x0
    mov x0, #0xff
    msr mair_el1, x0
    ldr x0, =0x803519 // 39-bit VA, 4KiB, WBWA walks; TTBR1 disabled
    msr tcr_el1, x0
    dsb sy
    tlbi vmalle1
    dsb sy
    isb
    mrs x0, sctlr_el1
    orr x0, x0, #1
    msr sctlr_el1, x0
    isb
    bl anne_diag_install_early_vectors
    bl exercise
    ldr x0, =vectors
    msr vbar_el1, x0
    isb
    bl exercise
    ldr x0, =anne_diag_probe_state
    ldr x0, [x0, #8]
    cmp x0, #2
    b.ne failed
    ldr x1, =exit_ok
    b quit
exercise:
    ldr x0, =anne_diag_probe_state
    mov x1, #1
    str x1, [x0]
    str xzr, [x0, #40]
    mov x4, #0x4444
    mov x9, #0x9999
    mov x10, #0xaaaa
    mov x11, #0xbbbb
    mov x12, #0xcccc
    mov x13, #0xdddd
    mov x14, #0xeeee
    mov x15, #0xffff
    mov x0, #0x1000 // deliberately unmapped; no peripheral access
    ldr x11, [x0]
    // All these registers were live across the fault, including the load dest.
    mov x16, #0x4444
    cmp x4, x16
    b.ne failed
    mov x16, #0x9999
    cmp x9, x16
    b.ne failed
    mov x16, #0xaaaa
    cmp x10, x16
    b.ne failed
    mov x16, #0xbbbb
    cmp x11, x16
    b.ne failed
    mov x16, #0xcccc
    cmp x12, x16
    b.ne failed
    mov x16, #0xdddd
    cmp x13, x16
    b.ne failed
    mov x16, #0xeeee
    cmp x14, x16
    b.ne failed
    mov x16, #0xffff
    cmp x15, x16
    b.ne failed
    ret
failed:
    ldr x1, =exit_fail
quit:
    mov x0, #0x20 // SYS_EXIT_EXTENDED
    hlt #0xf000
    b .
.global anne_diag_early_fault
anne_diag_early_fault:
    b failed
// Unused generic-vector destinations must never run in this board fixture.
.global syscall_entry, irq_entry, irq_entry_kernel, kernel_sync_fault, kernel_unexpected_exception
syscall_entry:
irq_entry:
irq_entry_kernel:
kernel_sync_fault:
kernel_unexpected_exception:
    b failed
.section .data
.balign 8
exit_ok: .quad 0x20026, 0
exit_fail: .quad 0x20026, 1
.global anne_diag_probe_state, anne_diag_fault_active
anne_diag_probe_state: .zero 48
anne_diag_fault_active: .zero 32
.section .bss
.balign 4096
page_table: .skip 4096
.balign 16
stack: .skip 16384
stack_end:
