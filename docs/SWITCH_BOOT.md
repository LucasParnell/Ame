# Switch boot investigation — 2026-09-22

Investigated `min-vm-switch` at `79b45aa` and the mounted test SD card. Both
previous corrected packages still retained the Hekate IPL splash on hardware.
The user confirmed an original V1 Switch: Hekate's menu works, and selecting Ame
completes a loading bar before the freeze. The handoff probe also retained the
splash. Subsequent disassembly identified an incompatible BL31 link address;
the confirmed defect and replacement build are described in the latest section.

## Confirmed boot package mismatch

The installed `ame/l4t/bl33.bin` identifies itself as
`U-Boot 2024.NX02.b201801 (Jun 01 2024 - 18:37:55 +0000)`, SHA-256
`b492a4017580ca11c9cd49075a899a65f62069d3750615217fc10308954c9faa`.

Its binary contains the `source` and `bootm` command names, but no `sysboot`
command name and no `booti` command. The only `sysboot` reference is inside the
default environment's extlinux scanner. An environment variable naming a command
does not mean that the command was compiled. The
[Switchroot defconfig](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/linux-hekatf/configs/nintendo-switch_defconfig)
also explicitly disables `CONFIG_CMD_BOOTI` and enables `CONFIG_CMD_BOOTM`.

The SD card's Ame entry used `extlinux/extlinux.conf` and `LINUX /ame/boot/Image`.
It therefore depended on both missing commands. `system.uInitrd` was already
selected on this branch; the missing bundle fix in `16e0c44` was present.

The corrected package has one board script, `boards/switch-t210/boot.cmd`,
wrapped as `ame/l4t/boot.scr`. U-Boot's existing distro script scanner executes
it with [`source`](https://docs.u-boot.org/en/latest/usage/cmd/source.html).
The script checks each file load and invokes
[`bootm`](https://docs.u-boot.org/en/latest/usage/cmd/bootm.html) with the generated
kernel uImage, signed bundle uInitrd and DTB. The kernel's load/entry address
comes from the linked ELF and board metadata. Staging addresses come from
U-Boot's environment; the old `kernel_addr_r` override equating staging with the
kernel entry is removed to avoid overlapping the wrapper with its destination.

The installer removes only Ame's obsolete extlinux entry. Hekate firmware path
comments are corrected: [Hekate loads BL31/BL33](https://github.com/CTCaer/hekate/blob/master/bootloader/l4t/l4t.c)
from `boot_prefixes` before switching to `bootloader/sys/l4t/` for shared firmware.
Both copies existed on this SD card, so this documentation error was not the
observed card's blocker.

## Validation and limits

- Switch kernel and userland rebuild, board-image validation and package staging pass.
- All 43 host regressions pass. The corrected package was installed on the test
  SD card and every installed file compared byte-for-byte with staging. Replaced
  files are backed up under `_ame-backup-20260922-204712-bootfix/` on that card;
  the obsolete Ame extlinux entry is removed. Filesystem writes were flushed.
- Independent decoding verifies the generated legacy headers, component length
  table, payload sizes and CRCs. U-Boot script images need the length table;
  prepending just a legacy header to script text is invalid.
- Executing the board script with instrumented loader commands verifies that
  failure of any of the three loads prevents kernel handoff.
- The SD card's preallocated `ame/log/boot.log` contained only zeroes. The current
  kernel initializes that sink after its first UART output, and flushes later;
  an empty file is not proof that the kernel entry was never reached.

Next physical check: launch the corrected Ame entry from Hekate. If the splash
still remains, distinguish whether it occurs before Hekate's menu or after
selecting Ame, and capture UARTA if available. That kernel's first LCD
paint follows UART output and SD initialization, so lack of that paint does not
by itself distinguish a loader failure from an early kernel failure. No physical
boot success is claimed by these host checks.

## Second device attempt: kernel logging bypass

The user retried the package and still saw the Hekate IPL splash. The remounted
card retained the corrected script; `boot.log` remained entirely zero-filled.

Inspection of the linked Switch kernel revealed that `anko::log` called the
public `drivers::uart::putchar` alias in `pl011.cc`, bypassing `console::bind`.
That PL011 driver retained its QEMU address, `0x09000000`. Therefore the first
`anko booting` log accessed QEMU MMIO on Tegra before reaching SD logging or LCD
diagnostics. Its panic output used the same incorrect path. This is a confirmed
code defect consistent with the symptom; there is still no device trace proving
how far this particular attempt reached.

All kernel character output now uses `console::putchar`, the existing ops-table
dispatch with the secondary sink. The direct PL011 entry points and unused
duplicated board early-console implementations are removed. PL011 no longer
has a default physical address: its address is supplied only when bound.
The console is rebound after DT discovery, and the existing LCD progress paint
now precedes SD initialization. Hekate passes `stdout=serial,vidconsole` and
`stderr=serial,vidconsole` so the U-Boot script's progress and errors can appear
on the screen as well as UARTA.

Regression coverage uses the real UART drivers with separate simulated register
banks and verifies secondary-sink delivery and rebinding under ASan/UBSan. A
second check compiles the actual AArch64 logger and rejects direct driver
dependencies. The original logger object imports `drivers::uart::putchar`;
the corrected object imports `console::putchar`. The Switch rebuild and full
default QEMU gate pass, including 1/2/4-CPU lifecycle, restart and persistence.

Installed this second build on the test SD card, verified all eight staged files
against the installed copies and flushed writes. The previous attempt is saved
under `_ame-backup-20260922-205752-consolefix/`. The next physical launch should
show U-Boot progress text if the script runs; Ame's blue diagnostic precedes SD
initialization and white follows GIC/timer initialization. Physical boot remains
unconfirmed until the user retries.

## Third attempt: isolate the ATF to BL33 handoff — 2026-09-23

The second build also retained the splash. The remounted card still contained
the expected kernel and unchanged BL31/BL33 firmware. Hekate's on-card update
metadata identifies version 6.5.3. The load bar finishing does not establish
that U-Boot or Ame started. U-Boot imports Hekate's environment during its boot
scan, so the `stdout` setting cannot expose failures before that point.

An optional `Ame handoff probe` entry replaces BL33 for that entry alone. It
reuses the exact installed BL31 and shared firmware. Its small AArch64 payload
checks for the expected EL2/MMU-off/cache-off entry state, supplies its own
stack, reads the active DC0 window, paints it **magenta**, and deliberately
halts. It does not initialize UART, SD, userland, or the kernel. Hekate's BL33
header includes an in-image zero sentinel at the DTB offset, preventing the
loader's U-Boot-specific DTB patches from touching arbitrary memory.

The diagnostic and kernel share `midix/hw/tegra_dc.h` and libk's framebuffer
validation/fill operations. This also fixes the previous kernel probe's guessed
`0xC0000000` fallback and incomplete bounds checks. The reader explicitly selects
the **active** window registers, restores the selectors, and accepts only enabled,
tightly packed 32-bit pitch scanout with a complete span inside DRAM. Unsupported
layouts remain unavailable. DT simple-framebuffer discovery now checks format,
stride, allocation size and DRAM bounds as well.

Build and stage:

```sh
scripts/build-hekate-package.sh --handoff-probe
python3 scripts/test/switch-handoff.py build-switch
```

Install on a mounted card that already has `ame/l4t/bl31.bin`:

```sh
scripts/build-hekate-package.sh --no-configure --no-build --handoff-probe "/Volumes/NO NAME"
```

The installer copies that BL31 into `ame/handoff/`; it does not replace the normal
entry's firmware. Select **More Configs → Ame handoff probe**. A solid magenta
screen proves ATF reached the diagnostic BL33 and its framebuffer write reached
scanout. The halt is intentional; power off afterward. An unchanged splash is
still inconclusive between an earlier handoff failure and an unavailable or
unsupported display state. This test alone cannot prove that U-Boot or Ame boots.

Validation executes the actual payload in QEMU with register fixtures in RAM:
valid packed scanout is painted, unsupported tiled scanout is untouched, and
selector values are restored. This is CPU/memory validation, not Tegra emulation.
ASan/UBSan host coverage rejects overflow, truncated spans, invalid dimensions,
disabled windows and unsupported layouts. Physical acceptance remains pending.

During validation, `ame_host_tools` was also corrected to put the project Python
environment ahead of Homebrew's global Python. A macOS PATH regression test
covers selection when both interpreters are present.

The Switch rebuild, all 47 host regressions and the complete default QEMU gate
pass (direct boot, 1/2/4-CPU lifecycle, supervision/restart and persistence).
Installed and byte-verified all 11 staged files on the remounted card, confirmed
the normal entry's BL31/BL33 hashes were unchanged, and flushed filesystem
writes. Previous files are backed up under
`_ame-backup-20260923-110432-handoff/`. The installed diagnostic SHA-256 is
`4740f2f3a32e37ea97f49f5bd0b1a7ccf628a9c11a748dd590e2940e07bb94a6`.

## Fourth attempt: confirmed BL31 link-address mismatch

The user ran the handoff probe and still saw the splash. Inspection then found
that the installed BL31 is built for **0xFF800000**, whereas the standard
[Hekate 6.5.3 SECFW configuration](https://github.com/CTCaer/hekate/blob/v6.5.3/bootloader/l4t/l4t.c)
loads it at **0xFFF00000** and passes that address as `tzdram_base` with a 1 MiB
size. This is independent of the kernel and framebuffer probe.

Evidence from the installed 74,719-byte BL31 (SHA-256
`40b37cf1c7baff6dbcaaa373daaf23daffa5ca23c4769abf13b4cc27a5f18ede`):

- At file offset `0x14e0`, its reset handler loads `x17 = 0xFF800000` and
  relocates itself there when entered at another address.
- At `0x1e80`, early platform setup compares the supplied secure-memory base
  against `0xFF800000`; mismatch enters the panic path.
- That check precedes console initialization and the BL33 handoff. It explains
  how both U-Boot and the replacement probe can remain silent.

The [reviewed Switchroot source](https://gitlab.com/switchroot/bootstack/switch-atf/-/blob/8902c539ce83b15637c5f47fc8aff5d14d95b992/plat/nvidia/tegra/common/tegra_bl31_setup.c)
contains the corresponding check. `scripts/build-switch-atf.sh` builds that
pinned commit using the checksum-verified archive and the board profile in
`boards/switch-t210/firmware.json`. The build keeps assertions and stack
protection enabled. It validates the ELF entry, all loadable segment spans and
the raw image's reset-handler address, and records source, patch, compiler and
binary provenance alongside the result.

The small reviewed source patch also parameterizes TZDRAM size and fixes three
range-check defects: 32-bit overflow of the secure aperture end at 4 GiB,
overflow in caller-supplied range lengths, and failure to reject a one-byte
overlap at the first secure address. Each firmware build compiles and executes
the actual patched range-check function and platform macros under ASan/UBSan,
including boundary and overflow cases. These are firmware correctness fixes,
not changes to the security checks to permit an incompatible image.

The package preflight now decodes the known ATF reset-handler sequence. It
rejects a mismatched address or unknown/ambiguous code before installation.
This is a compatibility guard, not authentication of arbitrary firmware.
`--bl31 FILE` explicitly stages a replacement for Ame; the diagnostic receives
the same validated firmware. U-Boot and Hekate's shared firmware stay as installed.

The old DTB reserved memory at `0x17C000000`, which did not cover the firmware
below 4 GiB. Reservations now come from the same board profile and cover
`0xFFD00000–0xFFFFFFFF`: GPU firmware/WPR, SECFW/BPMP/SC7 and BL31. Reserving those
hardware-owned areas is necessary even with GPU drivers disabled.

Reproduce the replacement build and package:

```sh
scripts/build-switch-atf.sh
scripts/build-hekate-package.sh --no-configure --no-build --handoff-probe \
    --bl31 build-switch/atf/bl31.bin "/Volumes/NO NAME"
```

The corrected BL31 SHA-256 is
`24b188a3aba5ffae0d54fd9e8a7c3cba9730d309b7bab8ba870679a1c5b4e1b4`.
The firmware builds and its range tests pass; package regressions reject the
previous binary and verify the generated DTB reservations. Physical boot success
still requires a new device run with this firmware.

All 51 host regressions pass. Installed and byte-verified 12 package files,
including the corrected BL31 for both Ame and the handoff probe, and flushed
writes. U-Boot and every shared `bootloader/sys/l4t/` firmware file retain their
previous hashes. Backup: `_ame-backup-20260923-163002-atf-layout/`.
Next device check: rerun the handoff probe, then power-cycle and launch Ame if
magenta appears. Report the visible result of each entry; host checks do not
establish physical boot success.
