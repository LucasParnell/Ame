// kernel/arch/arm64/psci.S — PSCI firmware call stubs.
//
// Using real assembly functions (not inline asm) ensures the C++ compiler
// treats hvc/smc as opaque calls and correctly clobbers all caller-saved
// registers (x0-x18, x30) per AAPCS64 — something inline asm clobber lists
// cannot easily express for an unknown firmware boundary.

.global psci_hvc_call
psci_hvc_call:
    hvc #0
    ret

.global psci_smc_call
psci_smc_call:
    smc #0
    ret
