// Only these explicit state-management routines use FP/SIMD in the kernel.
// Called with IRQs masked. x0 is the aligned State pointer; x1 is scratch.
.section .text, "ax"
.balign 4
.global arm64_fpsimd_enable
.type arm64_fpsimd_enable, %function
arm64_fpsimd_enable:
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20) // FPEN: permit EL0 and EL1 FP/Advanced SIMD
    msr cpacr_el1, x0
    isb
    ret
.size arm64_fpsimd_enable, . - arm64_fpsimd_enable

// FPEN=0b11: ordinary EL0 FP/SIMD execution, no trap.
.global arm64_fpsimd_open
.type arm64_fpsimd_open, %function
arm64_fpsimd_open:
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb
    ret
.size arm64_fpsimd_open, . - arm64_fpsimd_open

// FPEN=0b01: trap EL0 FP/SIMD, keep EL1 free for the save/restore routines.
// A thread that has never claimed its state pays one trap on first use.
.global arm64_fpsimd_close
.type arm64_fpsimd_close, %function
arm64_fpsimd_close:
    mrs x0, cpacr_el1
    and x0, x0, #~((3) << 20)
    orr x0, x0, #(1 << 20)
    msr cpacr_el1, x0
    isb
    ret
.size arm64_fpsimd_close, . - arm64_fpsimd_close

.global arm64_fpsimd_save
.type arm64_fpsimd_save, %function
arm64_fpsimd_save:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x1, fpsr
    str w1, [x0, #512]
    mrs x1, fpcr
    str w1, [x0, #516]
    ret
.size arm64_fpsimd_save, . - arm64_fpsimd_save

.global arm64_fpsimd_restore
.type arm64_fpsimd_restore, %function
arm64_fpsimd_restore:
    ldr w1, [x0, #512]
    msr fpsr, x1
    ldr w1, [x0, #516]
    msr fpcr, x1
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ret
.size arm64_fpsimd_restore, . - arm64_fpsimd_restore
