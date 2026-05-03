// kernel/arch/arm64/context.S — context_switch(prev, next)
//
// Spec ref: §5.4 Threads and Scheduler
//
// Signature (C++ mangling avoided with extern "C"):
//   void context_switch(CpuContext* prev, CpuContext* next)
//
// CpuContext layout (must match sched.h exactly, field order = offset):
//   +0x00  x19-x28  (10 × 8 = 80 bytes)
//   +0x50  fp  (x29)
//   +0x58  lr  (x30) — we return into the new thread via this
//   +0x60  sp
//
// Save callee-saved regs + SP into prev->ctx.
// Load them from next->ctx.
// Return — which now executes in the context of 'next'.

.section .text, "ax"
.global context_switch
context_switch:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]    // fp, lr
    mov x9, sp
    str x9,       [x0, #96]    // sp

    ldr x9,       [x1, #96]
    mov sp, x9
    ldp x29, x30, [x1, #80]
    ldp x27, x28, [x1, #64]
    ldp x25, x26, [x1, #48]
    ldp x23, x24, [x1, #32]
    ldp x21, x22, [x1, #16]
    ldp x19, x20, [x1, #0]
    ret
