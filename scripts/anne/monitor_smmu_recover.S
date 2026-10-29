// Replaces only the current-EL SP0 synchronous vector in the RAM profile.
// The callback executes with SP_EL0, as confirmed in the pinned SMC path.
// x9/x10 are caller-saved scratch; no stack or global recovery state is used.
.section .text, "ax"
.global smmu_recover
smmu_recover:
    mrs x9, elr_el3
    ldr x10, =probe_load
    cmp x9, x10
    b.ne unhandled
    mrs x9, esr_el3
    lsr x10, x9, #26
    cmp x10, #0x25             // data abort at the current exception level
    b.ne unhandled
    tbnz x9, #6, unhandled     // never recover writes
    mrs x10, far_el3
    cmp x10, x4               // only the callback's selected fixed word
    b.ne unhandled
    mov w0, w9                // fault result is the 32-bit ESR, not a read zero
    ldr x10, =probe_return
    msr elr_el3, x10
    eret
unhandled:
    // Preserve the stock fatal path for every other exception. x9/x10 in
    // its crash report are scratch from this guard; nothing else is resumed.
    bl stock_unhandled
    b .
    .ltorg
