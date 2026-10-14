# EL2 access for the first Dango hello guest

Updated 14 October 2026. This work targets the unlocked ANE-LX1 running
8.0.0.151(C782). The initial guest is a single-vCPU Linux appliance whose agent
prints hello through the emulated UART. Native execution still needs a firmware
handoff that gives Ame control of EL2.

## Recorded hardware evidence

- The verified native captures enter Anko at **EL1**.
- Saved recovery Linux output says `CPU: All CPU(s) started at EL1`.
- The saved recovery configuration enables `CONFIG_VIRTUALIZATION`, disables
  KVM, and disables `CONFIG_HISI_HHEE`.
- Saved bootloader output reports that its HHEE loader operations were absent
  and HHEE was not loaded. This does not demonstrate an accessible firmware
  hypervisor or an interface for installing one.

These are earlier private captures, not new device observations. Full captures
remain under `out/anne-device-private/`; no identifiers or firmware dumps are
added here. See [native hardware history](hardware.md) and
[boot diagnostics](BOOT_DIAGNOSTICS.md) for the existing capture/restore route.

## Implemented diagnostics and monitor gating

[Entry assembly](../../kernel/arch/arm64/entry.S) records the actual firmware
entry level for each logical host CPU before dropping to EL1. It publishes a
separate monitor-ready flag only after installing the Dango EL2 vectors and
initializing the monitor registers. An EL1 entry leaves that flag clear.

The fixed records live in `.data` so the primary's pre-BSS observation survives.
Each CPU owns a separate 64-byte record. The assembly uses release publication;
kernel readers use acquire loads. The vCPU path checks the current CPU's record
before executing HVC rather than inferring access from the boot CPU's level.
The flag means the monitor was configured; successful guest execution separately
proves it was callable.

The Huawei primary previously preserved its early diagnostic EL2 vectors after
the drop to EL1, whereas secondary CPUs installed Dango's vectors. A genuine
EL2 entry now installs Dango's monitor consistently on the primary too. Early
Huawei vectors still cover the earlier diagnostic setup and EL1 bring-up.

The retained entry trace includes `CPU_FEATURES` with MIDR, MPIDR, PFR0 and the
raw EL2 capability field. Later kernel logs report `EL2 hardware=...`,
`boot_entry=...` and `monitor=...`; secondary logs include `el2_entry` and
`el2_monitor`. Capability reads use EL1-accessible registers and do not invoke
unknown firmware SMC/HVC interfaces.

From a diagnostic-capable native shell, `diag el2` reports the raw hardware field,
the current EL1 host level, and `boot_entry_el` / `monitor_ready` for every logical
CPU. `diag` also includes all exported counters, service/resource state and the
retained boot log over the serial terminal; `diag log` replays just that log.
See [serial capture usage](../../docs/ANNE_USB.md#mac-serial-console-and-key-injection).
The read ABI copies at most 256 bytes per call, holds the existing log writer
lock only for that copy, and never probes an unknown HVC/SMC handler. Monitor
readiness is a configuration observation; a successful guest run remains the
execution proof.

## What remains unknown

The exact firmware stage selecting EL1 and a usable EL2 handoff interface have
not been established. Root access, an unlocked kernel partition and a CPU
virtualization feature do not provide that handoff. The new diagnostics do not
change the phone's firmware privilege policy.

Next inspect matching, hash-pinned bootloader/secure-monitor startup artifacts
to locate the transition into the kernel. Record the responsible component and
an evidenced entry mechanism before constructing a phone EL2 handoff probe.
No lower boot-stage modification is part of this implementation.

Separately, the native allocator remains capped at 128 MiB, which cannot back
the current 128 MiB Linux guest alongside Ame. Expand the verified usable host
window only after auditing reservations and mappings. QEMU hello testing uses
512 MiB host RAM and does not establish native phone memory acceptance.

## Local hello workflow

On macOS, build the pinned Linux source supplied by `DANGO_LINUX_SRC` using the
Linux container workflow (default source location: `~/Code/linux`):

```sh
DANGO_LINUX_OUT="$PWD/build/dango-linux-hello" scripts/linux/build-hello-linux-docker.sh
cmake --preset dango-hello-qemu-dev
cmake --build --preset dango-hello-qemu-dev --target anko mochi_system
python3 scripts/test/dango-hello.py --cpus 2
python3 scripts/test/dango-hello.py --cpus 1
python3 scripts/test/dango-hello.py --entry-el 1 --cpus 2
```

The signed `dango-hello` service profile selects nameservice, console, manager
and the native authority probe. The manager has VM authority only. The hello
guest has no device assignment or channel driver. Acceptance requires the
Linux agent's own output and a successful vCPU return; native service readiness
alone cannot pass it. The EL1 row instead requires explicit unavailability and
absence of guest execution.

That negative QEMU row disables virtual EL2 in the machine configuration. A
physical phone can advertise EL2 yet still enter Ame at EL1; the runtime gate
depends on an installed monitor, not solely on the CPU capability field.

Linux source and build outputs are separate, with the source mounted read-only
in the container. `build-info.json` records the source revision and resolved
configuration hash; the normal signing path authenticates the Image, initramfs,
DTB and manifest together.

For a future phone guest DTB, `DANGO_LINUX_TIMER_HZ=1920000` explicitly supplies
the measured counter rate. The default omits the override so QEMU uses its own
counter frequency. Using the phone rate in QEMU would produce incorrect timing.
This is a packaging option, not evidence that a phone guest has run.

## Validation for this implementation

| Check | Result |
| --- | --- |
| Signed hello guest through Dango, Cortex-A53 EL2, one host CPU | Pass: actual agent output and successful vCPU return. |
| Same guest with two host CPUs | Pass: CPU1 monitor installed and guest hello received. |
| EL1 with two host CPUs | Pass: explicit EL2 unavailability and no guest execution. |
| Synthetic Anne retained boot traces at EL1/EL2, one/eight CPUs | Four tests passed, including capability and monitor-state evidence. |
| Hello evidence, signed service policy and DT timer override | Four focused host tests passed. |
| Existing artifact, initramfs, descriptor, stage-2 and notification checks | 22 focused host tests passed. |
| `anne-native-dev` kernel build | Pass; no phone flash or new native guest acceptance. |

Per-run logs/results are under `out/dango-hello-acceptance/`. Its `artifacts.json`
pins the tested kernel, signed bundle and Linux payload hashes, plus Linux source
revision and resolved configuration hash. Linux hello artifacts are in
`build/dango-linux-hello/`; the QEMU build is `out/dango-hello-qemu-dev/`.

## Phone deployment, 14 October 2026

At the user's request, `anne-dango-entry-v1` was built with the native
`huawei-anne-usb` service profile and flashed to the connected, identity-checked
ANE-LX1. Model, exact firmware and unlocked bootloader were confirmed first.
The package's AVB metadata, full image checksums and stock restore checksum
were verified locally. Fastboot acknowledged sending/writing all 24 MiB to
`kernel`, then acknowledged reboot.

The phone returned as `Ame USB diagnostics` with a CDC serial port. Sending
`help` returned the native shell's builtin command list. The native image is
installed; stock has not been restored. This package contains entry/monitor
diagnostics and the existing USB/terminal services, with no Linux guest payload.
New physical EL2 capability/entry values still need an actual trace readback.

Package: `out/anne-dango-entry-package-v1/`. Native image SHA-256:
`5bc0893fe61827df3b22670b1440455be9d13677a39d0841d3922a72fa9a708b`.
The verified stock restore image is alongside it. Private preflight, flash,
reboot, USB and shell evidence are in
`out/anne-device-private/dango-entry-v1-test/`.

## Serial diagnostic command build, 14 October 2026

The follow-up `anne-serial-diag-v1` package adds `diag [all|el2|log]` to the same
native USB profile. The kernel/shell build passes, the shell formatter/log
replay passes ASan/UBSan checks (including an EL2-capable EL1 handoff and output
failure), and diagnostic reads pass the eight-core synthetic Anne workloads at
both EL1 and EL2. Seven native log/package tests pass. The QEMU-board kernel also
builds with retained-log support explicitly unavailable.

The prepared image is `out/anne-serial-diag-package-v1/ame-native-kernel.img`,
SHA-256 `84f9c18b7d71a7574c7a05164bfbdd55f26de04060188e49a117ff58a8e6a98e`.
Its AVB signature/hash and stock restore image were verified locally. This
follow-up package has **not** been flashed; the phone deployment above remains
the earlier image without the shell diagnostic command.
