// Kernel ownership adapter for the libk SHA primitive. Preserve every vector
// register it uses and CPACR/DAIF. A caller limits work to 4 KiB per call so
// IRQ/FIQ masking stays bounded, with watchdog progress between calls.
.text
.balign 4
.global kernel_sha256_blocks
.type kernel_sha256_blocks, %function
kernel_sha256_blocks:
    stp x29, x30, [sp, #-208]!
    stp x19, x20, [sp, #16]
    mov x29, sp
    mrs x19, daif
    msr daifset, #3
    mrs x20, cpacr_el1
    orr x9, x20, #(3 << 20)
    msr cpacr_el1, x9
    isb

    stp q0, q1, [sp, #32]
    stp q2, q3, [sp, #64]
    stp q4, q5, [sp, #96]
    stp q6, q7, [sp, #128]
    stp q16, q17, [sp, #160]
    str q18, [sp, #192]
    bl libk_arm64_sha256_blocks
    ldp q0, q1, [sp, #32]
    ldp q2, q3, [sp, #64]
    ldp q4, q5, [sp, #96]
    ldp q6, q7, [sp, #128]
    ldp q16, q17, [sp, #160]
    ldr q18, [sp, #192]

    msr cpacr_el1, x20
    isb
    msr daif, x19
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #208
    ret
.size kernel_sha256_blocks, . - kernel_sha256_blocks
