#include "anko/interrupt_mask.h"
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

// Start a station with a complete saved EL0 frame. Its first dispatch can be a
// migrated thread, so restoring only PC, SP, and x0 would corrupt its registers.
.global enter_user_frame
.type enter_user_frame, %function
enter_user_frame:
    mov x30, x0
    ldr x9, [x30, #248]
    ldr x10, [x30, #256]
    ldr x11, [x30, #264]
    msr elr_el1, x9
#if ANKO_HOST_FIQ_MASK
    orr x10, x10, #ANKO_HOST_FIQ_MASK
#endif
    msr spsr_el1, x10
    msr sp_el0, x11
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldp x18, x19, [x30, #144]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    ldr x30, [x30, #240]
    eret
