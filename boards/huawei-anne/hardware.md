# Huawei Anne hardware reference

Current system behavior and profile selection are summarized in [architecture](../../docs/architecture.md). These board notes preserve measured registers and bring-up observations. They are chronological evidence, not a statement of the currently installed phone image. Stock-LK framebuffer handoff and later direct scanout supersede the earliest fixed-framebuffer assumptions; see [boot](boot.md) and [graphics](../../docs/graphics.md).

- [Huawei anne bring-up status](#hardware)
- [Native framebuffer capture](#framebuffer)
- [Eight-core Ressha bring-up](#smp)
- [Native runtime FIQ investigation](#fiq)
- [Native power commands](#power)
- [Console status bar](#status_bar)

<a id="hardware"></a>
## Huawei anne bring-up status

<a id="hardware-huawei-anne-bring-up-status"></a>

Native eight-core Ressha startup is now hardware-verified: all eight pinned
workers completed on their assigned cores, every core handled timer ticks and
IPIs, and init reached service readiness without faults. See [framebuffer reference](hardware.md#framebuffer) for the
latest LED test, artifacts and device state.

Native framebuffer first light is now physically confirmed: `ANKO EL1` on a
green screen. D0 (channel 6) scans physical `0x5d000000`, ARGB8888, 1080 × 2280,
stride 4320. Four bounded DFX records confirm painting, eight completed pinned
workers, service readiness and LPM3 reset without faults or panics. Stock is
restored and checksum-verified in TWRP. See [framebuffer capture](hardware.md#framebuffer).

The console follow-up is physically confirmed: the inset `ANKO EL1` heading
and scrolling boot/service logs are visible below the notch. The normal native
profile enables the kernel framebuffer console, replays early output and renders
logs from `io.console`. The tested bounded package is
`out/anne-console-capture-package-v1/`; the normal console image was subsequently
flashed for a demonstration, reached readiness and then faulted on kernel FIQ
vector 6 at the DAIF restore in `kernel::LockGuard::~LockGuard()`. Stock is
restored and checksum-verified in TWRP. The later v3 source capture identified
an inherited watchdog timeout; v5 verifies SP805 keepalive through 60 seconds
of healthy runtime with eight CPUs ticking and 67 feeds. Its final watchdog
expiry was deliberately requested for trace retention. Normal v5 is packaged
and unflashed; indefinite uptime remains unverified. See [watchdog evidence](hardware.md#fiq).
Latest device state is recorded in the linked
framebuffer handoff. Local validation passes 17 focused host and 15 QEMU checks.

The [userspace USB plan](../../docs/usb.md#usb_plan) records the captured Hi6250 OTG
resources and proposes generic host/interface transfers, keyboard input and
device-role debugging. Native USB remains unimplemented and hardware-unverified.

Fastboot has confirmed an unlocked **ANE-LX1** running
**ANE-LX1 8.0.0.151(C782)**. The SHA-256-verified official TWRP
`3.7.0_9-0-anne` image has been flashed to `recovery_ramdisk`, with successful
send/write acknowledgements. TWRP now boots, exposes a root ADB shell, and has
successfully hosted a headless Ame evaluation under QEMU. An experimental native
Anko board target has now booted natively at EL1 through init and the selected
minimal services. Five identical retained `AMEANKO1` records show checkpoints
1–36, MMU enable, the first timer interrupt, signed MXB authentication,
`io.console: ready` and `init: all services ready`. No kernel fault, userspace
fault or panic was recorded. The capture intentionally reset at service
readiness; the full stock kernel checksum and working TWRP are verified again.
Longer-running stability and live native USB remain open.

The successful package is `out/anne-boot-trace-package-v2b/`; private records,
decoded logs and verification are in
`out/anne-device-private/boot-trace-v2-test/results/`. See
[native boot diagnostics](operations.md#boot_diagnostics) for the integrated logger,
fault registers, bounded capture and recovery readback procedure. The earlier
inconclusive tests below are historical observations.

The recovery image and installation result are under `out/anne-recovery/`.
The TWRP image is ramdisk-only (`kernel_size=0`). The native test temporarily
replaces the shared Huawei kernel and requires restoring it for Android/TWRP.
The bootloader does not support fastboot `fetch`, so the attempted
read-backup of the original recovery image was unavailable. After booting TWRP,
the current `kernel` (24 MiB), `ramdisk` (16 MiB), `recovery_ramdisk` (32 MiB), and
`dts` (28 MiB) partitions were read and backed up with SHA-256 checksums under
`out/anne-device-private/boot-backups/`. The recovery backup contains the newly
installed TWRP, not the previous stock recovery. No userdata partition was read
or erased during this work.

To enter the flashed recovery, disconnect USB and hold **Volume Up + Power** to
restart. Release Power at the Huawei logo and keep Volume Up held until recovery
opens, then reconnect USB for inspection. The disconnected recovery-entry combo
is documented by [Huawei support](https://consumer.huawei.com/ca/support/content/en-us00782420/).

The normal `huawei-anne` Mochi policy selects only `nameservice` and `io.console`.
It excludes Dango and the QEMU/USB/block driver chain. The native console writes
through the kernel debug syscall into retained RAM and the framebuffer console,
so this bring-up does not depend on a validated UART. Native startup, console
logging and physical framebuffer console output are verified. Live native USB
remains open.

<a id="hardware-experimental-native-build"></a>
### Experimental native build

The backed-up stock kernel confirms an Android v0 container, 2048-byte pages,
gzip ARM64 payload, `kernel_addr=0x00480000`, `tags_addr=0x07e00000`, and ARM64
`text_offset=0x80000`. Its legacy boot-image SHA-1 matches its payload. Linux's
captured iomem map also starts kernel code at `0x00480000`.

This gives a direct unlocked-bootloader route that does not require kexec. The
phone rejects fastboot's temporary `boot` command with `Command not allowed`,
so testing this route uses the backed-up `kernel` partition.

```sh
cmake --preset anne-native-dev
cmake --build --preset anne-native-dev --target anko provenance
python3 scripts/anne/build-native-package.py --output out/anne-native-package-new
```

The native target links Anko at the measured address, selects the captured
serial0 PL011 and GIC addresses, recognizes the Huawei GIC binding, limits the
allocator to 128 MiB of the first bank, applies existing DTB reservations, and
starts one CPU. It embeds the existing signed MXB payload in the native Image;
the same bundle authentication still runs before service loading. Android and
TWRP ramdisk partitions remain unchanged.

The package builder validates the native ELF/Image, current embedded bundle,
stock header/hash and restore backup. It preserves Huawei's stock header fields,
replaces the compressed kernel payload and legacy payload hash, and creates
locally verified AVB metadata with a development RSA-2048 key. It checks
partition fit and produces `ame-native-kernel.img`, `restore-stock-kernel.img`,
checksums, and provenance. These are experimental images, not a phone release.

The captured stock AVB footer ends at byte 25,161,728, one 4096-byte page before
the physical partition end. The stock metadata uses SHA256_RSA2048, rollback
index 0, flags 0, and a `kernel` hash descriptor. The first two test containers
omitted AVB metadata entirely; this is a concrete packaging mismatch and their
inconclusive results do not isolate runtime failures. The corrected builder
uses a SHA-256-pinned official avbtool revision, retains the observed effective
partition size and trailing page, and verifies both the new metadata signature
and the kernel payload hash before producing the full physical-partition image.
Its development key is not Huawei's key; handset acceptance still depends on
the already unlocked bootloader. V6 and the full Anko capture demonstrate
execution of this packaging route on this device. No vbmeta
partition is modified. Reference:
[Android AVB documentation](https://android.googlesource.com/platform/external/avb/+/refs/heads/main/README.md).

For a fresh local build, create a development signing key once (keep it outside
version control), then allow the builder to fetch the pinned AVB tool:

```sh
mkdir -p out/anne-native-build
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
  -out out/anne-native-build/dev-avb-key.pem
chmod 600 out/anne-native-build/dev-avb-key.pem
python3 scripts/anne/build-native-package.py --download-avbtool \
  --output out/anne-native-package-new
```

Reuse an existing key instead of running key generation again. A different local
key can be selected with `--avb-key`. The private key is not included in packages.

The earlier native logger validated the runtime ramoops DTB
layout before selecting only its console ring at `0x34860000`, size `0x80000`.
Stock recovery reports ramoops base `0x34800000`, total size `0x100000`, and
console size `0x80000`. The native logger uses Linux 4.4's persistent-ring header;
its region was mapped uncached, and userland console output went through the
existing debug-output syscall into this ring. It wrote `AME NATIVE ANE` before
the normal kernel boot log. The integrated logger now starts before BSS and
constructors, replicates `AMEANKO1` into all five ramoops zones, and preserves
fault registers and boot checkpoints. RAM retention through LPM3 reset, stock
restoration and TWRP startup is verified. References:
[Linux ramoops placement](https://github.com/torvalds/linux/blob/v4.4/fs/pstore/ram.c),
[persistent ring format](https://github.com/torvalds/linux/blob/v4.4/fs/pstore/ram_core.c).

No native framebuffer address is assumed and no display-register programming
has been added. A Huawei logo remaining on screen is inconclusive. Native
acceptance requires the persistent log (or a separate validated output path),
not merely a flash acknowledgement or a QEMU result.

The first full Anko image was flashed and rebooted, then the stock kernel was
restored. TWRP booted again and the entire kernel partition's SHA-256 matched
the backup exactly. Its mounted pstore filesystem contained no files, so this
test is inconclusive: native execution and RAM-log retention are unverified.
Evidence is in `out/anne-device-private/native-test/`.

An independent `anne-entry-probe` target now isolates the CPU entry/display path
from allocator, UART, interrupts, BSS, DTB parsing, and static constructors. It
requires EL1/EL2 with MMU/data-cache off, reads the firmware's active DSS D3
channel and LDI0 geometry, and validates a linear RGB565/ARGB8888/XRGB8888 buffer
inside the captured boot-logo reservation. It rejects IOVAs, compression,
tiling, cropping, transforms, unsupported formats, unexpected geometry, and
out-of-range allocations; padded stride is supported. The probe does not
change display registers. Valid scanout is painted green at EL1 or magenta at
EL2. The captured panel identifies itself as BOE_OTM1911A, video TFT, 1080 × 2280.
The bounded decoder has host tests covering unsafe configurations. Definitions
come from the vendor's
[Hi6250 register header](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/lineage-17.1/drivers/hisi/ap/platform/hi6250/soc_dss_interface.h)
and [RDMA setup code](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/lineage-17.1/drivers/video/fbdev/hisi/dss/hisi_overlay_utils.c).

```sh
cmake --build --preset anne-native-dev --target anne-entry-probe
python3 scripts/anne/build-native-package.py --probe \
  --output out/anne-entry-probe-package-new
```

The entry probe was flashed and rebooted. The user reported no visible output
and a hang/pause after reboot, then returned the phone to fastboot. The stock
kernel restore completed, TWRP booted, and the complete kernel partition checksum
matched the original backup again. Evidence and restore
state are in `out/anne-device-private/native-entry-probe-test/`. This result does
not establish whether the probe executed and rejected the display state, failed
earlier, or was not entered. A positive probe would establish CPU/display entry
only, not full Anko/Mochi readiness. Further native work needs reliable early
observability or additional bootloader/display handoff evidence before another
hardware boot attempt. The AVB packaging correction was validated locally and
the corrected entry probe was flashed/rebooted. The user observed it staying
on the unlocked-bootloader `device is booting now...` splash, with no colored
execution marker. The stock kernel was restored again, TWRP booted, and the entire
kernel partition's SHA-256 matched the original backup. This remains inconclusive and does
not distinguish bootloader rejection from an entry-state or display-validation
failure. Evidence is under `out/anne-device-private/native-entry-probe-avb-test/`.
Each test temporarily replaces the shared kernel: write the stock restore image
back to `kernel` before booting Android/TWRP. The per-command USB reset workaround
also applies to these fastboot operations.

<a id="hardware-minimal-framebuffer-entry"></a>
### Minimal framebuffer entry

The current target is first-light v6, built from the archived v5 sources whose
hashes were verified before modification. It records red, blue and green LED
transitions using the measured 1,920,000 Hz architectural counter rate;
the handset's CNTFRQ register incorrectly reports 16,384 Hz. White marks a fault
and green blinking marks a returned reset pulse. The v5 LPM3 NMI pulse remains
the reset mechanism. Logs now flush their complete written range and identify
themselves as `AMEFL6`, with per-hold timestamps and PMIC control readbacks.
These records can be recovered through pstore and Huawei DFX. The original v6
capture used three-second LED holds; the current minimal and full-kernel boot
paths record transitions without those delays. Six entry/render/timing/fault
tests and seven native package/layout tests passed before the v6 flash. Physical
LED visibility was observed on the handset: red, purple, purple, then reboot.
Five 2642-byte `AMEFL6` pstore records survived restoration and TWRP startup.
They show all three holds lasting approximately 3.0001 seconds, no recorded
fault, and one NMI reset note. The control/output readbacks were red `1/0x3c`,
blue stage `4/0x0f`, and green stage `2/0x33`; physical colour selection still
needs investigation. The DFX partition did not change from the v5 capture;
the successful v6 readback came directly from pstore. The stock kernel checksum
was verified again. For this handset/session, the user reports `reboot recovery`
selects eRecovery; after `fastboot reboot bootloader`, installed TWRP appeared
and was confirmed through ADB. Allow startup to complete before checking its
connection. Package metadata is in
`out/anne-firstlight-package-v6/manifest.json`; the verified stock restore image
is packaged alongside it. The description below records the original minimal
target before the first-light instrumentation was added.

`anko-minimal` is the first-output target. It has its own Image header, entry
assembly, 16 KiB boot stack, 16 KiB exception stack and EL1/EL2 vectors. It masks
interrupts, selects SP_ELx and clears its BSS. It keeps the entry exception level
and requires the Image ABI's MMU-off state; it does not bring up the allocator,
GIC, timer, scheduler, services or USB. There are no constructors, unresolved
symbols or runtime relocations. The full kernel entry now also masks interrupts,
selects SP_ELx and installs EL1 vectors before constructors and native logging.

The renderer reads the inherited Hi6250 D3, D2 and V1 RDMA channels and LDI0. It
uses DMA source extents (16-byte X units), rather than assuming every source fills
the panel. It permits linear physical RGB565, ARGB/XRGB4444, ARGB/XRGB5551 and
ARGB/XRGB8888, with source cropping and row padding, only inside the captured
48 MiB logo reservation. It rejects compression, IOVAs, tile/interleave modes,
transforms, stretching and enabled masking. It writes no display registers.
Successful output is a white `ANKO EL1` label over green, or `ANKO EL2` over
magenta; a handled exception after framebuffer discovery paints red. Framebuffer
writes are cleaned to PoC for inherited scanout. Optional DTB/pstore logging runs
after the screen attempt and is not required for it.

```sh
cmake --preset anne-native-dev
cmake --build --preset anne-native-dev --target anko-minimal
python3 scripts/anne/build-native-package.py --minimal \
  --output out/anne-minimal-package-new
```

The minimal package does not require a Mochi bundle. It validates its ELF entry,
load segments against the raw Image, complete memory footprint, the stock backup,
gzip container and AVB metadata. It includes the verified stock restore image.
Flashing still temporarily replaces the shared kernel. ADB can reboot TWRP into
fastboot; after a minimal boot, returning to fastboot may require the buttons.

Local verification passed six tests: source bounds/formats and bounded rendering,
Cortex-A53 EL1/EL2 entry and framebuffer pixels, alternate-channel selection, and
EL1/EL2 exception handling. The executable tests use synthetic display registers
in QEMU RAM and verify output memory through QMP; they do not establish native
Huawei acceptance or display access. Full Anko/Mochi EL1 and EL2 regressions and
the host suite passed after the shared entry changes. Hardware execution remains
unverified until an observed marker or retained log establishes it.

<a id="hardware-local-el1-service-bring-up"></a>
### Local EL1 service bring-up

The `anne-qemu-dev` preset uses QEMU hardware with the `huawei-anne` service
selection. Build and run its one-core Cortex-A53/EL1 smoke test:

```sh
cmake --preset anne-qemu-dev
cmake --build --preset anne-qemu-dev --target anko mochi_system
python3 scripts/test/boot.py --build out/anne-qemu-dev --cpus 1 \
  --cpu cortex-a53 --entry-el 1 --marker 'boot_el=1' \
  --marker 'init: all services ready'
```

This checks the signed minimal service policy without EL2. The resulting kernel
is still a `qemu-virt` image and must not be flashed onto the phone. Existing board
profiles remain the default unless `MOCHI_SYSTEM_PROFILE` is explicitly set.

Local validation has passed: the signed bundle contains only `nameservice` and
`io.console`, the Cortex-A53 run reports `boot_el=1` and reaches
`init: all services ready`, and the default EL2 QEMU boot still reaches readiness.
Boot evidence is saved under `out/anne-qemu-dev/boot-el1/` and
`out/anne-qemu-dev/baseline-el2/`; build provenance is under
`out/anne-qemu-dev/generated/`. These results establish local service bring-up,
not physical Huawei boot support.

<a id="hardware-evaluation-on-the-actual-phone"></a>
### Evaluation on the actual phone

The same minimal Ame build has now booted on the ANE-LX1 under QEMU 10.0.0 inside
TWRP. Its log reports `boot_el=1`, successful signed-bundle authentication,
`io.console: ready`, and `init: all services ready`. The reusable runner was
verified from a fresh staging directory: setup and boot completed in about six
seconds, the emulator was stopped, and TWRP remained accessible. Evidence is under
`out/anne-device-private/recovery-evaluation-v2/`.

This uses software emulation of `qemu-virt`, one Cortex-A53 CPU, and 256 MiB guest
RAM. It has no screen UI or phone input integration. It does not demonstrate
native Anko access to Huawei peripherals.

To reproduce after building the guest above:

```sh
cmake --build --preset anne-qemu-dev --target provenance
python3 scripts/anne/build-recovery-evaluation.py --download \
  --output out/anne-recovery-evaluation-new
python3 scripts/anne/run-recovery-evaluation.py \
  --package out/anne-recovery-evaluation-new \
  --output out/anne-device-private/evaluation-new
```

Use new output directories for each run. The builder pins official Alpine
aarch64 artifacts with SHA-256 hashes and records versions, licenses, package
source commits, and download URLs in the package. On the phone, apk verifies
package signatures using Alpine's bundled keys. Setup disables repository
access and package scripts; its explicit non-repository option is needed because
the evaluation lives in temporary RAM. No untrusted-signature option is used.

The runner first verifies the local package and checks for an `anne` TWRP root
shell. It stages into a unique `/tmp/ame-evaluation-*` directory, checks transfer
hashes, prepares the private runtime, boots the guest, saves logs, and stops only
the matching emulator process. Nothing is mounted or flashed. Staged files remain
in RAM for inspection and disappear when recovery reboots. SELinux stayed
Enforcing throughout both successful evaluations.

<a id="hardware-captured-native-hardware-evidence"></a>
### Captured native hardware evidence

Recovery debugging is available now. A temporary read-only debugfs mount was
tested and removed successfully; it exposes tracing/event interfaces, clock
summaries, regulators, GPIO, pinctrl, regmap, USB and `lcd-dbg`. Root ADB can also
collect the running Linux log, which includes Huawei bootloader messages. The
latest capability capture is `out/anne-device-private/native-entry-probe-avb-test/`
`debugfs-inspection.json`. KGDB and function tracing are not enabled in the
captured configuration. The pstore filesystem is mounted but contained no
previous-boot files after these tests.

These interfaces inspect the running recovery Linux kernel, not a simultaneously
running native Anko kernel. Anko would need its own output/USB/debug transport
after handoff. Booting Anko directly from recovery requires a compatible kernel
with ARM64 kexec implementation and configuration, plus a verified loader/DTB
contract; installing a kexec executable cannot supply the missing kernel support.
Upstream Linux 4.4's ARM64 Kconfig has no KEXEC option, so a stock-era kernel may
need a backport as well as configuration changes. References:
[Linux 4.4 ARM64 configuration](https://github.com/torvalds/linux/blob/v4.4/arch/arm64/Kconfig),
[kernel kexec configuration](https://www.kernel.org/doc/html/latest/admin-guide/kdump/kdump.html).

Private observations are under `out/anne-device-private/recovery-inspection/`;
sanitized facts and artifact hashes are recorded in `device-profile.json`.

| Observation | Measured value |
| --- | --- |
| Recovery | TWRP 3.7.0_9-0, root ADB, Linux 4.4.23+ aarch64 |
| Linux memory | 3,830,368 KiB total; about 3.4 GiB available before staging |
| Interrupt controller in DTB | `arm,cortex-a9-gic`, distributor `0xe82b1000`, CPU interface `0xe82b2000` |
| Generic timer in DTB | Nonsecure physical PPI 14 / IRQ 30; declared clock 1,920,000 Hz |
| CPU firmware interface | PSCI using SMC |
| Framebuffer | `hisifb0`, virtual size 1080 × 4560, stride 4320 bytes, 32 bpp |
| Input | `huawei,ts_kit` touch, `hisi_gpio_key` volume keys, `hisi_on` power |

The captured DTB declares three RAM banks and Linux's iomem report contains many
reserved holes. These must be reconciled before setting native allocator bounds.
The framebuffer's doubled virtual height is not the physical panel height;
its physical buffer address and ownership are still unverified. DTB register
addresses are observations, not a successful native access test or load address.

The kernel configuration contains no kexec options, kexec interfaces/tools are
absent, and the only observed kexec syscall symbols are weak placeholders.
Together these indicate that this recovery lacks a usable kexec handoff; no
recovery-kexec transition was attempted. The experimental direct bootloader
route above uses the backed-up kernel partition instead. Upstream Linux
registers unavailable kexec calls as weak fallbacks in
[kernel/sys_ni.c](https://github.com/torvalds/linux/blob/v4.4/kernel/sys_ni.c).

Use the Huawei OEM build query for the installed firmware version. Recovery's
Android build properties describe its own build, and its `ro.boot.flash.locked`
property conflicts with the OEM unlock query and acknowledged flash; it is not
used to replace the verified bootloader state.

<a id="hardware-collect-the-recovery-evidence"></a>
### Collect the recovery evidence

If the phone is in fastboot, first collect its Huawei information queries:

```sh
python3 scripts/anne/inspect-fastboot.py
```

This uses read-only model/build/bootloader-status commands. It does not establish
which recovery is installed. A remote `FAILED` response is treated as failure
even if the host tool exits with status zero, and collection stops at a timeout.
Some environments require USB access outside their filesystem sandbox.

The phone enumerates as `Fastboot2.0` (`18d1:d00d`). Initial information queries
timed out with official platform tools 37.0.1 and 35.0.2. A host-side libusb reset,
restricted to the matching phone serial/device IDs, restored communication. On
this Mac, each new fastboot invocation needed a fresh USB reset. Huawei OEM
model/build/bootloader queries then confirmed the device and unlock state; the
download limit is 471859200 bytes. The recovery-partition size variable is
unsupported and must not be interpreted as a real zero-byte partition. Private
observations and the temporary reset helper are under `out/anne-device-private/`.

For future fastboot work, use a fresh USB connection/reset between invocations;
another port/cable or a USB 2.0 hub may also avoid the stall. The anne guide
documents USB troubleshooting; its older flashing instructions are not a
substitute for this phone's verified split partition layout.
Source: [LineageOS anne installation guide](https://lineageos.github.io/lineage_wiki/devices/anne/install/).

Once a working TWRP recovery is confirmed, boot it and connect through USB. From
the repository root:

```sh
python3 scripts/anne/inspect-recovery.py collect
```

The collector finds ADB on `PATH` or in `out/host-tools/platform-tools/adb`. Pass
`--adb /path/to/adb` if necessary. With more than one attached transport, choose
one using `--serial`. It checks for an ANE model or `anne` codename and a TWRP
version before collecting hardware observations.

The default output is a new private temporary directory on the host. A permanent
local location can be chosen with `--output /path/to/new-directory`; existing
directories are refused. Keep captures outside the source tree. Collection uses
read-only shell commands and `exec-out cat`; it does not mount volumes, change
root state, reboot, load a kernel, flash, or read raw partition contents.

Outputs include `report.json`, `hardware.md`, text observations, and the running
DTB and kernel configuration if recovery exposes them. Missing, malformed, or
timed-out observations are recorded. Text captures redact common identifiers;
other identifiers can remain, and binary artifacts are private. Review before
sharing. The report never treats kernel kexec configuration as a tested handoff.

Saved captures can be checked without a connected device:

```sh
python3 scripts/anne/inspect-recovery.py summarize /path/to/inspection/report.json
```

The analyser checks capture paths, sizes, and SHA-256 hashes before deriving its
summary. It distinguishes unavailable configuration from explicitly disabled
kexec and always leaves native boot pending until a physical handoff is tested.

<a id="hardware-evidence-required-for-a-native-board"></a>
### Evidence required for a native board

- Exact model, firmware, recovery version, and CPU architecture.
- Actual partition/image formats and shared-kernel behavior.
- Running DTB, usable memory banks, firmware reservations, and loader placement.
- Interrupt controller, timer routing, CPU enable method, and a safe console.
- A demonstrated kexec or temporary-loader handoff and return to recovery.
- A usable UART or verified display allocation for the standalone boot probe.

The collector gathers recovery observations; bootloader state, partition backups,
framebuffer physical bounds, and handoff/reset behavior require additional
verification. No boot addresses are generated from speculative defaults.

See [the Dango plan](../../docs/linux-appliances.md#huawei_p20_lite_dango_plan) for guest bring-up milestones.

<a id="framebuffer"></a>
## Native framebuffer capture

<a id="framebuffer-native-framebuffer-capture"></a>

Current device state: TWRP is connected and the full stock kernel checksum
is verified after v5. Two identical DFX records prove 60 seconds of healthy
runtime with eight CPUs ticking and 67 watchdog feeds. The later watchdog FIQ
was deliberately requested after observation for firmware trace retention.
The inherited watchdog timeout is addressed by keepalive, with 42 local checks
passing. Normal v5 is packaged and unflashed; indefinite uptime is unverified.
Controller and acceptance: `out/anne-device-private/watchdog-v5-test/`.
See [FIQ evidence and watchdog implementation](hardware.md#fiq).

Updated 12 October 2026. The first native full-kernel framebuffer capture is
physically confirmed: the user saw a green screen with `ANKO EL1` at the top.
The label was slightly clipped. Four bounded records recovered from Huawei's
DFX backup confirm painting, eight-core worker completion, service readiness
and intentional LPM3 reset, with no faults or panics. After that capture, TWRP
and the complete stock shared-kernel SHA-256 were verified.

Console follow-up: the user confirmed the inset heading and boot/service logs
are visible and work on the phone. Clipping has been addressed and the verified native
scanout is now integrated with early boot output and the existing `io.console`
service. The normal Huawei target enables `ANKO_ANNE_FRAMEBUFFER` by default.
The heading begins at (64,144), with 3× 8×16 glyphs; the scrolling log viewport
begins at (64,224), uses 2× glyphs, and contains 59 columns × 61 rows. The
viewport preserves side margins, the heading and at least 96 bottom pixels.

The kernel owns this boot console. It replays retained early messages after
MMU setup, then renders boot stages, kernel output and service logs forwarded
by `io.console` through the native debug syscall. Raw constructor/register
metadata stays in the retained trace. Kernel ring messages have one visible
copy when `io.console` drains them. Writes are serialized by the existing
native trace guard and dirty pixel rows are cleaned for display DMA. The normal
image keeps the console after readiness; the bounded capture gives five extra
seconds to inspect it before resetting.

The console v1 capture is physically verified with retained-log readback. Its image is
`out/anne-console-capture-package-v1/`, SHA-256
`220e2afc5f62f4d3148191490ecc932affa80c9ad2ab1d04d353a4f0bc634c49`;
the normal image is `out/anne-native-console-package-v1/`, SHA-256
`ec2314a3a43783c6a7a79aca6054df581e33811ab4896126d0eed215fef5a7f0`.
Each includes its own matching ELF and stock restore image. The normal image
has no diagnostic deadline or readiness reset. It was flashed for the requested
demonstration after capture readback: visible console and readiness were
confirmed, followed by an FIQ fault. It is an archived failing image, not a
stable release.
The completed capture controller is
`out/anne-device-private/console-v1-test/control.py`.

Huawei DFX retained four tagged records from two executions. Two identical
35,667-byte copies reach `BOOT 36 services.ready`, contain `io.console: ready`
and `init: all services ready`, and confirm eight completed workers with positive
timer, preemption, context-switch and IPI counts on every core. The renderer
replayed 30,323 early trace bytes and processed 4,269 visible characters by
readiness, then intentionally reset after the viewing hold. The other two
34,978-byte copies initialize the console and finish all eight workers, but
hit the ten-second capture deadline before service readiness. Neither execution
records a fault or panic; repeated-startup reliability remains unverified.
Private raw traces, decoded records, stock verification and the evidence summary
are under `out/anne-device-private/console-v1-test/results/`.

The next planned native subsystem is [userspace USB](../../docs/usb.md#usb_plan),
covering generic host peripherals, keyboard input and device-role debugging.

Local verification passes 17 focused host checks and 15 full instrumented QEMU
checks. The QEMU tests decode the actual framebuffer glyphs to verify the inset
`ANKO EL1/EL2` heading, `io.console: ready` and `init: all services ready`,
including eight-core startup, scrolling, IOVA rejection and unchanged margins.
The saved review surface is `out/anne-console-validation/el1-console.png`.

```sh
cmake --preset anne-console-capture-dev
cmake --build --preset anne-console-capture-dev --target anko provenance
python3 scripts/anne/build-native-package.py \
  --build out/anne-console-capture-dev --output out/anne-console-capture-package-new
```

To collect a future run after restoring stock and returning to TWRP:

```sh
python3 scripts/anne/collect-native-log.py \
  --package out/anne-console-capture-package-v1 \
  --output out/anne-device-private/console-v1-test/results --dfx-fallback
```

The new `RUN tag=anne-console-v1` identifies this capture. DFX fallback extracts
bounded persistent_ram records only when their run tag matches the package,
so older first-light traces cannot be decoded as this image. `--offline
--dfx-fallback` re-decodes the saved backup without touching the phone.

The read-only AArch64 Linux probe in `scripts/anne/fb-ioctl-probe.c` obtained
`FBIOGET_FSCREENINFO` and `FBIOGET_VSCREENINFO` in installed TWRP. The shared
stock kernel partition SHA-256 matched the backup before collection. It found:

| Property | Recovery observation |
| --- | --- |
| Panel | BOE_OTM1911A 5P84 VIDEO TFT, 1080 × 2280 |
| Virtual geometry | 1080 × 4560 |
| Active page offset | x=0, y=2280 |
| Pixel channels | R16:8, G8:8, B0:8, A24:8 (ARGB8888) |
| Stride | 4320 bytes |
| Allocated framebuffer bytes | 29,552,640 |
| `fix.smem_start` | **IOVA `0x40000`, not a physical framebuffer address** |
| Recovery H porches/pulse | back=80, front=130, sync=6 |
| Recovery V porches/pulse | back=26, front=16, sync=10 |
| Raw `var.pixclock` | 180,000,000; retain raw value rather than assuming picoseconds |

The matching EMUI 8 [`hisifb_alloc_fb_buffer` implementation](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/lineage-20/drivers/video/fbdev/hisi/dss/hisi_fb.c)
allocates through the ION system heap and assigns `iommu_format.iova_start` to
`fix.smem_start`. The recovery buffer cannot be treated as one contiguous
physical allocation, or as the native boot-logo buffer. The actual DTB's BOE
panel node specifies a 180 MHz pixel clock, four DSI lanes, 550 MHz DSI bit
clock and uncompressed video mode. Recovery has no exposed `/dev/mem`,
`/dev/kmem`, clock summary or display-register debug interface usable by the
inspection. Raw captures and downloaded source hashes are retained privately
under `out/anne-device-private/framebuffer-v1-inspection/` and
`out/anne-framebuffer-research/`.

The native capture establishes the actual inherited scanout, separate from
recovery's IOMMU allocation:

| Native property | Verified register evidence |
| --- | --- |
| Read channel | D0, logical channel 6, RDMA offset `0x50000` |
| Physical buffer | `0x5d000000` |
| Pixel format | ARGB8888, `DMA_CTRL=0x28` |
| Width/height | 1080 × 2280 |
| Stride | 4320 bytes, `DMA_STRIDE0=0x10e` (270 × 16) |
| Channel control | `CH_CTL=0xf001`, enabled physical linear path |
| Overlay path | D0 → enabled overlay0 layer0 → MCTL0/DBUF0 → LDI0/DSI0 |

The configuration is unchanged between entry and MMU completion. D2, D3, V1,
G1 and D1 are disabled. The earlier three-channel probes never examined D0;
their rejection was correct for the channels they inspected. Full path
validation found D0 without relaxing the memory or format checks.

`ANKO_ANNE_DISPLAY_CAPTURE` enables `display_diagnostics.cc` in the full native
Anko target. It records named register offsets and values at entry before MMU
enable, then again at `MmuDone` after installing Device mappings for DSS and
CRG. It preserves and maps the complete 48 MiB logo reservation as RAM. All
implemented Hi6250 read channels (D2, D3, V1, G1, D0 and D1), MCTL0/overlay0,
DBUF0, LDI0, DSI0, SMMU and clock/reset state are captured. Each candidate
records the first failing scanout check with a named reason.

Painting requires enabled nonsecure physical linear RGB565/ARGB8888/XRGB8888,
the full portrait rectangle, a bounded address in the reserved logo pool,
enabled DSI video with matching geometry, MCTL routing through overlay0 and
DBUF0, and an enabled full-screen layer selected from that channel. Cropping,
padding, masks, stretching, rotation, compression and IOVA addressing are
rejected. A successful candidate receives the existing white `ANKO EL1/EL2`
label on a black band and a green/magenta field; writes are cleaned to memory
for DMA and held for five seconds before boot continues. An absent scanout
produces `DISPLAY paint=skip no_verified_scanout`. This capture does not yet
program a new display-controller path.

Build and package into a fresh directory:

```sh
cmake --preset anne-framebuffer-capture-dev
cmake --build --preset anne-framebuffer-capture-dev --target anko provenance
python3 scripts/anne/build-native-package.py \
  --build out/anne-framebuffer-capture-dev \
  --output out/anne-framebuffer-capture-package-new
```

The preset preserves the eight-core worker profile and bounded readiness reset.
The package includes the exact ELF and verified stock restore image. Image
SHA-256: `2e830014802b48b5cb744b062e3ddb818b29446a805b09c1d0945c999bdb0c85`.
The tested controller is
`out/anne-device-private/framebuffer-v1-test/control.py`; its state was created
fresh for this image and now records the completed run and verified restore.
Create fresh state for a new image. The earlier local
`v1` package has incorrect display metadata; use `v1a`.

Focused validation: 12 full instrumented QEMU checks pass, including eight-core
EL1 capture with painted memory, EL2 painting, IOVA rejection with unchanged
buffer memory, readiness ordering, and faults. Host tests validate the ioctl
ABI, address/routing rejection, native packaging and framebuffer rendering.
QEMU reserves its synthetic logo RAM below its DTB, so painting preserves
topology discovery and all eight workers. There are 13 focused host checks and
12 instrumented QEMU checks. The physical observation is separate evidence
from these tests. The native DFX records also show positive switch, preemption,
tick and handled-IPI counts on all eight cores.

Collect only after restoring the stock shared kernel and returning to TWRP:

```sh
python3 scripts/anne/collect-native-log.py \
  --package out/anne-framebuffer-capture-package-v1a \
  --output out/anne-device-private/framebuffer-v1-test/results
```

The decoder reports display phases, rejected channels and paint completion
alongside existing boot/SMP/fault evidence. This recovery boot exposed no pstore
files, but its 16 MiB `dfx` partition retained four records from the framebuffer
image: two copies of each of two complete executions. Each bounded trace is
35,402 bytes. The remaining AMEANKO1 records belong to an earlier image and
were excluded. Use explicit header offsets and the exact packaged ELF:

```sh
python3 scripts/anne/extract-dfx-native-log.py \
  --input out/anne-device-private/framebuffer-v1-test/results/dfx-private.bin \
  --package out/anne-framebuffer-capture-package-v1a \
  --output out/anne-device-private/framebuffer-dfx-decoded-new \
  --header-offset 132096 --header-offset 656384 \
  --header-offset 919040 --header-offset 1443328
```

The saved decode and summary are under
`out/anne-device-private/framebuffer-v1-test/results/dfx-decoded/`. Extraction
uses validated persistent_ram header lengths; bytes beyond those lengths
contain stale recovery logs or binary data and must not be included. Select
offsets belonging to the tested run rather than interpreting every common tag
with the current ELF.

The first-light run confirmed visible output and automatic reboot from native
EL1. The console follow-up above implements the inset layout and normal
boot/service log integration; its physical verification is tracked separately.

<a id="smp"></a>
## Eight-core Ressha bring-up

<a id="smp-eight-core-ressha-bring-up"></a>

The captured ANE-LX1 DTB lists eight CPUs at affinities `0,1,2,3,100,101,102,103`
in two clusters; the second cluster is listed out of numeric order. Recovery
also reports CPUs `0-7` online and possible. Native bring-up uses the DTB's SMC
conduit and `CPU_ON` function `0xc4000003`.

The native target now supports eight host CPUs. The shared kernel/userland limit
also sizes boot log rings, per-core stacks and EL2 state. Other boards retain
their four-core limit. The topology parser honours disabled nodes, one- or
two-cell affinities, the PSCI conduit and boot-core identity. Logical CPU zero
is the executing boot CPU; remaining identities are sorted by full affinity.

Before starting CPUs, the boot core cleans the kernel image, bootstrap data and
identity table to memory. Secondary assembly identifies the physical core from
that table, selects its own stack, masks interrupts and invalidates stale
instruction-cache entries. It enables the shared kernel mappings, installs
vectors, initializes its GIC interface and timer, then publishes online status.
Secondaries wait at a release gate while the primary finishes startup.

Each online core owns a Ressha/Eki station. An atomic online mask prevents
scheduling to failed CPU slots; count and mask are passed to init and services.
GIC wake SGIs use the interface's banked target mask rather than assuming a
logical ID equals a hardware target bit. Queue publication precedes SGI delivery.
Global userspace timestamps still use the boot CPU's 100 Hz tick count; per-core
ticks and scheduler/IPI statistics are retained separately.

Native tracing serializes records across cores only after normal-memory
attributes are installed. Each core has its own fault stack and recursion guard.
Kernel messages and debug-output syscalls write whole messages under the trace
guard, so concurrent CPU startup cannot interleave checkpoint lines.

The green LED request occurs at `BOOT 36 services.ready`, after the existing init
readiness message. Colour changes clear the previous channel enables and restore normal output
control before configuring the next channel. The stock recovery driver selected
only green, and the user confirmed the physical LED was green. The first native
eight-core capture reached readiness but appeared purple; its unselected output
control pairs had been forced to 3. The corrected sequence clears those pairs.
The final v3 native capture physically showed red, blue, green, reboot;
its test and recovery evidence are tracked in the framebuffer handoff.

The register sequence follows the matching public EMUI 8 Huawei
[LED driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/lineage-20/drivers/leds/hisi_leds.c)
and [LED definitions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/lineage-20/include/linux/hisi/hisi_leds.h),
using register locations and currents from the actual captured DTB. The vendor
[PSCI CPU operations](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/lineage-17.1/arch/arm64/kernel/psci.c)
also use the full physical affinity to start each CPU.

<a id="smp-validation-workload"></a>
### Validation workload

The isolated `huawei-anne-smp` profile adds `test.anne_smp` to nameservice and
console. It creates eight suspended helper threads, pins one to every core with
a Ressha scheduling context, and releases them together. An arrival barrier
checks shared-memory visibility across cores; each worker verifies its actual
CPU and computes for 200 ms before notifying completion. The main thread blocks
on a doorbell, then reports readiness only after all eight have completed.
Workers use the latency band because init polls readiness there: fair-band
workers on CPU0 would be starved while init is runnable in a higher band.

The trace records `SMP_WORK cpu=N observed=N rounds=...`, followed by
`SMP_WORK complete mask=ff workers=8`. At readiness it records every core's
dispatch/switch count, timer accounting, tick count and handled IPI count. The
initial idle-to-thread dispatch now counts as a context switch, so a core that
runs its first worker no longer appears unused.

```sh
cmake --preset anne-smp-native-dev
cmake --build --preset anne-smp-native-dev --target anko provenance
python3 scripts/anne/build-native-package.py --build out/anne-smp-native-dev \
  --output out/anne-smp-capture-package
```

This preset deliberately resets at readiness, faults or the ten-second runtime
capture deadline. The normal `anne-native-dev` profile keeps the two production
services and leaves the green LED selected after readiness, with capture reset
disabled. Package the exact build and use its saved ELF for fault decoding.

Local tests cover eight-core EL1 and EL2 startup, worker execution, per-core
timers and real SGI handling, green-stage ordering, faults before constructors
and after MMU enable, capture reset, and retention of all five record copies.
Parser tests cover both clusters and shuffled DT order, booting from the second
cluster, disabled CPUs and invalid conduits. Four-core lifecycle and supervision
matrices also pass with the shared changes.

<a id="smp-verified-phone-capture"></a>
### Verified phone capture

`out/anne-smp-capture-package-v2` ran natively at EL1. All seven secondary
`CPU_ON` calls returned zero; all eight CPUs came online (`mask=ff`). Each
pinned worker ran on its assigned core and completed, with positive switch,
preemption, timer-tick and handled-IPI counts on every core. Init reached service
readiness, and five records survived the intentional LPM3 reset. No kernel fault,
userspace fault or panic was recorded. Stock restoration and TWRP startup were
checksum-verified. This proves bounded eight-core startup and the test workload;
it does not establish long-term stability or load balancing under all workloads.

The exact package image SHA-256 is
`22c047bdb2f5a495513ac61685c0581950ad373bdcde5e928e4462f8434ab332`.
Decoded records are under
`out/anne-device-private/smp-capture-v1-test/results/`. The physical LED sequence
was red, purple, purple, reboot; the revised output-control sequence was built
after this capture. See [framebuffer reference](hardware.md#framebuffer)
for the latest device state and follow-up artifacts.

<a id="fiq"></a>
## Native runtime FIQ investigation

<a id="fiq-native-runtime-fiq-investigation"></a>

Updated 12 October 2026. The earlier normal console images faulted on FIQ
vector 6 because the inherited watchdog expired. V5 verifies 60 seconds of
healthy runtime with keepalive, followed by deliberate expiry for retention.
Stock is restored and its full checksum verified in TWRP. Latest evidence is
`out/anne-device-private/watchdog-v5-test/results/acceptance.json` and its
matching raw/decoded records. The keepalive passes 21 focused host checks and
21 instrumented QEMU checks. Normal v5 is packaged but remains unflashed;
indefinite uptime has not been verified.

<a id="fiq-recovered-evidence"></a>
### Recovered evidence

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

<a id="fiq-targeted-source-capture"></a>
### Targeted source capture

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

<a id="fiq-inherited-watchdog-keepalive"></a>
### Inherited watchdog keepalive

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

<a id="fiq-v5-phone-acceptance"></a>
### V5 phone acceptance

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

<a id="power"></a>
## Native power commands

<a id="power-native-power-commands"></a>

Updated 14 October 2026. `shutdown`, `reboot`, and `reboot fastboot` (also
`reboot bootloader`) go through init's service-control channel and its existing
`system.control` capability. A read-only kernel check rejects unsupported
operations before any service stops. Accepted requests receive a reply and a
100 ms terminal grace period before planned service teardown. A returned error
restores interrupt state and lets supervision restore the previously desired
services. A failed reset is not reported as a completed userland reboot.

The earlier path assumed that a working PSCI CPU_ON implied usable system power
calls and then waited forever if those calls returned. The captured firmware
advertises legacy PSCI, including explicit system-power function IDs. That is
not proof those functions perform the native board sequence. Generic boards now
use power IDs declared by their DT or implied by PSCI 0.2/1.0 compatibility;
an invalid conduit or unknown power function is rejected.

<a id="power-verified-mechanism-and-register-bounds"></a>
### Verified mechanism and register bounds

The captured phone DT has `hisi,powerhold` at **0xfff0e000**, offset **4** and no
powerhold-protect property. GPIO25's PL061 direction register is at 0x400 and its
masked bit-4 data register at 0x40. The kernel maps this one Device page in each
process address space. It preserves other direction/data bits and does not
invent a protection-register write.

Normal reboot writes **0x10** and fastboot writes **0x01** to PMIC HRST_REG0,
register index **0x18b**, at byte address **0xfff3462c**. The PMUSSI bus uses
byte-wide accesses with a four-byte stride; `libk::Mmio::write8/read8` verifies
the reason before reset. Reset pulses bit 2 at **0xfff0a510**, preserving other
bits and ordering the two edges. This is the LPM3 path already used by native
boot diagnostics. The normal command makes one pulse and returns an error if
firmware has not reset after three seconds.

Shutdown writes reason **0x00** and lowers power-hold. If external power keeps
the PMIC alive for a second, the vendor sequence requests charger boot with
reason **0x06** and resets. Thus USB-powered shutdown may enter charging boot
rather than remain off; Ame has no separate Android charging userspace.

The primary vendor references are the
[power driver](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/mntn/hisi_poweroff.c),
[reboot reason and NMI implementation](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/mntn/blackbox/platform_ap/rdr_hisi_ap_adapter.c),
[Hi6250 reboot reason values](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/ap/platform/hi6250/mntn_public_interface.h)
and PMIC byte-access implementation inspected during earlier board bring-up.
Pinned downloads and SHA-256 values are retained in
`out/anne-power-research/sources.json`; the captured phone DT is private under
`out/anne-device-private/recovery-inspection/observations/`.

<a id="power-validation"></a>
### Validation

ASan/UBSan probes check PMIC byte lanes, GPIO masks, NMI bit preservation,
register-view bounds, shell syntax, request routing, and returned errors. Real
QEMU power calls check capability ownership, unsupported fastboot handling,
shutdown, and reset. Eight-core synthetic Anne workloads at EL1 and EL2 retain
working diagnostics. The native USB build and existing USB/package checks pass.

Phone deployment and command observations are recorded after the physical test;
host/QEMU tests alone do not establish successful native shutdown.

<a id="status_bar"></a>
## Console status bar

<a id="status_bar-console-status-bar"></a>

`tty.fb0` owns the bar; the kernel boot renderer and graphics clients do not draw
it. The phone profile reserves a conservative 448-pixel centre cutout across a
128-pixel top strip, with 64-pixel outer margins and 2x console glyphs. This is a
configurable safe area, not a measured notch outline. The existing terminal
starts at y=224; scrolling copies only the terminal viewport. Other console
profiles get a compact 64-pixel strip and automatic terminal geometry below it.
Set `tty.status=0` to disable it. Geometry bindings are `tty.status.height`,
`tty.status.scale`, `tty.status.margin`, and `tty.status.notch` (all pixels except
scale). A strip that cannot fit both panels is disabled.

CPU is the interval's non-idle scheduler time divided by elapsed time and the
number of online CPUs: 100% means all online cores busy. Its cumulative counter
survives process/principal exit. The first sample and CPU-set changes show `--%`
until a valid interval exists. RAM is allocated plus reserved physical pages in
the allocator's managed RAM region, divided by the total managed pages. Used
pages are maintained under the existing allocator lock, so telemetry does not
scan the RAM bitmap with interrupts masked.

The battery field shows **voltage**, not state of charge. The captured phone DT
identifies `hisilicon,hi6555v100_coul` under the PMIC at 0xfff34000. The reader
checks VERSION4=1, reads V_OUT at registers 0x1f4/0x1f5 with a bounded high/low/high
retry, and applies the vendor ADC scale and OTP factory voltage trims from
0x182/0x183. All accesses are byte reads at four-byte PMUSSI strides within the
already mapped PMIC page. No charger settings, gauge initialization, saved SOC,
or interrupt registers are written. It relies on the inherited gauge continuing
to sample; live updates and absolute accuracy still need handset verification.
It does not load Android's additional nonvolatile calibration. Unsupported IDs,
invalid/out-of-range samples, and platforms without this gauge show `BAT --.--V`.
Voltages below 3.50 V use amber text. A percentage/charging indicator requires
state-of-charge and charger policy support, which this bar does not invent.

Primary register/conversion references, pinned to the same vendor revision as
other Anne board mechanisms:

- [Hi6555v100 driver](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/power/hisi/coul/hi6555v100/hisi_hi6555v100_coul.c): `hi6555v100_coul_convert_regval2uv`, `hi6555v100_coul_get_battery_voltage_uv`, `hi6555v100_coul_get_ate_a`, and `hi6555v100_coul_get_ate_b`.
- [Gauge definitions](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/power/hisi/coul/hi6555v100/hisi_hi6555v100_coul.h).
- [Hi6250 PMIC register map](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/ap/platform/hi6250/pmic_interface.h).

Telemetry samples once per second through `MX_DIAG_HEALTH`. Unchanged displayed
values produce no drawing commands. Publication shares the terminal's single
producer, retains partially admitted frames under backpressure, and participates
in the normal retry/deadline loop. No UART/USB output includes the bar.

Host validation is in `tty_status_probe.cc`, run by `tests.test_framebuffer`:
notch/text bounds, unavailable telemetry, normalized multicore CPU usage, CPU-set
changes, redraw suppression, ring saturation, pixel-copy scrolling, and PMIC
identification/byte lanes/voltage conversion/read-only behavior. Scheduler probes
also check the monotonic busy counter across concurrent accounting and principal
reuse. The optional probe output renders the actual font/rasterizer to a PPM
preview. Hardware battery sampling and the physical safe area remain unverified.
