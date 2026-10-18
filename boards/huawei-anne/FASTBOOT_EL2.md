# Fastboot EL2 patch feasibility on ANE-LX1

14 October 2026. Target: the saved ANE-LX1 8.0.0.151(C782), Kirin 659/Hi6250.
This follows the [EL2 path audit](EL2_PATH_AUDIT.md), independently checks its
handoff instruction, and resolves the stock fastboot command-permission gate.
The subsequent [exact-device xloader validation](XLOADER_VALIDATION.md) captures
xloader/VRL and proves that this patch fails the captured content hash, while
updating that hash fails the existing certificate signature. The unresolved
authentication statements below describe the earlier stage of this audit.
The analysis uses the existing private firmware dump and recorded phone logs.
Live `fastboot devices` enumeration found no transport; no phone command,
reboot, flash, partition write, or lock-state change was performed.

## Finding

**There is a concrete fastboot patch that requests EL2, but no demonstrated
way to deploy and execute it through this handset's current stock fastboot.**
The hardware supports EL2, while all eight recorded Ame entries are EL1.
The normal bootloader handoff supplies a hardcoded EL1h SPSR to BL31. An offline
candidate changes that request to EL2h. Three separate questions remain:

| Gate | Exact-device evidence | Result |
| --- | --- | --- |
| Permit the partition write | USERLOCK is unlocked, FBLOCK is locked. The applicable command allowlist omits `flash:fastboot`. | Stock fastboot rejects that command in the recorded state. |
| Authenticate and execute modified fastboot | Saved logs identify a secure product. The preceding xloader and VRL metadata have not been collected or audited here. | Acceptance of a modified image is unproved; writing bytes through another route does not establish acceptance. |
| Give Ame working EL2 | Patched SPSR requests EL2h, but matching BL31 remains encrypted. | BL31 consumption, HVC enablement, secondary CPU entry, and actual guest execution need demonstration. |

Thus a normal bootloader unlock and `fastboot flash fastboot <patched-image>`
do not currently constitute a working EL2 route. This is not proof that every
boot-chain modification or vulnerability is impossible.

## Exact candidate

Source: `out/anne-device-private/el2-firmware-inspection/fastboot.img`.
Size: 4,194,304 bytes. SHA-256:
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
All code offsets in this document are offsets into that raw image, not runtime
addresses. They must not be applied to another firmware version.

The normal trustfirmware handoff at `0x10b5b8` builds its non-secure entry
record at physical `0x35e80030`. Its SPSR field is at `0x35e80040`:

```asm
; Original
0x10b634: 528078a1    mov w1, #0x3c5  ; AArch64 EL1h, D/A/I/F masked
0x10b640: b9001001    str w1, [x0, #0x10]

; Offline candidate
0x10b634: 52807921    mov w1, #0x3c9  ; AArch64 EL2h, D/A/I/F masked
0x10b640: b9001001    str w1, [x0, #0x10]
```

The four-byte instruction changes from `a1 78 80 52` to `21 79 80 52` in
little-endian order. Only bytes at `0x10b634` and `0x10b635` differ; the image
size and every other byte are preserved. This changes the requested entry mode,
not an instruction that directly elevates an EL1 caller.

Candidate directory: `out/anne-device-private/fastboot-el2-candidate-v1/`.
The raw image is `fastboot-el2h.ANALYSIS-ONLY.img`, SHA-256:
`d236eff237e57f31be3c528e79c7e4386674d1d85e568b395ddb35a29612b46a`.
The directory also contains the manifest, independent assembly check,
disassembly of the changed handoff, and command-gate evidence. It is ignored
by Git and has never been flashed. It has no newly valid Huawei signature or
verified VRL/update packaging; preserving its size does not make it a flash
package.

Validation used Apple's Clang targeting `aarch64-none-elf` to independently
assemble both instructions, LLVM to disassemble the candidate, an exact source
hash/size check, and a whole-image comparison confirming only the two intended
bytes changed. These are static checks, not a successful EL2 boot.
The [TF-A entry-point layout](https://github.com/ARM-software/arm-trusted-firmware/blob/master/include/export/common/ep_info_exp.h)
and [AArch64 mode definitions](https://github.com/ARM-software/arm-trusted-firmware/blob/master/include/arch/aarch64/arch.h)
support the field and mode interpretation.

## Why the current fastboot cannot flash itself

The private capture `el2-firmware-inspection/recovery-dmesg-private.txt`
records these bootloader observations, independently repeated in the restored
native-entry probe capture:

```text
secureboot: The product is secure!
hwdog_certify.lock_state.fb_lock_stat = 0
hwdog_certify.lock_state.user_lock_stat = 1
hwdog_certify.lock_state.check_frpopen_stat = 1
```

These are saved observations, not a fresh lock-state query. Their meanings
are confirmed by the exact binary's lock-state reporting function at `0x2afa0`:
state offset `+0x214` controls the FB LockState line and `+0x218` controls the
USER LockState line; value 1 prints UNLOCKED. The saved values therefore mean
FBLOCK locked and USERLOCK unlocked.

The `fastboot_ctrl` descriptor at `0x25c378` points to operations at `0x25c398`.
Its command-permission operation at table offset `+0x10` is `0x2abb8`.
The USB dispatcher invokes it at `0x2154c`; a zero result reaches
`FAILCommand not allowed` at `0x215d4..0x215dc` before command execution.

The permission function has two relevant paths:

- `fb_lock_stat == 1` at `0x2abd0..0x2abd8` permits dispatch without this
  allowlist restriction. This does not prove subsequent image checks pass.
- Otherwise USERLOCK/FRP determine which allowlist is used. For the recorded
  unlocked USERLOCK and open FRP state, `0x2ac94..0x2acc8` scans the 58 string
  pointers at `0x25c038..0x25c208`, loaded via the cell at `0x292088`.
  The list includes `flash:kernel`, `flash:ramdisk`, `flash:vbmeta`, and recovery
  commands, but no `flash:fastboot`, `flash:xloader`, or `flash:trustfirmware`.
  No listed command prefix matches any of those three requests.

The general `flash:` handler exists: command entry `0x25b820` points to
`0x3a078`, whose normal path calls `0x21d60`. Its existence does not bypass
the earlier permission check. The underlying writer also invokes secboot's
download-verification operation at `0x21f1c` via `0x219a0`; the secboot
operation resolves to `0x127ec4`. The USERLOCK-unlocked branch at
`0x21b00..0x21b10` can tolerate that verification failure in this routine,
which helps explain custom kernel flashing. It runs after the command gate
and is not evidence that xloader permits unsigned fastboot on the next boot.

## What could make this route viable

The [PotatoNV author's documentation](https://github.com/kitsuned/PotatoNV)
explicitly supports ANE with the Kirin 65x (A) loader and distinguishes FBLOCK
from normal USERLOCK unlocking. It documents loading factory board software
into RAM through USB download mode and changing FBLOCK to allow secure
partition writes. This supplies a device-specific lead for removing the
command gate, usually requiring a physical testpoint. It does not establish
that a patched persistent fastboot image authenticates or that a modified
RAM-loaded payload is accepted. The factory loader is also not automatically
a replacement boot chain for Ame.

The [authors' Huawei secure-boot research](https://i.blackhat.com/USA21/Wednesday-Handouts/US-21-Komaromy-How-To-Tame-Your-Unicorn-wp.pdf)
describes authentication before fastboot and separately demonstrates modified
fastboot execution after earlier-stage verification is bypassed. Its tested
Kirin 710/980 exploit details are not an ANE/Kirin 659 procedure. They support
investigating xloader/VRL acceptance rather than assuming a partition write
disables secure boot. This handset's exact preceding-stage implementation
still requires a matching dump.

The next useful evidence is a matching **read-only xloader and VRL/VRL-backup
capture**, followed by tracing fastboot authentication and its lock-state
dependencies. If a supported RAM-loading path can execute the modified code,
use that for an initial retained-entry trace before considering persistent
replacement. A stock image backup alone cannot restore a phone whose modified
fastboot fails before its USB recovery interface starts.

Even after successful loading, the one-instruction candidate remains a probe:
BL31 might validate or replace the supplied SPSR. EL3 must also provide usable
HVC access; the [Linux AArch64 boot protocol](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html)
requires `SCR_EL3.HCE=1` for an EL2 handoff and consistent CPU entry levels.
Check Ame's existing `boot_entry_el` and `monitor_ready` evidence on every CPU
that will run a vCPU, then execute the signed hello guest. The native memory
limit documented in [EL2.md](EL2.md) also needs resolving for the current
128 MiB guest. No VM success is claimed from the offline candidate.
