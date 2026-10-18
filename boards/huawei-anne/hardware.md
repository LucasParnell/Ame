# Huawei anne bring-up status

Native eight-core Ressha startup is now hardware-verified: all eight pinned
workers completed on their assigned cores, every core handled timer ticks and
IPIs, and init reached service readiness without faults. See [framebuffer reference](FRAMEBUFFER.md) for the
latest LED test, artifacts and device state.

Native framebuffer first light is now physically confirmed: `ANKO EL1` on a
green screen. D0 (channel 6) scans physical `0x5d000000`, ARGB8888, 1080 × 2280,
stride 4320. Four bounded DFX records confirm painting, eight completed pinned
workers, service readiness and LPM3 reset without faults or panics. Stock is
restored and checksum-verified in TWRP. See [framebuffer capture](FRAMEBUFFER.md).

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
and unflashed; indefinite uptime remains unverified. See [watchdog evidence](FIQ.md).
Latest device state is recorded in the linked
framebuffer handoff. Local validation passes 17 focused host and 15 QEMU checks.

The [userspace USB plan](../../docs/USB_PLAN.md) records the captured Hi6250 OTG
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
[native boot diagnostics](BOOT_DIAGNOSTICS.md) for the integrated logger,
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

## Experimental native build

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

## Minimal framebuffer entry

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

## Local EL1 service bring-up

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

## Evaluation on the actual phone

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

## Captured native hardware evidence

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

## Collect the recovery evidence

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

## Evidence required for a native board

- Exact model, firmware, recovery version, and CPU architecture.
- Actual partition/image formats and shared-kernel behavior.
- Running DTB, usable memory banks, firmware reservations, and loader placement.
- Interrupt controller, timer routing, CPU enable method, and a safe console.
- A demonstrated kexec or temporary-loader handoff and return to recovery.
- A usable UART or verified display allocation for the standalone boot probe.

The collector gathers recovery observations; bootloader state, partition backups,
framebuffer physical bounds, and handoff/reset behavior require additional
verification. No boot addresses are generated from speculative defaults.

See [the Dango plan](../../docs/HUAWEI_P20_LITE_DANGO_PLAN.md) for guest bring-up milestones.
