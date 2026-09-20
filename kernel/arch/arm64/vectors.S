// kernel/arch/arm64/vectors.S — AArch64 exception vector table.
//
// Spec ref: §5.10 IRQ/Timer and §6 syscall entry from EL0.

.section .text.vectors, "ax"
.balign 2048
.global vectors
.type vectors, %function

.extern syscall_entry
.extern irq_entry
.extern irq_entry_kernel
.extern kernel_sync_fault

.macro VEC target
    .balign 128
    b \target
.endm

vectors:
    // Current EL with SP0
    VEC vec_trap   // sync
    VEC vec_trap   // irq
    VEC vec_trap   // fiq
    VEC vec_trap   // serror

    // Current EL with SPx
    VEC vec_kernel_sync   // sync — kernel fault → panic
    VEC irq_entry_kernel  // irq
    VEC vec_trap   // fiq
    VEC vec_trap   // serror

    // Lower EL using AArch64
    .balign 128
    .inst 0xd503229f // CSDB: speculation-control hint, safe on the QEMU cortex-a57 baseline.
    b syscall_entry  // sync (SVC from EL0)
    VEC irq_entry     // irq
    VEC vec_trap      // fiq
    VEC vec_trap      // serror

    // Lower EL using AArch32
    VEC vec_trap
    VEC vec_trap
    VEC vec_trap
    VEC vec_trap

vec_trap:
1:  wfe
    b 1b

// EL1 synchronous fault handler — saves full register state and calls
// kernel_sync_fault(esr, far, frame).  The sp_el0 slot in TrapFrame holds
// the pre-fault SP_EL1 value.
vec_kernel_sync:
    sub  sp, sp, #288
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30,      [sp, #240]
    mrs  x9,  ELR_EL1
    mrs  x10, SPSR_EL1
    add  x11, sp, #288          // pre-fault SP_EL1
    str  x9,  [sp, #248]        // -> tf.elr
    str  x10, [sp, #256]        // -> tf.spsr
    str  x11, [sp, #264]        // -> tf.sp_el0 slot (holds sp_el1 here)
    mrs  x0,  ESR_EL1
    mrs  x1,  FAR_EL1
    mov  x2,  sp
    bl   kernel_sync_fault
1:  wfe
    b    1b
