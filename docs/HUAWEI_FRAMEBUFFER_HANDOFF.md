# Huawei P20 Lite: handoff for native framebuffer bring-up

Current device state: TWRP is connected and the full stock kernel checksum
is verified after v5. Two identical DFX records prove 60 seconds of healthy
runtime with eight CPUs ticking and 67 watchdog feeds. The later watchdog FIQ
was deliberately requested after observation for firmware trace retention.
The inherited watchdog timeout is addressed by keepalive, with 42 local checks
passing. Normal v5 is packaged and unflashed; indefinite uptime is unverified.
Controller and acceptance: `out/anne-device-private/watchdog-v5-test/`.
See [FIQ evidence and watchdog implementation](../boards/huawei-anne/FIQ.md).

Follow-up: [native framebuffer capture](../boards/huawei-anne/FRAMEBUFFER.md)
records the completed TWRP ioctl inspection and new opt-in full-kernel capture.
The recovery framebuffer reports IOVA `0x40000`, ARGB8888, stride 4320 and
active y-offset 2280; its physical backing remains unresolved. Use capture
package `out/anne-framebuffer-capture-package-v1a/` and its fresh private
controller. The user physically confirmed `ANKO EL1` on a green screen, with
slight text clipping at the top, and automatic reboot. Four bounded DFX records
confirm D0 scanout at physical `0x5d000000`, ARGB8888, 1080 × 2280, stride 4320,
eight completed workers, services ready and LPM3 reset, without faults or panics.
TWRP and the stock shared-kernel SHA-256 were verified after that capture.

Updated 12 October 2026. The **native framebuffer first-light and clipping/console
objectives are complete**. Clipping and boot/console integration are physically
confirmed with `out/anne-console-capture-package-v1/`; its fresh
controller is `out/anne-device-private/console-v1-test/control.py`. The normal
console package was subsequently flashed for the requested demonstration;
the console was visible and reached readiness, then faulted on FIQ vector 6.
Sustained runtime remains unverified. The LED fix and
eight-core Ressha bring-up are complete. Keep the native Anko
route; QEMU inside TWRP was an earlier evaluation, not the requested runtime.

## Proven on the phone

- ANE-LX1, firmware `ANE-LX1 8.0.0.151(C782)`, Kirin 659, unlocked bootloader.
- Native EL1 Anko reaches MMU enable, interrupts, authenticated MXB, EL0 init,
  nameservice, console and `BOOT 36 services.ready`, without faults or panics.
- All eight CPUs (`0,1,2,3,100,101,102,103` physical affinities) execute pinned
  Ressha workers on their assigned cores. All cores have positive timer, context
  switch, preemption and handled-IPI counts. Seven PSCI CPU_ON calls return zero.
- Native LED colours are now physically confirmed: **red, blue, green, reboot**.
  Green is selected only after all selected services report readiness.
- LPM3 NMI reset works; five ramoops records survive that reset and can be
  collected through TWRP after restoring the stock shared kernel.
- Native framebuffer shows a recognisable `ANKO EL1` label and green pattern.
  D0 (channel 6, offset `0x50000`) is the enabled inherited linear scanout; the
  earlier D2/D3/V1-only probes missed it. Four framebuffer traces survived in
  Huawei DFX, even though this recovery's pstore directory was empty.
- The inset heading and scrolling boot/service console are physically confirmed.
  Two matching 35,667-byte DFX copies confirm early-log replay, 4,269 rendered
  characters at readiness, all eight workers and intentional readiness reset.
  Two copies of a separate execution initialize the console and complete the
  workers, but hit the ten-second capture deadline before service readiness.
  No fault or panic is recorded in either execution.

This proves bounded startup, the eight-worker test and native framebuffer
console output. Repeated-startup reliability, long-term stability, live native USB, charging and other phone
drivers remain unverified.

The next subsystem plan is [userspace USB host/input and native debugging](USB_PLAN.md).
It covers a generic bus/transfer interface, the Hi6250 DesignWare OTG backend,
HID/input/terminal work and a separate device-role gadget interface.

## Console artifacts

| Artifact | Purpose |
| --- | --- |
| `out/anne-console-capture-package-v1/` | Physically tested inset console, eight-worker capture and bounded reset; run tag `anne-console-v1` |
| `out/anne-device-private/console-v1-test/results/` | Four tagged DFX copies from two executions, exact-ELF decode, full stock verification and hardware evidence summary |
| `out/anne-native-console-package-v1/` | Archived normal demo: console/readiness confirmed, then FIQ fault; stock restored |
| `out/anne-device-private/console-normal-demo-v1/` | Pinned demo controller/state, two decoded fault copies and stock checksum verification |
| `out/anne-console-validation/el1-console.png` | Actual QEMU-rendered pixels showing the inset heading and readiness logs |

Console capture image SHA-256:
`220e2afc5f62f4d3148191490ecc932affa80c9ad2ab1d04d353a4f0bc634c49`.
Normal console image SHA-256:
`ec2314a3a43783c6a7a79aca6054df581e33811ab4896126d0eed215fef5a7f0`.
Both packages preserve their exact ELF, raw Image, manifest and stock restore.
The capture ELF is
`0f0b12506b53fd897bccbd6f20b05a70cd2335abc2dedabd165f37294c1b609c`.
Do not rebuild or substitute an ELF when decoding these records.

## Latest LED correction and artifacts

`boards/huawei-anne/debug_io.h` previously ORed `0x3f` into PMIC register `0x14e`
and cleared only the selected channel pair. Unselected output-control pairs
became 3; selecting green yielded register value `0x33` and physical purple.
The corrected sequence clears channel enables in `0x14d` and returns all six
output-control bits in `0x14e` to zero. It preserves unrelated upper bits and
uses the stock driver's timing/current settings. The same phone now displays
the correct colours. The archived minimal/v6 probe is unchanged.

PMIC base `0xfff34000`; byte register accesses at `base + (register << 2)`.
LED control/output `0x14d/0x14e`; currents `0x14f/0x151/0x153` = `7/1/7`;
start delays `0x150/0x152/0x154` = 3; shared timing `0x155/0x156` = `0xf0/0x33`.
Reference: matching public
[EMUI 8 LED driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/lineage-20/drivers/leds/hisi_leds.c).
Downloaded sources and PMIC field definitions are in `out/anne-smp-research/`.

| Artifact | Purpose |
| --- | --- |
| `out/anne-smp-capture-package-v3/` | Physically tested LED-corrected eight-core capture; resets at readiness |
| `out/anne-device-private/smp-led-v3-test/` | Final controller, state, physical observation, restore and readback evidence |
| `out/anne-native-smp-led-package-v3/` | Normal eight-core image, nameservice + console, capture reset disabled; this exact normal image has not been flashed |
| `out/anne-smp-capture-package-v2/` | Earlier successful native eight-core capture, with purple LED bug |
| `out/anne-device-private/smp-capture-v1-test/results/` | Earlier five decoded eight-core records |

Final capture image SHA-256:
`29b68ea01c854587c51693dd2c348f86921c209ceb6a5d580c0c4bbc2cff728d`.
Normal image SHA-256:
`476fbe9790ea6579d01b272481b1fa5ff74f93f1c9aecbaf695d1fd05053ab63`.
Each package includes the exact matching `kernel.elf`, raw Image, manifest and
stock restore image. Never decode a tested run using a subsequently rebuilt ELF.
Old `out/anne-native-logged-package/` is a pre-SMP one-core archive.

## Device state and recovery

The latest console capture is **physically confirmed, decoded and restored**.
`out/anne-device-private/console-v1-test/state.json` records that completed capture.
Its full stock shared-kernel SHA-256 matched the backup in TWRP. Subsequently,
the user requested a normal console demonstration; it reached readiness, then
faulted. Stock has been restored and checksum-verified in TWRP. Its final state is
`out/anne-device-private/console-normal-demo-v1/state.json`.
The final LED run physically confirmed red, blue, green and self-reset, but
its recovery readback retained no AMEANKO1 records. Use the earlier v2 five-record
capture for the eight-core execution evidence; do not claim final v3 log retention.

The `kernel` partition is shared by Android and TWRP. Always restore stock
before attempting TWRP. Verified stock backup:
`out/anne-device-private/boot-backups/kernel.img`, 24 MiB, SHA-256
`ad00caaa84c53d3ce478e562a6a1116ab720b2f0fce565c518928476b55cc268`.
Recovery block path: `/dev/block/platform/hi_mci.0/by-name/kernel`.
TWRP is installed in `recovery_ramdisk`, version `3.7.0_9-0`, root ADB,
Linux `4.4.23+`, SELinux Enforcing. No userdata was erased.

Tools:

- ADB: `out/host-tools/platform-tools/adb`.
- Working fastboot: `out/host-tools/platform-tools-35.0.2/fastboot --unbuffered`.
- Read private serial from `out/anne-device-private/adb-targets.json`; do not
  print it. Do not include serials or raw bootloader logs in public documents.
- `out/anne-device-private/reset-phone-usb.py` resets only the matched phone's
  USB interface. Use before fastboot OEM/flash/reboot; never during a write.
- Latest pinned controller: `python3 out/anne-device-private/console-normal-demo-v1/control.py`
  with `preflight`, `flash`, `restore`, `reboot`, or `bootloader_reboot`.
  Do not reuse its successful run state for a new image; make a fresh controller
  and pin its native/restore hashes before a new test.

After stock flash, use **`fastboot reboot bootloader`**, then allow **20–60 seconds**
for installed TWRP to enumerate. This is the user-confirmed route on this phone.
`fastboot reboot recovery` enters eRecovery. Fastboot RAM `boot` is rejected
(`Command not allowed`), and `fetch` is unsupported. Reported partition size 0
is a bootloader query limitation, not actual size. Packaged AVB metadata is
locally verified; the footer-copy warning during full-image flash is expected.

TWRP `adb reboot bootloader` has repeatedly returned to TWRP this session.
Give transitions time before judging transport. If needed, user enters fastboot:
USB connected, hold Volume Down + Power through restart, release Power and keep
Volume Down held until FASTBOOT. To enter TWRP with buttons: unplug USB, hold
Volume Up + Power, release Power at Huawei logo, keep Volume Up until TWRP,
then reconnect USB. Trust the user's physical observations.

USB commands require sandbox escalation. The user has authorized native tests
and stock restoration; ask only for missing physical help, not repeat permission.
No subagent delegation is authorized. Preserve unrelated dirty repository files.

## Native diagnostics to reuse

Start with `boards/huawei-anne/BOOT_DIAGNOSTICS.md` and `SMP.md`.
Trace tag `AMEANKO1`; logs include boot stage, CPU ID, constructors, kernel and
console output, faults/panics, worker completion and per-core Ressha statistics.
Per-CPU fault stacks and recursion guards are installed early. A trace guard
serializes complete messages only after MMU/normal-memory setup.

Ramoops reserved 1 MiB at `0x34800000`: console +`0x60000` size `0x80000`,
pmsg +`0xe0000` size `0x20000`, dmesg at +`0`, +`0x20000`, +`0x40000`, each
`0x20000`. Records are cleaned to memory at checkpoints and before reset.
The actual counter runs at **1,920,000 Hz**, although CNTFRQ reports 16384.
Reset is SCTRL `0xfff0a000 + 0x510`, set then clear bit 2 with barriers.
Physical button reset previously lost logs; prefer a bounded LPM3 self-reset.

Collect only after stock restore and TWRP startup:

```sh
python3 scripts/anne/collect-native-log.py \
  --package out/anne-console-capture-package-v1 \
  --output out/anne-device-private/console-v1-test/results --dfx-fallback
```

The collector verifies TWRP, the exact stock partition SHA and matching ELF,
then pulls available pstore records. If none match, `--dfx-fallback` reads only
the diagnostic DFX backup and extracts bounded records with the package's run
tag. Older first-light/SMP traces cannot be mistaken for the console image.
ADB get-state reports `recovery`, not `device`. `--offline --dfx-fallback`
re-decodes the saved backup without touching the phone.

## Framebuffer: evidence and first objective

First objective: draw a recognisable pattern or text **from native Anko** and
retain display-stage logs, then self-reset for readback. A full UI is not needed.
This objective is now complete; see the linked framebuffer capture for the
verified D0 register state and bounded DFX extraction procedure.

- Actual running DTB: `out/anne-device-private/recovery-inspection/observations/firmware.dtb`
  and `firmware.dts`; SHA `0052d5d005e2cac6212f22fe616a669eba5f562eb78dcc29a82ea0668febb517`.
- Panel: **BOE_OTM1911A 5P84 VIDEO TFT**, physical **1080 × 2280**.
- Recovery `hisifb0` reports virtual **1080 × 4560**, stride **4320**, 32 bpp.
  Its physical framebuffer address has not been established. Virtual double-height
  is not the panel's visible height.
- DSS register base **`0xe8600000`**; LDI0 offset **`0x7d000`**.
- Observed reserved boot-logo pool **`0x5d000000` + `0x03000000`** (48 MiB).
  This reservation does not by itself establish a live scanout buffer.
- Earlier probes inspected D3/D2/V1 RDMA offsets `0x53000/0x52000/0x28000`.
  All returned `paint=skip`: no valid inherited linear display channel.
  Nonzero LDI geometry/control alone did not establish a usable scanout path.
- `boards/huawei-anne/minimal/framebuffer.h` has cautious register validation,
  pixel conversion and text painter; `minimal/main.cc` has existing register
  snapshots. `entry-probe/display.h` is an older, stricter validator.
- Existing reusable display paths: `kernel/platform/probe.cc`,
  `kernel/include/anko/hw_info.h`, `userland/drivers/framebuffer/main.cc`,
  `lib/include/libk/framebuffer.h`, `tests/anne_display_probe.cc`,
  `tests/anne_minimal_framebuffer_probe.cc` and framebuffer tests.

Completed bring-up sequence:

1. In TWRP collect read-only fb ioctl/sysfs geometry, stride, pixel format,
   buffer physical address if exposed, panel timing and relevant DSS state.
   Compare against the actual DTB and matching EMUI 8 display driver sources.
   Stock has no `/dev/mem` or enabled CONFIG_DEVMEM/KPROBES; ordinary PMIC debug
   regmap is absent. Avoid assuming arbitrary MMIO access exists in recovery.
2. At native entry capture the needed DSS, DSI, clocks/reset and panel state
   into the existing retained logger. Map MMIO as Device before post-MMU reads;
   log precisely which checks reject inherited scanout. Examine the complete
   channel/overlay path rather than disabling validation to make `paint` pass.
3. If a valid inherited scanout is found, use its verified format, address and
   stride, preserve reservations, draw and clean framebuffer memory for display
   DMA. If it is absent, implement the minimum observed DSS/panel setup needed
   to drive this panel; do not assume a writable logo pool alone is sufficient.
4. Build a fresh bounded capture with exact ELF and verified restore image.
   Show the pattern, leave it visible long enough, self-reset, restore stock,
   return to TWRP and decode logs. Preserve working eight-core and LED paths.
5. After visible native output is proven, wire the framebuffer into the normal
   console/service profile. GPU acceleration and live USB can wait.

## Build and validation

```sh
cmake --preset anne-console-capture-dev
cmake --build --preset anne-console-capture-dev --target anko provenance
python3 scripts/anne/build-native-package.py --build out/anne-console-capture-dev \
  --output out/anne-console-capture-package-new
```

The console capture preset includes `test.anne_smp`, a ten-second runtime capture
deadline and five extra seconds of console viewing after the readiness LED
hold. Normal preset `anne-native-dev` selects nameservice + console, enables
the framebuffer console and uses capture_seconds=0. The exact normal console
package was flashed for demonstration, reached readiness and then faulted on
FIQ vector 6. Preserve it and its exact ELF as fault evidence.

Console validation passes 17 focused host checks and 15 instrumented QEMU
checks, including actual glyph decoding, inset margins, scrolling, early replay,
service readiness, EL1/EL2, eight workers and invalid inherited IOVA rejection.

136 host tests passed during SMP work. After the LED fix, all nine instrumented
QEMU trace tests passed again (EL1/EL2, eight workers, timer/IPI statistics,
readiness-only green with output register zero, injected faults and capture).
Four-core lifecycle and supervision regression matrices also passed. Physical
eight-core and LED evidence is separate from the QEMU tests.
