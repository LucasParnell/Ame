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
    VEC vec_trap   // sync
    VEC irq_entry_kernel // irq
    VEC vec_trap   // fiq
    VEC vec_trap   // serror

    // Lower EL using AArch64
    VEC syscall_entry // sync (SVC from EL0)
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
