# EL2 access for the first Dango hello guest

Updated 14 October 2026. This work targets the unlocked ANE-LX1 running
8.0.0.151(C782). The initial guest is a single-vCPU Linux appliance whose agent
prints hello through the emulated UART. Native execution still needs a firmware
handoff that gives Ame control of EL2.

See the later [EL2 path audit](EL2_PATH_AUDIT.md) for alternate boot-chain
routes and corrections to the RPMB canary evidence. Deployment paragraphs
below are historical snapshots; they do not describe the latest flashed image.

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

## Receiving an EL2 firmware handoff

The ANE entry path accepts an AArch64 non-secure EL2 handoff as well as the
existing EL1 handoff. No build switch selects the level: `CurrentEL` determines
the path on each CPU. EL2 hardware capability alone does not enable Dango.

On EL2 entry, assembly now normalizes HCR/CPTR/HSTR, clears the inherited
stage-2 table pointer, restores physical CPU IDs in VPIDR/VMPIDR, enables EL1
physical timer/counter access and clears CNTVOFF before early diagnostics.
Both primary and secondary paths use this setup. Each installs Dango's EL2
vectors and its own EL2 stack, initializes SP_EL1, then returns to the host at
EL1h with interrupts masked. Dango retains EL2 and enters guests through the
installed monitor; moving the host to EL1 does not relinquish EL2.

The loader must supply the existing Linux Image ABI: x0 points to the physical
DTB, MMU/data cache are off, and the CPUs are coherent. The primary retains the
existing rejection of MMU/data-cache-on handoffs. Secure firmware must permit
HVC (`SCR_EL3.HCE=1`) and supply EL2 to secondary CPUs too for Dango to run on
them. Ame cannot configure EL3 from EL2 or promote an EL1 handoff to EL2.
These requirements follow the [AArch64 boot protocol](https://www.kernel.org/doc/html/v6.12/arch/arm64/booting.html).

Check `diag el2` before attempting the ANE guest: each CPU intended to run a
vCPU must report `boot_entry_el=2` and `monitor_ready=1`. An EL1 handoff still
boots with its monitor flag clear, including when the hardware supports EL2.
Monitor readiness establishes configuration; a successful guest run is still
required to establish working Dango execution on the phone.

Local validation uses the production entry assembly on synthetic Anne QEMU
peripherals. The firmware fixture seeds EL2 traps, bogus virtual CPU IDs, a
nonzero timer offset and an empty SP_EL1 before an eight-core boot/workload;
a second case hands off at EL1 on an EL2-capable CPU and checks the fallback.
The native `anne-native-dev` kernel also builds. These checks require no phone
access and do not establish that ANE firmware supplies EL2.

## EL3 compatibility probe

The INFILTRATE 2019 presentation describes a Kirin 970/P20-specific RPMB
callback path. Its addresses cannot be copied to the ANE-LX1: the matching
Hi6250 EMUI-8 kernel source places the BL31 shared area at `0x34a21000`, the
RPMB request window at `0x34a29000` (size `0x8000`), and BL31 at `0x35e00000`.
The presentation's Kirin 970 values (`0x209e1000` and the `0x1fe00000` BL31
region) therefore do not describe this phone.

The native shell now has `diag el3`. It issues only the standard RPMB service
metadata calls (`CALL_COUNT`, `UID`, and `VERSION`) and prints their four return
words, together with the Hi6250 layout. The legacy callback ABI fields are zero:
Linux defines no callback in its RPMB request. It does not set the RPMB request
pointer, touch the shared window, call the request-completion service, alter page permissions, or
execute a payload. A successful metadata response establishes that the EL3
service is reachable; it does not establish an EL2 handoff.

The probe was physically run on the connected ANE-LX1 after the verified
identity preflight. The serial report completed with `el3.rpmb_call_count.x0 =
0xf` and service version words `0, 1, ...`, confirming the standard RPMB
metadata service is reachable. The same run reports `hardware_field=2`, but all
eight CPUs still enter Ame at EL1 with `monitor_ready=0`. Thus the metadata
service is present, while this probe does not provide an EL2 handoff.

The tested image is `out/anne-el3-probe-package-v1/ame-native-kernel.img`,
SHA-256
`3b2ec226ba9d71d5e8b9bb76143d75d4c1564052859862f0804586fa2f767102`.
The phone is currently left running this image for follow-up work; the verified
stock restore image remains in the same package.

The explicit `diag el3-canary` run then issued only `RPMB_SVC_REQUEST_ADDR`
with that validated request address. EL3 returned `0xff01`, which the matching
Hi6250 header names `RPMB_EXCEED_PART`; the shell prompt remained responsive and
the callback/completion SMC was never issued. This result is inconclusive: the
stock Hi6250 driver populates a `rpmb_config_info` structure in the request
window before registering the address, while this first canary intentionally
left the window untouched. It must not be read as proof that the address is
rejected until that precondition is reproduced with the correct cache policy.

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

## Native Dango profile and framebuffer handoff (21 October 2026)

The `huawei-anne-usb` profile now starts `dango.manager` and its restricted
capability probe. The manager receives VM authority without PCI/GPU assignment.
The native Image selects its EL1 or EL2 entry path from `CurrentEL`; the same
installed kernel accepts either firmware handoff. Dango queries the installed
monitor at runtime. With an EL1 handoff it reports EL2 unavailable and parks,
leaving the native console running without issuing HVC. An EL2 handoff installs
the monitor on each CPU and permits VM operations there.

This enables the manager and channel diagnostics. The native package does not
include a Linux guest payload, so it does not claim Linux guest execution.
The service registry now holds 28 entries, which still fits the original 4 KiB
page, accommodating the phone's existing services and both Dango services.

The 21 October physical EL2 test booted through a RAM-patched copy of the
handset's stock LK with a staged TEE image. All eight CPUs reported EL2 entry
and an installed monitor. The factory LK instead selected a fake panel and
left display hardware inactive; stock LK detected the BOE OTM1911A panel.
Stock LK's framebuffer begins at `0x5d964b00`. Its covering pages are now mapped
Normal NC, and `fb.offset=0xb00` travels through init to framebuffer consumers.
This fixes the previous stage-44 `Anne framebuffer attributes failed` panic.
A RAM boot with the mapping fix displayed the Ame terminal and enumerated the
Ame USB diagnostics device.

Installing the kernel does not persist the RAM LK patch. An ordinary stock-LK
handoff can remain EL1; the runtime detection handles that without promoting
EL1 or pretending the EL2 monitor exists. Firmware images and device captures
remain in the private workspace, outside this source repository.
