// kernel/arch/arm64/syscall.S — SVC entry and exit.
#include "anko/interrupt_mask.h"

.section .text.syscall, "ax"
.global syscall_entry
.type syscall_entry, %function
.global irq_entry
.type irq_entry, %function
.global irq_entry_kernel
.type irq_entry_kernel, %function

.extern syscall_dispatch
.extern syscall_fault
.extern post_syscall_frame
.extern fpsimd_claim
.extern irq_dispatch

// Stack frame layout (288 bytes):
//   0x000..0x0f0  x0..x30
//   0x0f8         ELR_EL1
//   0x100         SPSR_EL1
//   0x108         SP_EL0
//   0x110..0x11f  padding
syscall_entry:
    sub     sp, sp, #288

    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    mrs     x9,  ELR_EL1
    mrs     x10, SPSR_EL1
    mrs     x11, SP_EL0
    str     x9,  [sp, #248]
    str     x10, [sp, #256]
    str     x11, [sp, #264]

    // Distinguish EL0 SVC from other EL0 synchronous faults.
    mrs     x12, ESR_EL1
    lsr     x13, x12, #26
    cmp     x13, #0x15
    b.eq    1f

    // FP/SIMD access trap (EC 0x07): the thread touched a vector register
    // before claiming its state. Restore it, open FPEN and re-execute the
    // faulting instruction; ELR already points at it.
    cmp     x13, #0x07
    b.ne    4f
    bl      fpsimd_claim
    b       2f

4:
    // Non-SVC EL0 sync fault: kill current thread and reschedule.
    mrs     x1, FAR_EL1
    mov     x0, x12
    mov     x2, sp
    bl      syscall_fault
    mov     x0, #7
    neg     x0, x0
    str     x0, [sp, #0]
    str     xzr, [sp, #8]
    b       2f

1:
    // syscall_dispatch(nr, a0, a1, a2, a3, a4, a5)
    ldr     x0, [sp, #64]   // x8 syscall number
    ldr     x1, [sp, #0]    // a0
    ldr     x2, [sp, #8]    // a1
    ldr     x3, [sp, #16]   // a2
    ldr     x4, [sp, #24]   // a3
    ldr     x5, [sp, #32]   // a4
    ldr     x6, [sp, #40]   // a5
    bl      syscall_dispatch

    // Return values in x0/x1.
    str     x0, [sp, #0]
    str     x1, [sp, #8]

2:
    // post_syscall(current_frame) -> next_frame
    mov     x0, sp
    bl      post_syscall_frame

    // x0 selects the live stack frame or the next thread-owned frame.
    mov     x30, x0
    ldr     x9,  [x30, #248]
    ldr     x10, [x30, #256]
    ldr     x11, [x30, #264]
    msr     ELR_EL1,  x9
#if ANKO_HOST_FIQ_MASK
    orr     x10, x10, #ANKO_HOST_FIQ_MASK
#endif
    msr     SPSR_EL1, x10
    msr     SP_EL0,   x11


    // Release this CPU exception stack; restore the selected register file.
    add     sp, sp, #288
    ldp     x0,  x1,  [x30, #0]
    ldp     x2,  x3,  [x30, #16]
    ldp     x4,  x5,  [x30, #32]
    ldp     x6,  x7,  [x30, #48]
    ldp     x8,  x9,  [x30, #64]
    ldp     x10, x11, [x30, #80]
    ldp     x12, x13, [x30, #96]
    ldp     x14, x15, [x30, #112]
    ldp     x16, x17, [x30, #128]
    ldp     x18, x19, [x30, #144]
    ldp     x20, x21, [x30, #160]
    ldp     x22, x23, [x30, #176]
    ldp     x24, x25, [x30, #192]
    ldp     x26, x27, [x30, #208]
    ldp     x28, x29, [x30, #224]
    ldr     x30,      [x30, #240]

    eret

irq_entry:
    sub     sp, sp, #288

    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    mrs     x9,  ELR_EL1
    mrs     x10, SPSR_EL1
    mrs     x11, SP_EL0
    str     x9,  [sp, #248]
    str     x10, [sp, #256]
    str     x11, [sp, #264]

    bl      irq_dispatch

    mov     x0, sp
    bl      post_syscall_frame

    // x0 selects the live stack frame or the next thread-owned frame.
    mov     x30, x0
    ldr     x9,  [x30, #248]
    ldr     x10, [x30, #256]
    ldr     x11, [x30, #264]
    msr     ELR_EL1,  x9
#if ANKO_HOST_FIQ_MASK
    orr     x10, x10, #ANKO_HOST_FIQ_MASK
#endif
    msr     SPSR_EL1, x10
    msr     SP_EL0,   x11


    // Release this CPU exception stack; restore the selected register file.
    add     sp, sp, #288
    ldp     x0,  x1,  [x30, #0]
    ldp     x2,  x3,  [x30, #16]
    ldp     x4,  x5,  [x30, #32]
    ldp     x6,  x7,  [x30, #48]
    ldp     x8,  x9,  [x30, #64]
    ldp     x10, x11, [x30, #80]
    ldp     x12, x13, [x30, #96]
    ldp     x14, x15, [x30, #112]
    ldp     x16, x17, [x30, #128]
    ldp     x18, x19, [x30, #144]
    ldp     x20, x21, [x30, #160]
    ldp     x22, x23, [x30, #176]
    ldp     x24, x25, [x30, #192]
    ldp     x26, x27, [x30, #208]
    ldp     x28, x29, [x30, #224]
    ldr     x30,      [x30, #240]

    eret

irq_entry_kernel:
    sub     sp, sp, #288

    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    mrs     x9,  ELR_EL1
    mrs     x10, SPSR_EL1
    mrs     x11, SP_EL0
    str     x9,  [sp, #248]
    str     x10, [sp, #256]
    str     x11, [sp, #264]

    bl      irq_dispatch

    ldr     x9,  [sp, #248]
    ldr     x10, [sp, #256]
    ldr     x11, [sp, #264]
    msr     ELR_EL1,  x9
#if ANKO_HOST_FIQ_MASK
    orr     x10, x10, #ANKO_HOST_FIQ_MASK
#endif
    msr     SPSR_EL1, x10
    msr     SP_EL0,   x11

    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]

    add     sp, sp, #288
    eret
