// boot/entry/start.S
// Entry point jumped to by U-Boot.
//
// AArch64 Linux boot protocol:
//   x0 = physical address of DTB blob
//   x1-x3 = reserved (0)
//   Current exception level: EL2 (or EL1 if EL2 not available)
//
// TODO:
//   - Verify / drop to EL1 if running at EL2
//   - Disable caches and MMU (in case U-Boot left them on)
//   - Set up an early boot stack (use .boot_stack section)
//   - Clear BSS
//   - Call boot_main(x0)  [x0 = dtb_phys]
//   - Halt on unexpected return

.section .text.boot, "ax"
.global _start
_start:
    // TODO: implementation
    b .
