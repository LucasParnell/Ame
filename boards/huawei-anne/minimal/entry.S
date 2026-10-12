// First-light v6: C++ holds red/blue/green for three seconds each, then this
// entry code uses the v5 LPM3 reset pulse. No PSCI or WFI-based timed waits.
.section .text.entry, "ax"
.global _start
_start:
    b start
    .long 0
    .quad 0x80000
    .quad image_end - _start
    .quad 2 // little-endian, 4K pages, placement near the start of RAM
    .quad 0, 0, 0
    .long 0x644d5241
    .long 0
start:
    msr daifset, #15
    mov x19, x0                 // x0 = DTB pointer from the firmware
    mrs x20, CurrentEL
    lsr x20, x20, #2            // x20 = 1 (EL1) / 2 (EL2) / other
    msr spsel, #1
    adr x1, stack_end
    mov sp, x1
    adr x1, fl_vectors
    cmp x20, #1
    b.eq fl_el1
    cmp x20, #2
    b.eq fl_el2
    b fl_reset_other
fl_el1:
    msr vbar_el1, x1
    mrs x21, sctlr_el1
    b fl_entry_state
fl_el2:
    msr vbar_el2, x1
    mrs x21, sctlr_el2
fl_entry_state:
    isb
    tst x21, #1
    b.ne fl_reset_mmu_on
    adr x1, __bss_start
    adr x2, __bss_end
fl_bss:
    cmp x1, x2
    b.hs fl_main
    str xzr, [x1], #8
    b fl_bss
fl_main:
    mov x0, x20                 // EL
    mov x1, x19                 // DTB
    mov x2, x21                 // SCTLR
    bl anko_firstlight_main     // never returns; ends in the reset ladder
    b fl_park                   // unreachable safety
fl_park:
    wfe
    b fl_park

// --- timed reset ladder ----------------------------------------------------
.global anko_fl_finish
.type anko_fl_finish, %function
anko_fl_finish:                 // x0 = EL
    cmp x0, #1
    b.eq fl_wait_8
    cmp x0, #2
    b.eq fl_wait_25
    b fl_wait_60
fl_reset_mmu_on:
    mov x2, #45
    mov x6, xzr
    b fl_wait_reset
fl_reset_nested:
    mov x2, #6
    mov x6, xzr
    b fl_wait_reset
fl_reset_other:
    mov x2, #60
    mov x6, xzr
    b fl_wait_reset
fl_wait_8:
    mov x2, #0
    mov x6, #1
    b fl_wait_reset
fl_wait_25:
    mov x2, #0
    mov x6, #1
    b fl_wait_reset
fl_wait_60:
    mov x2, #0
    mov x6, #1
fl_wait_reset:                  // x2 = seconds to wait; x6 = 1: note reset attempts
    mov x19, x6
    movz x3, #0x4c00
    movk x3, #0x1d, lsl #16    // measured 1,920,000 Hz; CNTFRQ is incorrect
fl_have_freq:
    mrs x4, CNTPCT_EL0
    mul x2, x2, x3
    add x2, x4, x2
fl_wait_loop:
    mrs x4, CNTPCT_EL0
    cmp x4, x2
    b.lo fl_wait_loop
fl_try_reset:
    cbz x19, fl_r0
    mov x0, #0x4
    bl anko_fl_note             // note before the pulse so it survives a fast reset
fl_r0:
    // Pulse nmi_in (bit 2) in SCLPMCUCTRL: ask the LPM3 MCU to reset the SoC
    // (this mirrors the stock kernel's hisi_pm_system_reset path).
#ifdef ANNE_MINIMAL_TEST
    movz x9, #0
    movk x9, #0x4500, lsl #16   // synthetic SCTRL registers in QEMU RAM
#else
    movz x9, #0xa000
    movk x9, #0xfff0, lsl #16   // x9 = 0xFFF0A000 (SCTRL base)
#endif
    add x9, x9, #0x510          // 0xFFF0A510 = SCLPMCUCTRL
    ldr w10, [x9]
    orr w10, w10, #0x4
    str w10, [x9]
    bic w10, w10, #0x4
    str w10, [x9]
fl_r1:
    // A returning pulse gets a two-second green blink before another attempt.
    cbz x19, fl_retry_plain
    bl anko_fl_retry_wait
    b fl_try_reset
fl_retry_plain:
    movz x3, #0x4c00
    movk x3, #0x1d, lsl #16
fl_retry_have_freq:
    mrs x4, CNTPCT_EL0
    mov x5, #2
    mul x5, x5, x3
    add x5, x4, x5
fl_retry_wait:
    mrs x4, CNTPCT_EL0
    cmp x4, x5
    b.lo fl_retry_wait
    b fl_try_reset

.balign 2048
fl_vectors:
    .rept 16
    b fl_fault_entry
    .balign 128
    .endr

fl_fault_entry:
    msr daifset, #15
    adr x3, stack_end
    cmp sp, x3
    b.lo fl_fault_go
    adr x3, fault_stack_end
    cmp sp, x3
    b.ls fl_reset_nested        // already on the fault stack: reset fast
fl_fault_go:
    adr x3, fault_stack_end
    mov sp, x3
    mrs x0, CurrentEL
    cmp x0, #8
    b.eq fl_fault_el2
    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    b fl_fault_report
fl_fault_el2:
    mrs x0, esr_el2
    mrs x1, elr_el2
    mrs x2, far_el2
fl_fault_report:
    bl anko_fl_fault             // best-effort record, then the reset ladder
    b fl_park
