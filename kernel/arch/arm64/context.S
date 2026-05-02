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
    // TODO: stp x19, x20, [x0, #0]
    // TODO: stp x21, x22, [x0, #16]
    // TODO: ... x23-x28, fp, lr
    // TODO: mov x9, sp; str x9, [x0, #0x60]
    // TODO: ldr x9, [x1, #0x60]; mov sp, x9
    // TODO: ldp x19, x20, [x1, #0]
    // TODO: ... restore x21-x28, fp, lr
    // TODO: ret  — returns into next thread's lr
    ret
