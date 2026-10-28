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
    // Keep recovery-sensitive registers before assigning diagnostic scratch.
    sub sp, sp, #32
    stp x4, x9, [sp, #0]
    stp x10, x11, [sp, #16]
    mov x4, #anne_vector_index
    b anne_diag_fault_entry
    .balign 128
    .set anne_vector_index, anne_vector_index + 1
    .endr

.global anne_diag_fault_entry
anne_diag_fault_entry:
    msr daifset, #15
    // Fault-tolerant probe hook: when a probe read is armed on this CPU, data
    // aborts are recorded and the faulting load is skipped (+4) so execution
    // resumes; everything else continues on the fatal path below.
    adrp x9, anne_diag_probe_state
    add  x9, x9, :lo12:anne_diag_probe_state
    ldr  x10, [x9]
    cbnz x10, anne_diag_probe_take
anne_diag_fault_fatal:
    // All board entry callers saved 32 bytes before setting the vector index.
    add sp, sp, #32
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

// ---- fault-tolerant probe resume path (see anne_diag_probe_read) ----
anne_diag_probe_take:
    sub sp, sp, #64
    stp x9,  x10, [sp, #0]
    stp x11, x12, [sp, #16]
    stp x13, x14, [sp, #32]
    str x15,      [sp, #48]
    adrp x15, anne_diag_probe_state
    add  x15, x15, :lo12:anne_diag_probe_state
    ldr  x10, [x15, #40]
    mrs  x11, tpidr_el1
    cmp  x11, #MX_HOST_MAX_CPUS
    b.lo 60f
    mov  x11, xzr
60: cmp  x10, x11
    b.eq 66f
    ldp x11, x12, [sp, #16]
    ldp x13, x14, [sp, #32]
    ldr x15,      [sp, #48]
    ldp x9,  x10, [sp], #64
    b anne_diag_fault_fatal
66: mrs  x10, CurrentEL
    cmp  x10, #8
    b.eq 61f
    mrs  x11, esr_el1
    mrs  x12, elr_el1
    mrs  x13, far_el1
    b 62f
61: mrs  x11, esr_el2
    mrs  x12, elr_el2
    mrs  x13, far_el2
62: ubfx x14, x11, #26, #6
    cmp  x14, #0x24
    b.eq 63f
    cmp  x14, #0x25
    b.eq 63f
    ldp x11, x12, [sp, #16]
    ldp x13, x14, [sp, #32]
    ldr x15,      [sp, #48]
    ldp x9,  x10, [sp], #64
    b anne_diag_fault_fatal
63: str  x11, [x15, #16]
    str  x12, [x15, #24]
    str  x13, [x15, #32]
    add  x12, x12, #4
    cmp  x10, #8
    b.eq 64f
    msr  elr_el1, x12
    b 65f
64: msr  elr_el2, x12
65: ldr  x9, [x15, #8]
    add  x9, x9, #1
    str  x9, [x15, #8]
    ldp x11, x12, [sp, #16]
    ldp x13, x14, [sp, #32]
    ldr x15,      [sp, #48]
    ldp x9,  x10, [sp], #64
    // The entry hook used x9/x10 and the vector slot used x4. Restore their
    // pre-exception values too, not the diagnostic scratch saved afterwards.
    ldp x4, x9, [sp, #0]
    ldp x10, x11, [sp, #16]
    add sp, sp, #32
    eret

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
