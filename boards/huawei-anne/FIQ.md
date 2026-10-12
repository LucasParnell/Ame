# Native runtime FIQ investigation

Updated 12 October 2026. The earlier normal console images faulted on FIQ
vector 6 because the inherited watchdog expired. V5 verifies 60 seconds of
healthy runtime with keepalive, followed by deliberate expiry for retention.
Stock is restored and its full checksum verified in TWRP. Latest evidence is
`out/anne-device-private/watchdog-v5-test/results/acceptance.json` and its
matching raw/decoded records. The keepalive passes 21 focused host checks and
21 instrumented QEMU checks. Normal v5 is packaged but remains unflashed;
indefinite uptime has not been verified.

## Recovered evidence

- Normal v1: two identical DFX copies show readiness, followed by vector 6 at
  `0x483a5c`, the DAIF restore in `kernel::LockGuard::~LockGuard()`.
- Normal v2: four tagged DFX copies represent two executions. Both reach
  readiness and then take vector 6 on CPU0. PCs resolve to `libk::memcpy` at
  `0x4eb474` and the native trace `IrqGuard` constructor at `0x4cead0`.
- V2 records first userspace SVC state `0x40`. The fault snapshots are
  `0x80000385` and `0x60000385`. Their F bit is clear; why that state was reached
  remains unresolved. Do not claim that the first userspace mask proves the
  interrupted kernel state stayed masked.
- ESR `0x56000000` is retained stale synchronous state for this FIQ, not evidence
  of a new SVC fault. The collector now records vector/kind and marks its
  syndrome invalid. FAR zero does not establish a null-pointer fault.
- The different interrupted PCs do not identify the hardware/firmware trigger.
  A successful short capture does not establish sustained native operation.

Preserve both failing images and their exact ELFs:

| Package | Image SHA-256 | ELF SHA-256 |
| --- | --- | --- |
| `out/anne-native-console-package-v1/` | `ec2314a3a43783c6a7a79aca6054df581e33811ab4896126d0eed215fef5a7f0` | `cdaf374a2947c30ccf2d80a3666375a5b7cbf3fd14d8ecc696af857bb1f64147` |
| `out/anne-native-console-package-v2/` | `8455626e4fa63b639b77ab49ebe5b007ea00025da2bb326a927060e1ef4b76aa` | `6ee62c056919c53629b4e9ee84bf8f935cfe208698283925bf8243359620f365` |

## Targeted source capture

The matching Huawei kernel enables `CONFIG_HISI_FIQ`. Its EL1/EL0 FIQ entries
call `fiq_dump`, which invokes `dmss_fiq_handler` and performs fatal diagnostics.
That handler reads exactly one eight-byte normal-world source word at
`HISI_SUB_RESERVED_BL31_SHARE_MEM_PHYMEM_BASE = 0x34a21000`.
`hisi_ddr_secprotect.h` identifies low-byte reason 76 as watchdog and 163 as DMSS
DDR security; the DMSS flag is `0x5a5a5a5a5a5a5a00`.

The actual DTB reserves `0x34a00000 + 0x78000` as `fka-mem`, and its
`bl31-kernel-shared-memory/dmss-int-mem` occupies the first `0x400` bytes of the
vendor shared area. This supports reading the named diagnostic word. No DDR
security configuration, watchdog control, efuse, DRM or other shared-buffer
contents are read or changed by this probe.

The new probe logs `FIQ_SOURCE` at entry, after MMU setup and on FIQ, including
the full word and counter tick. Its page is reserved and every kernel alias is
Normal Non-Cacheable, including process address spaces. QEMU uses a separate
reserved fixture page at `0x47400000`. Initial testing caught a conflicting
Device/RAM alias in the synthetic process maps; the mapping policy was corrected
before any phone flash.

Build directory: `out/anne-fiq-source-v3-dev`, based on
`anne-console-runtime-capture-dev`, run tag `anne-fiq-source-v3`. It uses the
normal nameservice/console profile and a 60-second deadline, with readiness
reset disabled. A fault resets early; otherwise the observation deadline resets
for recovery. It is diagnostic, not a proposed stable normal image.

Two identical tagged DFX records were recovered at offsets `0x1a0800` and
`0x220800`. Both reach service readiness with eight CPUs online, then take FIQ
vector 6 on CPU0. The source word is zero at entry and `mmu.done`, then becomes
`0x5a5a5a5a5a5a5a4c` at the fault: firmware's watchdog reason 76. Entry tick
`11489919` and fault tick `117534871` at 1,920,000 Hz place the fault about
55.23 seconds after native entry. The interrupted PC `0x4eb488` resolves to
`libk::memcpy`; the changing PC is incidental to the watchdog event.

Preserve `out/anne-fiq-source-capture-package-v3/`: image SHA-256
`9b482572940bbf30c28f1287bbb0f1596c237506916801cd8edcf7022531ac21`, ELF SHA-256
`30271997a0d9399d3bab55768d897bb3bf0d8f808b284b619e065f4437851cb7`.

## Inherited watchdog keepalive

The actual DTB declares `arm,sp805` watchdog0 at `0xe8a06000`, with a 60-second
timeout. The pinned Huawei driver pings by unlocking `WDTLOCK`, rewriting
`WDTLOAD`, relocking, then reading the lock register to flush posted writes.
Anko now adopts an already active, recognized SP805 and reloads its inherited
load once a second on CPU0. It preserves control, interrupt state and clocks,
and refuses unsupported, inactive, expired or externally changed state.

Adoption runs before the initial LED delay and survives BSS/constructor setup.
Early checkpoints and LED holds feed during boot; CPU0 timer and syscall paths
feed during runtime. The watchdog MMIO page is mapped Device in every native
kernel/process address space. Synthetic QEMU uses a reserved Normal NC fixture
page; the host register model verifies counter reloads and rejected states.
`WATCHDOG` records expose the inherited ID, control, load, raw status and feed
count. `ANKO_ANNE_WATCHDOG` defaults on for the Huawei board.

The v4 bounded capture uses the normal service profile and a 60-second runtime
deadline with readiness reset disabled. Acceptance requires readiness, eight
active CPUs at the deadline, `CAPTURE reason=runtime.observed`, an active
watchdog with repeated feeds, unchanged load/control, and no faults or panics.
Passing this capture establishes a bounded observation, not indefinite uptime.

The v4 readback contains no matching run tag or `WATCHDOG` line; DFX still holds
the earlier v3 records. This is a retention failure, not evidence that v4 did
or did not fault. Its verified stock restore and readback are preserved under
`out/anne-device-private/watchdog-v4-test/`.

V5 keeps feeding throughout the same 60-second observation window, records
runtime counters and live watchdog registers, then marks
`RETENTION watchdog.expiry deliberately_requested=1` and stops further feeds.
The inherited watchdog remains enabled and expires naturally while Anko keeps
running. This deliberately revisits the firmware path that retained the v3
fault trace. Acceptance requires a complete healthy observation before the
marker, repeated feeds, and no prior fault/panic. A later FIQ with watchdog
reason 76 is expected for this diagnostic. An unsupported/inactive watchdog
falls back to the ordinary NMI capture reset; it is never started by the probe.
The retention option defaults off and requires a positive deadline, keepalive,
and readiness reset disabled. Normal images keep feeding continuously.

| V4 artifact | Image SHA-256 | Exact ELF SHA-256 |
| --- | --- | --- |
| `out/anne-watchdog-runtime-capture-package-v4/` | `fcfabf05cade345477979d1b26ddf9318aef3ec2354d4653606002c8a5635e66` | `53090cb2bc8f279ef665f36c7e9d91904ad797f43b2dbfec73b5f551637d443e` |
| `out/anne-native-watchdog-package-v4/` (normal candidate, unflashed) | `8d3c2f44ff4d1e3b4238078c8d4d16557ea23232be8317d2ae592230186acccf` | `966c72929ae9e414966ba996388ab0aab8425b8802196cf602894194fe0f0e6b` |

The capture controller is `out/anne-device-private/watchdog-v4-test/control.py`,
with exact package/restore hashes in its fresh `state.json`. Restore the stock
shared kernel and verify its full checksum before TWRP readback. Decode only
the matching `anne-watchdog-v4` records with the archived capture ELF.

V5 package: `out/anne-watchdog-retention-capture-package-v5/`, tag
`anne-watchdog-v5`, image SHA-256
`71ca98eee28acdc740a8cf75aabee90be171bdf8ac5882dc4ecb119fdb9def88`, exact ELF
SHA-256 `d0e859eff88dda1439a9104445d6c191b1508c976c4d18c9a776105cb59246fb`.
Its fresh restore/readback controller is
`out/anne-device-private/watchdog-v5-test/control.py`.
Normal v5 candidate: `out/anne-native-watchdog-package-v5/`, tag
`anne-native-watchdog-v5`, image SHA-256
`c6f95bb848ba73128ee8e20fcf4076b4b008fbf27a50d2c2cd1ba7805f32d694`, ELF SHA-256
`87054f804b85a8b2f1fc0a1fb03b3759841b34587644932feaab5ca35b5e074e`.
It has no capture deadline or deliberate watchdog expiry and remains unflashed.

## V5 phone acceptance

Two identical records at DFX offsets `0x260a00` and `0x2e0a00` pass acceptance.
They show service readiness, the framebuffer console, and all eight CPUs
continuing to tick through the `RUNTIME observed seconds=60` deadline. Runtime
tick counts range from 5,656 to 5,677. The inherited SP805 identifies as
`0x141805`; control remains `3`, load remains `0x1e0000`, raw interrupt status
is zero at observation, and 67 feeds have completed. No kernel/userspace fault
or panic occurs before the deliberate retention marker.

Feeds remain at 67 afterward. The later expected FIQ has firmware watchdog
reason 76 and live raw interrupt status one, approximately 132.445 seconds
after entry. This verifies both the keepalive interval and the deliberate
expiry path. The stale ESR/PC are still incidental to the FIQ. The fault's
summary `last=35` remains lower than the explicit CPU1 `BOOT 36 services.ready`
record; acceptance uses the recorded readiness and runtime events rather than
that summary field.

The complete stock partition SHA-256 again matches
`ad00caaa84c53d3ce478e562a6a1116ab720b2f0fce565c518928476b55cc268`.
The final controller state records stock restoration and collection complete.

Pinned vendor source and hashes are under `out/anne-usb-research/`:
[HiSilicon fatal FIQ handler](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/mntn/hisi_fiq.c),
[source-word reader](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/ddrc/hisi_ddr_secprotect.c),
[reason definitions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/ddrc/hisi_ddr_secprotect.h),
[Hi6250 memory map](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/ap/platform/hi6250/global_ddr_map.h).

[Huawei SP805 driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/watchdog/hisi_sp805_wdt.c).
