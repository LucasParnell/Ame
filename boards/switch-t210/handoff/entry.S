// Hekate's BL33 ABI: entry at byte 0, embedded-DTB offset at byte 0x10.
// The offset points to a zero marker, so Hekate skips its U-Boot DTB patches.
.section .text.entry, "ax"
.global _start
_start:
    b start
    nop
    .quad _start
    .quad probe_end - _start
start:
    msr daifset, #15
    // A dark screen is an entry signal even if the subsequent state/display
    // checks fail. Restore brightness only after successful magenta scanout.
    bl tegra_backlight_off
    mov w19, w0
#if !defined(ANKO_CPU_ENTRY_PROBE)
    mrs x0, CurrentEL
    cmp x0, #8
    b.ne park
    mrs x0, sctlr_el2
    tst x0, #1
    b.ne park
    tst x0, #4
    b.ne park
    msr spsel, #1
    adrp x0, stack_end
    add x0, x0, :lo12:stack_end
    mov sp, x0
    bl handoff_probe
    cbz w0, park
    mov w0, w19
    bl tegra_backlight_restore
#endif
park:
    wfe
    b park
