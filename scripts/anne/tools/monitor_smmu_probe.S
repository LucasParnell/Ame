// RAM-only, read-only secure SMMU identification experiment.
// No supplied address, writes, shared buffers, or persistent state.
.section .text, "ax"
.global monitor_probe, smmu_probe_load, smmu_probe_return
monitor_probe:
    mov w2, #0x4552
    movk w2, #0x414d, lsl #16
    cmp w1, w2
    b.ne rejected
    cmp x0, #1
    b.eq version
    cmp x0, #5
    b.eq recovery_check
    sub x2, x0, #2
    cmp x2, #2
    b.hi rejected
    mov x4, #0x20
    movk x4, #0xe8dc, lsl #16
    add x4, x4, x2, lsl #2
    b smmu_probe_load
recovery_check:
    // Deliberately unmapped in the pinned monitor's translation map.
    mov x4, #0x20000000
smmu_probe_load:
    ldr w0, [x4]
smmu_probe_return:
    ret
version:
    mov w0, #4
    movk w0, #0x414d, lsl #16
    ret
rejected:
    mov w0, #-1
    ret
