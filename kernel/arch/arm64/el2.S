// kernel/arch/arm64/el2.S — tiny EL2 monitor for Dango bring-up.

.section .text.el2, "ax"
.balign 2048
.global el2_vectors
.type el2_vectors, %function

.equ CTX_REGS,          0
.equ CTX_HOST_SP_EL1,   248
.equ CTX_PC,            256
.equ CTX_PSTATE,        264
.equ CTX_VTTBR,         272
.equ CTX_VTCR,          280
.equ CTX_EXIT_ESR,      288
.equ CTX_EXIT_FAR,      296
.equ CTX_EXIT_HPFAR,    304
.equ CTX_HOST_ELR,      312
.equ CTX_HOST_SPSR,     320
.equ CTX_HOST_SCTLR,    328
.equ CTX_HOST_TTBR0,    336
.equ CTX_HOST_TCR,      344
.equ CTX_HOST_MAIR,     352
.equ CTX_HOST_VBAR,     360
.equ CTX_GUEST_SCTLR,   368
.equ CTX_GUEST_TTBR0,   376
.equ CTX_GUEST_TTBR1,   384
.equ CTX_GUEST_TCR,     392
.equ CTX_GUEST_MAIR,    400
.equ CTX_GUEST_VBAR,    408
.equ CTX_GUEST_SP_EL1,  416
.equ CTX_INITIALIZED,   424
.equ CTX_HOST_CNTP_CTL,    432
.equ CTX_HOST_CNTP_CVAL,   440
.equ CTX_GUEST_CNTP_CTL,   448
.equ CTX_GUEST_CNTP_CVAL,  456
.equ CTX_GUEST_CNTV_CTL,   464
.equ CTX_GUEST_CNTV_CVAL,  472
.equ CTX_GUEST_IRQ_PENDING, 480
.equ CTX_TIMER_HW_MASK,    488
.equ CTX_EXIT_KIND,        496
.equ CTX_GUEST_VMPIDR,    504
.equ CTX_GUEST_VPIDR,     512
.equ CTX_HOST_TTBR1,      520
.equ CTX_HOST_SP_EL0,     528
.equ CTX_HOST_TPIDR_EL1,  536
.equ CTX_HOST_TPIDR_EL0,  544
.equ CTX_HOST_TPIDRRO_EL0,552
.equ CTX_HOST_CONTEXTIDR, 560
.equ CTX_HOST_CPACR,      568
.equ CTX_HOST_CNTKCTL,    576
.equ CTX_HOST_AMAIR,      584
.equ CTX_GUEST_SP_EL0,    592
.equ CTX_GUEST_TPIDR_EL1, 600
.equ CTX_GUEST_TPIDR_EL0, 608
.equ CTX_GUEST_TPIDRRO_EL0,616
.equ CTX_GUEST_CONTEXTIDR,624
.equ CTX_GUEST_CPACR,     632
.equ CTX_GUEST_CNTKCTL,   640
.equ CTX_GUEST_AMAIR,     648
.equ CTX_GUEST_ELR_EL1,   656
.equ CTX_GUEST_SPSR_EL1,  664
.equ CTX_GUEST_FAR_EL1,   672
.equ CTX_GUEST_ESR_EL1,   680
.equ CTX_GUEST_AFSR0_EL1, 688
.equ CTX_GUEST_AFSR1_EL1, 696
.equ CTX_HOST_FPSIMD,     704
.equ CTX_GUEST_FPSIMD,   1232

#include "midix/abi/cpu_limits.h"
.equ EL2_RUN_STATE_MAX_CPUS, MX_HOST_MAX_CPUS

.macro EL2_VEC target
    .balign 128
    b \target
.endm

.macro LOAD_EL2_RUN_STATE_SLOT slot_reg, tmp_reg
    mrs     \tmp_reg, TPIDR_EL2
    cmp     \tmp_reg, #EL2_RUN_STATE_MAX_CPUS
    b.lo    1f
    mov     \tmp_reg, xzr
1:
    adrp    \slot_reg, g_el2_run_state
    add     \slot_reg, \slot_reg, :lo12:g_el2_run_state
    add     \slot_reg, \slot_reg, \tmp_reg, lsl #3
.endm

/*
 * Save lower-EL GPRs before using any register as EL2 scratch state.
 *
 * Asynchronous entries are especially easy to get wrong: the interrupted
 * guest's x0-x30 are still live in the hardware registers when the vector is
 * entered.  Do not mark the exit kind in x23 until after x23 has been saved to
 * the stack, otherwise every host IRQ corrupts guest x23.
 */
.macro SAVE_LOWER_GPRS
    sub     sp, sp, #256
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
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
    str     x30, [sp, #240]
.endm

el2_vectors:
    EL2_VEC el2_unhandled
    EL2_VEC el2_unhandled
    EL2_VEC el2_unhandled
    EL2_VEC el2_unhandled

    EL2_VEC el2_unhandled
    EL2_VEC el2_unhandled
    EL2_VEC el2_unhandled
    EL2_VEC el2_unhandled

    EL2_VEC el2_lower_sync
    EL2_VEC el2_lower_irq
    EL2_VEC el2_lower_fiq
    EL2_VEC el2_lower_serror

    EL2_VEC el2_lower_sync
    EL2_VEC el2_lower_irq
    EL2_VEC el2_lower_fiq
    EL2_VEC el2_lower_serror

el2_unhandled:
1:  wfe
    b 1b

el2_lower_irq:
    SAVE_LOWER_GPRS
    mov     x23, #1
    b       el2_lower_async_saved

el2_lower_fiq:
    SAVE_LOWER_GPRS
    mov     x23, #2
    b       el2_lower_async_saved

el2_lower_serror:
    SAVE_LOWER_GPRS
    mov     x23, #3

el2_lower_async_saved:
    LOAD_EL2_RUN_STATE_SLOT x20, x24
    ldr     x21, [x20]
    cbz     x21, el2_unhandled_saved
    b       el2_guest_exit_common

el2_unhandled_saved:
    add     sp, sp, #256
    b       el2_unhandled

el2_lower_sync:
    SAVE_LOWER_GPRS

    LOAD_EL2_RUN_STATE_SLOT x20, x24
    ldr     x21, [x20]
    cbnz    x21, el2_guest_exit

    mrs     x0, ESR_EL2
    ubfx    x1, x0, #26, #6
    cmp     x1, #0x16
    b.ne    el2_unhandled

    ldr     x21, [sp, #0]
    str     x21, [x20]

    mrs     x0, ELR_EL2
    str     x0, [x21, #CTX_HOST_ELR]
    mrs     x0, SPSR_EL2
    str     x0, [x21, #CTX_HOST_SPSR]
    mrs     x0, SP_EL1
    str     x0, [x21, #CTX_HOST_SP_EL1]
    mrs     x0, SCTLR_EL1
    str     x0, [x21, #CTX_HOST_SCTLR]
    mrs     x0, TTBR0_EL1
    str     x0, [x21, #CTX_HOST_TTBR0]
    mrs     x0, TTBR1_EL1
    str     x0, [x21, #CTX_HOST_TTBR1]
    mrs     x0, TCR_EL1
    str     x0, [x21, #CTX_HOST_TCR]
    mrs     x0, MAIR_EL1
    str     x0, [x21, #CTX_HOST_MAIR]
    mrs     x0, VBAR_EL1
    str     x0, [x21, #CTX_HOST_VBAR]
    mrs     x0, SP_EL0
    str     x0, [x21, #CTX_HOST_SP_EL0]
    mrs     x0, TPIDR_EL1
    str     x0, [x21, #CTX_HOST_TPIDR_EL1]
    mrs     x0, TPIDR_EL0
    str     x0, [x21, #CTX_HOST_TPIDR_EL0]
    mrs     x0, TPIDRRO_EL0
    str     x0, [x21, #CTX_HOST_TPIDRRO_EL0]
    mrs     x0, CONTEXTIDR_EL1
    str     x0, [x21, #CTX_HOST_CONTEXTIDR]
    mrs     x0, CPACR_EL1
    str     x0, [x21, #CTX_HOST_CPACR]
    mrs     x0, CNTKCTL_EL1
    str     x0, [x21, #CTX_HOST_CNTKCTL]
    mrs     x0, AMAIR_EL1
    str     x0, [x21, #CTX_HOST_AMAIR]

    // The scalar monitor must isolate the runner's userland vector state
    // from every vCPU, including asynchronous IRQ/FIQ exits.
    add     x0, x21, #CTX_HOST_FPSIMD
    bl      arm64_fpsimd_save

    ldr     x0, [x21, #CTX_VTCR]
    msr     VTCR_EL2, x0
    ldr     x0, [x21, #CTX_VTTBR]
    msr     VTTBR_EL2, x0
    dsb     ishst
    tlbi    vmalls12e1
    dsb     ish
    isb

    mov     x0, #3
    msr     CNTHCTL_EL2, x0
    msr     CNTVOFF_EL2, xzr
    msr     CPTR_EL2, xzr
    add     x0, x21, #CTX_GUEST_FPSIMD
    bl      arm64_fpsimd_restore

    ldr     x0, [x21, #CTX_GUEST_VMPIDR]
    msr     VMPIDR_EL2, x0
    ldr     x0, [x21, #CTX_GUEST_VPIDR]
    msr     VPIDR_EL2, x0

    ldr     x0, [x21, #CTX_GUEST_SP_EL0]
    msr     SP_EL0, x0
    ldr     x0, [x21, #CTX_GUEST_TPIDR_EL1]
    msr     TPIDR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_TPIDR_EL0]
    msr     TPIDR_EL0, x0
    ldr     x0, [x21, #CTX_GUEST_TPIDRRO_EL0]
    msr     TPIDRRO_EL0, x0
    ldr     x0, [x21, #CTX_GUEST_CONTEXTIDR]
    msr     CONTEXTIDR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_CPACR]
    msr     CPACR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_CNTKCTL]
    msr     CNTKCTL_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_AMAIR]
    msr     AMAIR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_ELR_EL1]
    msr     ELR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_SPSR_EL1]
    msr     SPSR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_FAR_EL1]
    msr     FAR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_ESR_EL1]
    msr     ESR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_AFSR0_EL1]
    msr     AFSR0_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_AFSR1_EL1]
    msr     AFSR1_EL1, x0

    ldr     x0, [x21, #CTX_GUEST_MAIR]
    msr     MAIR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_TCR]
    msr     TCR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_TTBR0]
    msr     TTBR0_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_TTBR1]
    msr     TTBR1_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_VBAR]
    msr     VBAR_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_SP_EL1]
    msr     SP_EL1, x0
    ldr     x0, [x21, #CTX_GUEST_SCTLR]
    msr     SCTLR_EL1, x0
    isb

    ldr     x0, [x21, #CTX_PC]
    msr     ELR_EL2, x0
    ldr     x0, [x21, #CTX_PSTATE]
    msr     SPSR_EL2, x0

    mov     x0, #1
    orr     x0, x0, #(1 << 3)
    orr     x0, x0, #(1 << 4)
    orr     x0, x0, #(1 << 5)
    // Trap guest WFI/WFE back to the host.  The virtual-timer poll path is
    // slower than sleeping on the hardware timer, but it is deterministic
    // during bring-up and avoids losing the guest after early console init.
    orr     x0, x0, #(1 << 13)   // TWI: trap guest WFI
    orr     x0, x0, #(1 << 14)   // TWE: trap guest WFE
    orr     x0, x0, #(1 << 19)
    orr     x0, x0, #(1 << 31)
    ldr     x1, [x21, #CTX_GUEST_IRQ_PENDING]
    cbz     x1, 1f
    orr     x0, x0, #(1 << 7)
1:
    msr     HCR_EL2, x0
    isb

    mrs     x0, CNTP_CTL_EL0
    str     x0, [x21, #CTX_HOST_CNTP_CTL]
    mrs     x0, CNTP_CVAL_EL0
    str     x0, [x21, #CTX_HOST_CNTP_CVAL]

    ldr     x3, [x21, #CTX_TIMER_HW_MASK]

    ldr     x0, [x21, #CTX_GUEST_CNTP_CTL]
    tbz     x3, #0, 1f
    orr     x0, x0, #2
1:
    msr     CNTP_CTL_EL0, x0
    ldr     x0, [x21, #CTX_GUEST_CNTP_CVAL]
    msr     CNTP_CVAL_EL0, x0

    ldr     x0, [x21, #CTX_GUEST_CNTV_CTL]
    tbz     x3, #1, 2f
    orr     x0, x0, #2
2:
    msr     CNTV_CTL_EL0, x0
    ldr     x0, [x21, #CTX_GUEST_CNTV_CVAL]
    msr     CNTV_CVAL_EL0, x0
    isb

    add     sp, sp, #256
    mov     x20, x21

    ldp     x0, x1, [x20, #0]
    ldp     x2, x3, [x20, #16]
    ldp     x4, x5, [x20, #32]
    ldp     x6, x7, [x20, #48]
    ldp     x8, x9, [x20, #64]
    ldp     x10, x11, [x20, #80]
    ldp     x12, x13, [x20, #96]
    ldp     x14, x15, [x20, #112]
    ldp     x16, x17, [x20, #128]
    ldp     x18, x19, [x20, #144]
    ldp     x22, x23, [x20, #176]
    ldp     x24, x25, [x20, #192]
    ldp     x26, x27, [x20, #208]
    ldp     x28, x29, [x20, #224]
    ldr     x30, [x20, #240]
    ldr     x21, [x20, #168]
    ldr     x20, [x20, #160]
    eret

el2_guest_exit:
    mov     x23, #0

el2_guest_exit_common:
    mov     x22, x21
    add     x0, x22, #CTX_GUEST_FPSIMD
    bl      arm64_fpsimd_save

    ldp     x0, x1, [sp, #0]
    stp     x0, x1, [x22, #0]
    ldp     x0, x1, [sp, #16]
    stp     x0, x1, [x22, #16]
    ldp     x0, x1, [sp, #32]
    stp     x0, x1, [x22, #32]
    ldp     x0, x1, [sp, #48]
    stp     x0, x1, [x22, #48]
    ldp     x0, x1, [sp, #64]
    stp     x0, x1, [x22, #64]
    ldp     x0, x1, [sp, #80]
    stp     x0, x1, [x22, #80]
    ldp     x0, x1, [sp, #96]
    stp     x0, x1, [x22, #96]
    ldp     x0, x1, [sp, #112]
    stp     x0, x1, [x22, #112]
    ldp     x0, x1, [sp, #128]
    stp     x0, x1, [x22, #128]
    ldp     x0, x1, [sp, #144]
    stp     x0, x1, [x22, #144]
    ldp     x0, x1, [sp, #160]
    stp     x0, x1, [x22, #160]
    ldp     x0, x1, [sp, #176]
    stp     x0, x1, [x22, #176]
    ldp     x0, x1, [sp, #192]
    stp     x0, x1, [x22, #192]
    ldp     x0, x1, [sp, #208]
    stp     x0, x1, [x22, #208]
    ldp     x0, x1, [sp, #224]
    stp     x0, x1, [x22, #224]
    ldr     x0, [sp, #240]
    str     x0, [x22, #240]

    cbnz    x23, 1f
    mrs     x0, ESR_EL2
    str     x0, [x22, #CTX_EXIT_ESR]
    mrs     x0, FAR_EL2
    str     x0, [x22, #CTX_EXIT_FAR]
    mrs     x0, HPFAR_EL2
    str     x0, [x22, #CTX_EXIT_HPFAR]
    b       2f
1:
    str     xzr, [x22, #CTX_EXIT_ESR]
    str     xzr, [x22, #CTX_EXIT_FAR]
    str     xzr, [x22, #CTX_EXIT_HPFAR]
2:
    str     x23, [x22, #CTX_EXIT_KIND]

    mrs     x0, ELR_EL2
    str     x0, [x22, #CTX_PC]
    mrs     x0, SPSR_EL2
    str     x0, [x22, #CTX_PSTATE]
    mrs     x0, SP_EL1
    str     x0, [x22, #CTX_GUEST_SP_EL1]
    mrs     x0, SCTLR_EL1
    str     x0, [x22, #CTX_GUEST_SCTLR]
    mrs     x0, TTBR0_EL1
    str     x0, [x22, #CTX_GUEST_TTBR0]
    mrs     x0, TTBR1_EL1
    str     x0, [x22, #CTX_GUEST_TTBR1]
    mrs     x0, TCR_EL1
    str     x0, [x22, #CTX_GUEST_TCR]
    mrs     x0, MAIR_EL1
    str     x0, [x22, #CTX_GUEST_MAIR]
    mrs     x0, VBAR_EL1
    str     x0, [x22, #CTX_GUEST_VBAR]
    mrs     x0, SP_EL0
    str     x0, [x22, #CTX_GUEST_SP_EL0]
    mrs     x0, TPIDR_EL1
    str     x0, [x22, #CTX_GUEST_TPIDR_EL1]
    mrs     x0, TPIDR_EL0
    str     x0, [x22, #CTX_GUEST_TPIDR_EL0]
    mrs     x0, TPIDRRO_EL0
    str     x0, [x22, #CTX_GUEST_TPIDRRO_EL0]
    mrs     x0, CONTEXTIDR_EL1
    str     x0, [x22, #CTX_GUEST_CONTEXTIDR]
    mrs     x0, CPACR_EL1
    str     x0, [x22, #CTX_GUEST_CPACR]
    mrs     x0, CNTKCTL_EL1
    str     x0, [x22, #CTX_GUEST_CNTKCTL]
    mrs     x0, AMAIR_EL1
    str     x0, [x22, #CTX_GUEST_AMAIR]
    mrs     x0, ELR_EL1
    str     x0, [x22, #CTX_GUEST_ELR_EL1]
    mrs     x0, SPSR_EL1
    str     x0, [x22, #CTX_GUEST_SPSR_EL1]
    mrs     x0, FAR_EL1
    str     x0, [x22, #CTX_GUEST_FAR_EL1]
    mrs     x0, ESR_EL1
    str     x0, [x22, #CTX_GUEST_ESR_EL1]
    mrs     x0, AFSR0_EL1
    str     x0, [x22, #CTX_GUEST_AFSR0_EL1]
    mrs     x0, AFSR1_EL1
    str     x0, [x22, #CTX_GUEST_AFSR1_EL1]

    ldr     x3, [x22, #CTX_TIMER_HW_MASK]

    mrs     x0, CNTP_CTL_EL0
    mrs     x1, CNTP_CVAL_EL0
    ldr     x2, [x22, #CTX_GUEST_CNTP_CTL]
    ldr     x4, [x22, #CTX_GUEST_CNTP_CVAL]
    tbz     x3, #0, 1f
    orr     x5, x2, #2
    cmp     x0, x5
    b.ne    0f
    cmp     x1, x4
    b.ne    0f
    mov     x0, x2
    mov     x1, x4
    b       1f
0:
    tbnz    x2, #1, 1f
    bic     x0, x0, #2
1:
    str     x0, [x22, #CTX_GUEST_CNTP_CTL]
    str     x1, [x22, #CTX_GUEST_CNTP_CVAL]

    mrs     x0, CNTV_CTL_EL0
    mrs     x1, CNTV_CVAL_EL0
    ldr     x2, [x22, #CTX_GUEST_CNTV_CTL]
    ldr     x4, [x22, #CTX_GUEST_CNTV_CVAL]
    tbz     x3, #1, 3f
    orr     x5, x2, #2
    cmp     x0, x5
    b.ne    2f
    cmp     x1, x4
    b.ne    2f
    mov     x0, x2
    mov     x1, x4
    b       3f
2:
    tbnz    x2, #1, 3f
    bic     x0, x0, #2
3:
    str     x0, [x22, #CTX_GUEST_CNTV_CTL]
    str     x1, [x22, #CTX_GUEST_CNTV_CVAL]

    ldr     x0, [x22, #CTX_HOST_CNTP_CTL]
    msr     CNTP_CTL_EL0, x0
    ldr     x0, [x22, #CTX_HOST_CNTP_CVAL]
    msr     CNTP_CVAL_EL0, x0
    isb

    mov     x0, #0x80000000
    msr     HCR_EL2, x0

    // Return host-visible virtual CPU ID registers to physical-looking values.
    // Host scheduler code should not depend on MPIDR_EL1 anymore, but keeping
    // VMPIDR_EL2 sane avoids future surprises.
    mrs     x1, MPIDR_EL1
    msr     VMPIDR_EL2, x1
    mrs     x1, MIDR_EL1
    msr     VPIDR_EL2, x1

    msr     VTTBR_EL2, xzr
    isb

    ldr     x0, [x22, #CTX_HOST_MAIR]
    msr     MAIR_EL1, x0
    ldr     x0, [x22, #CTX_HOST_TCR]
    msr     TCR_EL1, x0
    ldr     x0, [x22, #CTX_HOST_TTBR0]
    msr     TTBR0_EL1, x0
    ldr     x0, [x22, #CTX_HOST_TTBR1]
    msr     TTBR1_EL1, x0
    ldr     x0, [x22, #CTX_HOST_VBAR]
    msr     VBAR_EL1, x0
    ldr     x0, [x22, #CTX_HOST_SP_EL1]
    msr     SP_EL1, x0
    ldr     x0, [x22, #CTX_HOST_SP_EL0]
    msr     SP_EL0, x0
    ldr     x0, [x22, #CTX_HOST_TPIDR_EL1]
    msr     TPIDR_EL1, x0
    ldr     x0, [x22, #CTX_HOST_TPIDR_EL0]
    msr     TPIDR_EL0, x0
    ldr     x0, [x22, #CTX_HOST_TPIDRRO_EL0]
    msr     TPIDRRO_EL0, x0
    ldr     x0, [x22, #CTX_HOST_CONTEXTIDR]
    msr     CONTEXTIDR_EL1, x0
    ldr     x0, [x22, #CTX_HOST_CPACR]
    msr     CPACR_EL1, x0
    ldr     x0, [x22, #CTX_HOST_CNTKCTL]
    msr     CNTKCTL_EL1, x0
    ldr     x0, [x22, #CTX_HOST_AMAIR]
    msr     AMAIR_EL1, x0
    ldr     x0, [x22, #CTX_HOST_SCTLR]
    msr     SCTLR_EL1, x0
    isb

    add     x0, x22, #CTX_HOST_FPSIMD
    bl      arm64_fpsimd_restore

    ldr     x0, [x22, #CTX_HOST_ELR]
    msr     ELR_EL2, x0
    ldr     x0, [x22, #CTX_HOST_SPSR]
    msr     SPSR_EL2, x0
    str     xzr, [x20]
    ldr     x0, [x22, #CTX_EXIT_ESR]
    add     sp, sp, #256
    eret

.global vm_el2_run
.type vm_el2_run, %function
vm_el2_run:
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x27, x28, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!
    hvc     #0
    ldp     x29, x30, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16
    ret

.section .bss
.align 12
.global g_el2_run_state
g_el2_run_state:
    .rept EL2_RUN_STATE_MAX_CPUS
    .quad 0
    .endr

.align 14
.global g_el2_stacks
g_el2_stacks:
    // 4 CPUs * 16 KiB.  Stack top for CPU n is:
    //   g_el2_stacks + (n + 1) * 16384
    .skip (MX_HOST_MAX_CPUS * 16384)

// Compatibility symbol: top of the whole EL2 stack area.
.global __el2_stack_top
__el2_stack_top:
