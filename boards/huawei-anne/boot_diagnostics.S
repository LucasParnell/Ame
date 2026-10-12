.section .text.anne_diag, "ax"
#include "midix/abi/cpu_limits.h"
.global anne_diag_install_early_vectors
anne_diag_install_early_vectors:
    adr x9, anne_diag_vectors
    mrs x10, CurrentEL
    cmp x10, #8
    b.eq 1f
    msr vbar_el1, x9
    b 2f
1:  msr vbar_el2, x9
2:  isb
    ret

.balign 2048
anne_diag_vectors:
    .set anne_vector_index, 0
    .rept 16
    mov x4, #anne_vector_index
    b anne_diag_fault_entry
    .balign 128
    .set anne_vector_index, anne_vector_index + 1
    .endr

.global anne_diag_fault_entry
anne_diag_fault_entry:
    msr daifset, #15
    adrp x9, anne_diag_fault_active
    add x9, x9, :lo12:anne_diag_fault_active
    mrs x11, tpidr_el1
    cmp x11, #MX_HOST_MAX_CPUS
    b.lo 9f
    mov x11, xzr
9:  add x9, x9, x11, lsl #2
    ldr w10, [x9]
    cbnz w10, anne_diag_reset
    mov w10, #1
    str w10, [x9]
    mov x3, sp
    adrp x9, anne_diag_fault_stacks
    add x9, x9, :lo12:anne_diag_fault_stacks
    add x11, x11, #1
    add x9, x9, x11, lsl #14
    mov sp, x9
    mrs x9, CurrentEL
    cmp x9, #8
    b.eq 3f
    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    b 4f
3:  mrs x0, esr_el2
    mrs x1, elr_el2
    mrs x2, far_el2
4:  bl anne_diag_early_fault

.global anne_diag_reset
anne_diag_reset:
    msr daifset, #15
#ifdef ANKO_ANNE_DIAG_TEST
    movz x9, #0
    movk x9, #0x4710, lsl #16
#else
    movz x9, #0xa000
    movk x9, #0xfff0, lsl #16
#endif
    add x9, x9, #0x510
5:  ldr w10, [x9]
    orr w10, w10, #4
    str w10, [x9]
    bic w10, w10, #4
    str w10, [x9]
    dsb sy
    // A nested fault bypasses counter waits and logging entirely.
    b 5b

.section .bss.anne_diag_stack, "aw", %nobits
.balign 16
anne_diag_fault_stacks:
    .skip (MX_HOST_MAX_CPUS * 16384)
anne_diag_fault_stack_end:
