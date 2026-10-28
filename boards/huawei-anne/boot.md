# Huawei Anne boot and firmware investigations

The working EL2 route uses RAM-patched stock LK and a staged TEE. All-core EL2 entry and monitor installation have recorded evidence; ordinary reboot discards the RAM patch. Start with the handoff reference below, especially its final 21 October checkpoint. The remaining sections preserve earlier investigations and negative results; their historical open tasks and device state are not current implementation claims. [Operations](operations.md) contains the installation workflow.

- [EL2 access for the first Dango hello guest](#el2)
- [ANE-LX1 EL2 path audit](#el2_path_audit)
- [Fastboot EL2 patch feasibility on ANE-LX1](#fastboot_el2)
- [EL2 handoff chain — state of the work](#el2_chain_state)
- [Fastboot RD mode — 18 October 2026](#rd_mode)
- [Fastboot OEMINFO consumers — fresh handset analysis, 2026-10-17](#oeminfo_consumers)
- [Fastboot eMMC inputs before EL1 — 18 October 2026](#pre_el1_loads)
- [Exact-device fastboot authentication evidence](#xloader_validation)
- [Offline fastboot analysis](#unicorn)

<a id="el2"></a>
## EL2 access for the first Dango hello guest

<a id="el2-el2-access-for-the-first-dango-hello-guest"></a>

Updated 14 October 2026. This work targets the unlocked ANE-LX1 running
8.0.0.151(C782). The initial guest is a single-vCPU Linux appliance whose agent
prints hello through the emulated UART. Native execution still needs a firmware
handoff that gives Ame control of EL2.

See the later [EL2 path audit](boot.md#el2_path_audit) for alternate boot-chain
routes and corrections to the RPMB canary evidence. Deployment paragraphs
below are historical snapshots; they do not describe the latest flashed image.

<a id="el2-recorded-hardware-evidence"></a>
### Recorded hardware evidence

- The verified native captures enter Anko at **EL1**.
- Saved recovery Linux output says `CPU: All CPU(s) started at EL1`.
- The saved recovery configuration enables `CONFIG_VIRTUALIZATION`, disables
  KVM, and disables `CONFIG_HISI_HHEE`.
- Saved bootloader output reports that its HHEE loader operations were absent
  and HHEE was not loaded. This does not demonstrate an accessible firmware
  hypervisor or an interface for installing one.

These are earlier private captures, not new device observations. Full captures
remain under `out/anne-device-private/`; no identifiers or firmware dumps are
added here. See [native hardware history](hardware.md#hardware) and
[boot diagnostics](operations.md#boot_diagnostics) for the existing capture/restore route.

<a id="el2-implemented-diagnostics-and-monitor-gating"></a>
### Implemented diagnostics and monitor gating

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
See [serial capture usage](../../docs/usb.md#anne_usb-mac-serial-console-and-key-injection).
The read ABI copies at most 256 bytes per call, holds the existing log writer
lock only for that copy, and never probes an unknown HVC/SMC handler. Monitor
readiness is a configuration observation; a successful guest run remains the
execution proof.

<a id="el2-what-remains-unknown"></a>
### What remains unknown

The exact firmware stage selecting EL1 and a usable EL2 handoff interface have
not been established. Root access, an unlocked kernel partition and a CPU
virtualization feature do not provide that handoff. The new diagnostics do not
change the phone's firmware privilege policy.

Next inspect matching, hash-pinned bootloader/secure-monitor startup artifacts
to locate the transition into the kernel. Record the responsible component and
an evidenced entry mechanism before constructing a phone EL2 handoff probe.
No lower boot-stage modification is part of this implementation.

<a id="el2-receiving-an-el2-firmware-handoff"></a>
### Receiving an EL2 firmware handoff

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

<a id="el2-el3-compatibility-probe"></a>
### EL3 compatibility probe

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

<a id="el2-local-hello-workflow"></a>
### Local hello workflow

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

<a id="el2-validation-for-this-implementation"></a>
### Validation for this implementation

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

<a id="el2-phone-deployment-14-october-2026"></a>
### Phone deployment, 14 October 2026

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

<a id="el2-serial-diagnostic-command-build-14-october-2026"></a>
### Serial diagnostic command build, 14 October 2026

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

<a id="el2-native-dango-profile-and-framebuffer-handoff-21-october-2026"></a>
### Native Dango profile and framebuffer handoff (21 October 2026)

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

<a id="el2_path_audit"></a>
## ANE-LX1 EL2 path audit

<a id="el2_path_audit-ane-lx1-el2-path-audit"></a>

14 October 2026; ANE-LX1 8.0.0.151(C782), Kirin 659/Hi6250.
This audit uses saved phone captures, the dumped fastboot image, matching Linux
source, and the primary references below. No phone command, reboot, or flash
was performed during this audit. It is not proof that every firmware interface
or vulnerability has been exhausted.

<a id="el2_path_audit-finding"></a>
### Finding

There is no demonstrated way to install Dango's monitor from the phone's
current EL1 kernel entry. There **are other unresolved paths** besides the RPMB
callback research: the board-software USB download loader, modifications to
the boot chain, and the bootloader's HHEE/BL31 handoff machinery. None has yet
produced a measured EL2 entry on this handset.

The CPU supports EL2. The saved native diagnostic reports EL2 feature field 2,
EL1 entry on all eight CPUs, and no installed Dango monitor. Recovery Linux
independently reports that all CPUs started at EL1. An empty EL2 or an absent
HHEE image does not let EL1 install a replacement hypervisor.

<a id="el2_path_audit-paths-and-limits"></a>
### Paths and limits

| Path | Evidence | Finding / missing proof |
| --- | --- | --- |
| Kernel config, boot argument, DT property, root | All eight recorded firmware entries are EL1. Linux's boot protocol requires the loader to supply EL2 for virtualization. | These do not themselves change the executing privilege level. No documented ANE option that changes the loader's choice was found. A property interpreted by secure firmware would be a different, unverified mechanism. |
| EL1 `ERET`, writing EL2 registers, plain `HVC` | Dango installs its vectors only after a measured EL2 entry. | EL1 cannot return upward or install EL2 vectors. HVC invokes a higher-level handler when enabled; it does not run caller-supplied code at EL2. An existing usable handler would have to be established separately. |
| PSCI CPU_ON, hotplug, suspend/resume | The current CPU_ON route already starts all seven secondary cores at EL1. The DT uses SMC and legacy PSCI function IDs. TF-A's reference implementation derives the entry mode from secure state, not from a caller-supplied EL parameter. | No standard PSCI parameter selects EL2. A vendor resume defect remains a firmware-research hypothesis, not an available alternate startup mode. TF-A reference code is not proof of this encrypted BL31's exact implementation. |
| HHEE / HKIP | Saved boot log says `__load_hhee can not get load_hhee_ops, hhee will not load`; recovery config disables CONFIG_HISI_HHEE. The dumped fastboot has distinct HHEE and BL31 boot branches. | No loaded vendor hypervisor or installation API has been established. HHEE is a security hypervisor, not automatically a general-purpose VM host. Loader wiring and verification requirements need tracing before this can become a route. |
| Bootloader or BL31 handoff modification | The dumped fastboot executes EL3 register accesses in its early initialization and later dispatches to BL31. It also contains BL31 secure verification/decryption paths. | An earlier privileged stage can establish an EL2 handoff in principle. An unlocked kernel partition does not prove that modified fastboot/BL31 images will authenticate. Must resolve signing, encryption, rollback, entry parameters, and a recoverable deployment method. No such image was flashed. |
| USB download / board software | PotatoNV explicitly lists P20 Lite / Nova 3e (ANE), Kirin 65x (A), and describes loading a board-software USB loader into RAM through DOWNLOAD_VCOM. Its loader repository contains hisi65x_a. | A concrete device-specific lead distinct from the RPMB exploit. Requires access to download mode, ordinarily a physical testpoint. PotatoNV's success at unlocking is not proof of arbitrary EL3 execution or an EL2 handoff. Audit the loader's execution level, accepted payloads, validation, and transfer capabilities first. |
| RPMB or other EL3 service vulnerability | RPMB metadata SMCs return successfully; earlier request registration returned 0xff01. BL31 partition dump is encrypted. | Still unvalidated. The tested canary contained a config-count bug (below), since corrected in source; the talk's callback offset was for Kirin970. No completed callback/payload or EL2 transition was observed. Other SiP/OEM services have not been exhaustively enumerated and audited. |
| TWRP, kexec, U-Boot or UEFI chainloaded from EL1 | TWRP's kernel was observed entering at EL1. | Another EL1 program inherits the same limitation unless it invokes a separately demonstrated privileged firmware mechanism. Rejecting `fastboot boot` is a transport-policy result, not an EL2-capability test. |
| Hardware debug / fault injection | No unlocked debug authentication or applicable board procedure has been established. | Not ruled out in principle, but no concrete usable route found. |
| QEMU TCG / software translation | Earlier QEMU guest execution succeeds with an emulated CPU that provides EL2. | Can supply an emulated EL2 and guest execution; it does not acquire the physical phone's EL2. A software guest backend inside Ame would be separate implementation work. |

<a id="el2_path_audit-exact-fastboot-evidence"></a>
### Exact fastboot evidence

Private dump: `out/anne-device-private/el2-firmware-inspection/fastboot.img`.
SHA-256: `16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
Offsets below are image-relative disassembly addresses, **not runtime addresses**.

- At 0xbc and 0xc4, early code reads/writes ACTLR_EL3; later setup also accesses
  SCR_EL3. This is direct evidence that fastboot contains code requiring EL3.
- At 0x90708, the HHEE loader obtains the `hhee` operations by registry lookup.
  Its null branch at 0x90764 prints the absent-operations message seen on the
  phone. Strings alone do not establish a functional loader on this build.
- At 0x910d0, boot dispatch looks up `trustfirmware` and `hhee` operations. The
  default branch logs `boot_from_bl31` and calls the trustfirmware operation at
  table offset +8 (indirect branch at 0x91158). An enabled HHEE path instead
  calls +16 at 0x911b0. Resolving these operation tables and their handoff data
  remains necessary; this audit does not claim the final EL selector is found.
- At 0x91470, the direct kernel fallback calls a byte-zeroing helper at 0x320
  and branches to its supplied address. Branching does not change EL.
  No evidence shows that this fallback is selectable for unsigned kernel
  execution on the current authenticated BL31 boot path.

<a id="el2_path_audit-correction-to-the-existing-canary-evidence"></a>
### Correction to the existing canary evidence

The earlier tested `kernel/syscall/dispatch.cc` initialized `total_blks` to 8192 and
commented that block-size value 8 means log2(512). That is incorrect: log2(512)
is 9. The matching Linux `emmc_get_rpmb_info()` uses:

```c
total_blks = (raw_rpmb_size_mult * (128 * 1024)) >> RPMB_BLK_SIZE;
/* RPMB_BLK_SIZE is 8 in emmc-rpmb.h */
```

For multiplier 0x20, this is 16384, not 8192. The 512-byte transport frame
must not replace the driver's calculation. Region fields are still assumed
rather than captured from this handset's EXT_CSD. Therefore v5/v6 are **not
validated reproductions of Linux initialization**, and 0xff01 cannot close the
EL3 research path. This audit records the defect without altering/flashing code.

15 October source update: the current calculation in `rpmb.h` now produces
16384 and passes differential checks against the pinned Huawei driver. This
does not retroactively validate v5/v6. See [RPMB_AUDIT.md](storage.md#rpmb_audit) for
source provenance, exact request offsets, and the remaining EXT_CSD assumptions.

The earlier truncated serial capture also does not demonstrate a secure-world
fault: a subsequent full capture returned 0xff01 and shell status 1 normally.
USB enumeration delays alone do not establish a panic or failed boot.

<a id="el2_path_audit-next-investigation"></a>
### Next investigation

The user has excluded a testpoint-dependent system and bootloader replacement,
and requested that the RPMB canary be left alone. Investigation therefore
continues through the existing stock firmware, with no new phone probes in
this audit. A working route must demonstrate both primary and secondary EL2 entry,
monitor installation, and actual guest execution; finding a loader or a
privileged branch is insufficient.

<a id="el2_path_audit-follow-up-resolved-normal-handoff"></a>
### Follow-up: resolved normal handoff

Further static analysis of the same hash-verified fastboot image resolves the
operation registry and the normal-world entry-state producer. No device was
accessed, and the canary implementation was not changed.

The lookup routine at image offset `0x110f44` walks the pointer table at
`0x292190..0x292518` (113 descriptors). It compares each descriptor's name,
checks its initialized status at descriptor offset `+0xc`, and returns its
operations pointer at `+0x18`. The bounds are loaded through pointer cells at
`0x291a30` and `0x291868` respectively. These observations derive from the
actual lookup code, rather than interpreting every binary string as a module.

The `trustfirmware` descriptor is at `0x290fd0`; its operations table at
`0x290ff0` contains:

| Operations offset | Function image offset | Observed purpose |
| --- | --- | --- |
| +0 | 0x10bb04 | Load BL31, selecting its encrypted-header path when the image magic matches. |
| +8 | 0x10b5b8 | Construct the normal BL31 parameters and branch into BL31. |
| +16 | 0x10b724 | Alternative parameter construction requiring HHEE operations. |

There is **no `hhee` descriptor in these 113 entries**. This explains the
observed missing-operations message and means that merely supplying an HHEE
partition or setting a hypothetical enable flag has no evidenced loader here.
This does not exclude a different firmware build having such a module.

The normal handoff routine at `0x10b5b8` constructs a TF-A-shaped parameter
block at physical `0x35e80000`, with its non-secure entry record at
`0x35e80030`. The field layout matches TF-A's AArch64 entry-point information:
entry PC at `+8`, SPSR at `+16`, and arguments starting at `+24`.

At image offsets `0x10b634` and `0x10b640`, it loads the immediate `0x3c5` and
writes it to the entry record's SPSR field (`0x35e80040`). Its low mode bits
are `0b0101`: AArch64 EL1h. The high `0x3c0` bits mask D/A/I/F. The value is
an instruction immediate, independent of the kernel header, command line, DT,
or NVRAM in this routine. The PC and first argument are separately copied
from the boot descriptor at offsets `+0xc` and `+0x20`.

The normal and secure entry pointers are installed into the BL31 parameter
block. The routine then passes the parameter block in x0 and branches to
BL31 at `0x35e00000` through the `br x2` stub at image offset `0x348`.
This identifies the bootloader's producer of the EL1 request; the encrypted
BL31 implementation has not been examined to prove exactly how it consumes
or overrides every field. The recorded EL1 entries are consistent with it.

Consequently, the normal handoff supplies a hardcoded EL1 state, and the
apparent HHEE alternative lacks a registered implementation. No stock setting
that requests an EL2 handoff has been identified. Obtaining EL2 while retaining
this boot chain would require an additional firmware mechanism or defect,
not a change to Dango's normal EL1 startup code.

Published HISEE secure-monitor advisories are another research category, but
the saved recovery config disables both CONFIG_HISI_HISEE and
CONFIG_HISI_HISEE_MNTN. That config does not prove the corresponding BL31 SMC
handlers are absent; equally, advisories for another firmware do not establish
their presence or exploitability here. No HISEE command was sent.

Supporting layout and mode definitions:
[TF-A entry-point structure](https://github.com/ARM-software/arm-trusted-firmware/blob/master/include/export/common/ep_info_exp.h),
[TF-A AArch64 mode definitions](https://github.com/ARM-software/arm-trusted-firmware/blob/master/include/arch/aarch64/arch.h).
The unverified HISEE lead is documented in the
[researchers' advisory](https://blog.impalabs.com/2212_advisory_huawei-secure-monitor.html).

<a id="el2_path_audit-primary-references"></a>
### Primary references

- [Linux AArch64 boot requirements](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html): firmware entry level, SCR_EL3.HCE and CPU consistency.
- [TF-A PSCI reference implementation](https://github.com/ARM-software/arm-trusted-firmware/blob/master/lib/psci/psci_common.c): psci_get_ns_ep_info selects EL1/EL2 using SCR_EL3.HCE, not CPU_ON's context ID.
- [PotatoNV author documentation](https://github.com/kitsuned/PotatoNV): ANE support, testpoint/download mode, board-software loader, and distinction between USERLOCK and FBLOCK.
- [PotatoNV loader repository](https://github.com/kitsuned/HiSiBootloaders): hisi65x_a and hisi65x_b artifacts.
- [Impalabs' HHEE research](https://blog.impalabs.com/2212_huawei-security-hypervisor.html): vendor hypervisor purpose and boot flow; its analyzed binaries are not this ANE firmware.
- Matching Hi6250 Linux revision used locally: `65e545cbaa11d132e243287b150146bc2a51c45e` in `hisi-oss/android_kernel_huawei_hi6250-8`. Reviewed local files: `/tmp/hi6250-src/emmc-rpmb.c`, `emmc-rpmb.h`, `hisi_rpmb.c`, `bootdevice.h`, `share.dtsi`, and `global_ddr_map.h`.

<a id="fastboot_el2"></a>
## Fastboot EL2 patch feasibility on ANE-LX1

<a id="fastboot_el2-fastboot-el2-patch-feasibility-on-ane-lx1"></a>

14 October 2026. Target: the saved ANE-LX1 8.0.0.151(C782), Kirin 659/Hi6250.
This follows the [EL2 path audit](boot.md#el2_path_audit), independently checks its
handoff instruction, and resolves the stock fastboot command-permission gate.
The subsequent [exact-device xloader validation](boot.md#xloader_validation) captures
xloader/VRL and proves that this patch fails the captured content hash, while
updating that hash fails the existing certificate signature. The unresolved
authentication statements below describe the earlier stage of this audit.
The analysis uses the existing private firmware dump and recorded phone logs.
Live `fastboot devices` enumeration found no transport; no phone command,
reboot, flash, partition write, or lock-state change was performed.

<a id="fastboot_el2-finding"></a>
### Finding

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

<a id="fastboot_el2-exact-candidate"></a>
### Exact candidate

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

<a id="fastboot_el2-why-the-current-fastboot-cannot-flash-itself"></a>
### Why the current fastboot cannot flash itself

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

<a id="fastboot_el2-what-could-make-this-route-viable"></a>
### What could make this route viable

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
limit documented in [EL2.md](boot.md#el2) also needs resolving for the current
128 MiB guest. No VM success is claimed from the offline candidate.

<a id="el2_chain_state"></a>
## EL2 handoff chain — state of the work

<a id="el2_chain_state-el2-handoff-chain-state-of-the-work"></a>

17 October 2026. Target: ANE-LX1, 8.0.0.151(C782), Kirin 659 / Hi6250.
Image under analysis: `out/anne-device-private/el2-firmware-inspection/fastboot.img`,
4,194,304 bytes, SHA-256 `16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
796,272 instructions decoded.

**Two address spaces are in play.** Everything below is written as *image-relative*
(`+0x10b634`) with the runtime address in brackets, because the image is loaded at
`0x10000000` — see §2.5. Quoting one without the other is how the `0x110b634`
slip below happened.

No device command was issued and nothing was flashed.

<a id="el2_chain_state-where-it-stands"></a>
### Where it stands

| link | state |
| --- | --- |
| BMP overflow writes `w·h·bpp` bytes into the heap, unbounded | **measured** (earlier work) |
| write-what-where gadget `+0x8d290` | **measured** |
| EL2h request site `+0x10b634` and its patch value | **measured** (`+0x10b634` = runtime `0x1010b634`) |
| corrupted object field redirects a real dispatch at `+0x3989c` | **measured** |
| entry + dispatch + write run as one chain at the real load base | **measured in Unicorn** |
| load base `0x10000000` | **unsupported** — the manifest field is a flash offset, not a RAM address (§7.3) |
| `usb_init` precedes the logo, `usbloader` follows it | **measured** (log line order) |
| logo records come from oeminfo | **established** (source of the images) |
| relocation of the image's absolute pointers at load | **none in the image**; an external relocator would be required (§7.2) |
| `.text` writable at the running EL | **unverified** |
| BL31 honours the supplied SPSR | **unverified** |
| trigger: two centring calls, small then large, one boot | **not demonstrated** |
| victim object inside the span | **not demonstrated** |

<a id="el2_chain_state-1-the-defect-as-measured"></a>
### 1. The defect, as measured

`hisi_display_logo` (`+0x4fac8`, display ops[0] at `+0x25f270`) takes an index in
`w21` and, for indices 0–2, resolves the `logo` module by name (`+0x15bf10`), calls
its prepare op, then calls `+0x4ef74` (also published as display ops[3] at
`+0x25f288`), which calls the centring routine `+0x4ebec` at `+0x4f020`. The range
check is the string `logo index is out supported` at `+0x15be88`.

```
4ec78: add  x28, x24, #0x600      ; x28 = 0x293600
4ec7c: mul  w26, w20, w21         ; w26 = width * height * bpp
4ec80: ldr  x0, [x28, #0x8]       ; cached buffer pointer (0x293608)
4ec88: cbz  x0, 0x4ef14           ; NULL -> allocate
4ec98: bl   0x119c4c              ; memset(buf, 0, w26)
4ecb0: bl   0x1176ac              ; memcpy(buf, src, w26)
4ef14: mov  w0, w26
4ef18: bl   0x111680              ; malloc(w26)
4ef1c: str  x0, [x28, #0x8]       ; the only store to 0x293608
```

The cache holds a pointer and nothing else — no capacity field, so a second call
can never learn the buffer is too small. The firmware names the buffer itself: the
malloc-failure path at `+0x4ef38`/`+0x4ef40` references `+0x15b588`,
`alloc logo_temp_buf failed,error`.

Allocator `+0x111680` is a monotonic bump with 32-byte alignment and no header, so
a 4-byte request yields 32 usable bytes. The overrun is exactly `w·h·bpp − 32`.
The entry clamp at `+0x4ec5c`/`+0x4ec70` compares `width+1`/`height+1` against the
panel and bails to a log-only path at `+0x4ee38`.

`+0x4ebec` has one caller (`+0x4f020`); `+0x4ef74` has one direct caller
(`+0x4fd18`) and is otherwise reachable only through its ops-table entry.

<a id="el2_chain_state-2-what-is-proven"></a>
### 2. What is proven

<a id="el2_chain_state-21-write-what-where-at-0x8d290"></a>
#### 2.1 Write-what-where at `+0x8d290`

```
8d290: ldr  w1, [x0, #0x18]   ; destination address (32-bit, zero-extended)
8d294: cbz  w1, 0x8d2a4
8d298: mov  w1, w1
8d29c: ldr  w0, [x0, #0x10]   ; value
8d2a0: str  w0, [x1]
8d2a4: ldp  x29, x30, [sp], #0x10
8d2a8: ret
```

Both operands come from the same buffer, the write is 32-bit (enough for any
address below 4 GiB), and it returns cleanly. Regex scans report this as absent;
the operand-based model finds it.

<a id="el2_chain_state-22-the-el2-request-and-its-patch-site"></a>
#### 2.2 The EL2 request and its patch site

From `EL2_PATH_AUDIT.md` and `FASTBOOT_EL2.md`, re-read at instruction level:

```
10b634: mov  w1, #0x3c5       ; EL1h, D/A/I/F masked
10b640: str  w1, [x0, #0x10]  ; x0 = 0x35e80030 -> writes 0x35e80040
```

The handoff routine `+0x10b5b8` builds a TF-A-shaped block at physical `0x35e80000`
with the non-secure entry record at `0x35e80030`: PC at `+8`, SPSR at `+16`
(`0x35e80040`), args at `+24`. It then branches to BL31 at `0x35e00000` through the
`br x2` stub at image offset `0x348`.

`0x528078a1` → `0x52807921` changes the immediate to `0x3c9`, i.e. EL2h. For an EL3
request the value is `0x3cd`, but BL31 is still resident at EL3 in that case, so it
buys coexistence, not removal of the monitor.

<a id="el2_chain_state-23-dispatch-through-a-corrupted-object-field"></a>
#### 2.3 Dispatch through a corrupted object field

```
3989c: ldr  x22, [x0]
398a0: cbz  x22, ...
398a4: ldr  x1, [x22]
398a8: mov  x0, x20
398ac: blr  x1
```

`+0x398a8` is the trap in this shape: the callee receives `x20`, not the object that
was corrupted. A victim is only usable if its field is the one forwarded as `x0`.

<a id="el2_chain_state-24-the-joined-chain-at-the-real-load-base"></a>
#### 2.4 The joined chain, at the real load base

```
0x10021544 → 0x10021548 → 0x1002154c   (op slot +0x25c3a8)
           → 0x1003989c                (corrupted field → fake table)
           → 0x1008d290                (write gadget)
           → 0x1010b634: 0x528078a1 → 0x52807921
err: None
```

The session object and the corrupted object are the same block in this run, because
`+0x21548` reads the command buffer from `[x20+0x5b8]` while `+0x398a8` forwards
`x20`. It proves the mechanics and the address arithmetic, not a real path.

<a id="el2_chain_state-25-load-base"></a>
#### 2.5 Load base

`out/anne-device-private/xloader-validation-v1/reference/factory-manifest.xml`:

```xml
<image path="xloader.img"  role="xloader"  address="0x00020000" />
<image path="fastboot.img" role="fastboot" address="0x10000000" />
```

Image-relative `+x` is runtime `0x10000000 + x`. The patch site is therefore
`0x1010b634`. The image begins with a `MAGICNUMBER!` header at `+0x28` and contains
9,893 absolute qwords pointing inside its own 4 MB, which means it is linked at 0
and either relocated at load or only ever referenced PC-relatively.

Caveat: this is the flashing tool's manifest — the address the boot chain expects —
not a value read back from a running handset.

<a id="el2_chain_state-3-negative-results-worth-keeping"></a>
### 3. Negative results worth keeping

<a id="el2_chain_state-31-no-usable-stack-pivot-therefore-no-return-oriented-chain"></a>
#### 3.1 No usable stack pivot, therefore no return-oriented chain

| mechanism | sites |
| --- | --- |
| `mov sp, x0` | `+0x1a0`, `+0x407c` — both in the EL3/QEMU entry stub |
| `mov sp, x1` | `+0x4110`, same stub |
| `msr SP_EL0/1/2, x0` | `+0x4080`, `+0x4084`, `+0x4088`, same 24 bytes |
| `mov sp, x29` | 4 ordinary epilogues |
| `mov x29, <reg>` from a data register | 0 |
| load `x30` from non-`sp` memory then `ret` | 0 of 4,679 `x30` loads |

Executing `+0x1a0` in Unicorn gives `SP = 0x90000000` (full stack control) and then
`UC_ERR_EXCEPTION` at `+0x1a8`, `msr VBAR_EL3`. At EL1 it faults earlier, at
`+0x4088`. It only runs to completion at EL3, which is not where this code runs.
`ret` is `br x30`, and every `x30` in the image is restored from the stack, so a
stackless dispatch chain does not exist either.

This corrects the earlier chain map, which marked "stage 6 — pivot source" as
proven. No pivot source had been identified.

<a id="el2_chain_state-32-fastboot-state-is-bss-not-heap"></a>
#### 3.2 Fastboot state is BSS, not heap

```
214c8: adrp x20, 0x292000
214d8: ldr  x21, [x20, #0x5b8]    ; command buffer pointer at 0x2925b8
```

The session object is the global area at `0x292000`, outside the arena, so the
overflow cannot reach it. "Corrupt the fastboot session" is not available.

<a id="el2_chain_state-33-no-allocation-on-the-command-path"></a>
#### 3.3 No allocation on the command path

169 allocator call sites exist; the reachability pass from the command dispatch
finds none of them on that path. Everything is allocated during module init.

<a id="el2_chain_state-34-what-the-tooling-got-wrong"></a>
#### 3.4 What the tooling got wrong

Four separate scans returned confident zeros because of bugs in the scanner, not in
the binary:

- capstone prints `bl #0x111680`; parsing `'#0x…'` as hex silently failed, so every
  `bl` target was missed.
- treating `sp` as register index 0 made `ldp x29, x30, [sp]` look attacker-fed.
- treating "loaded from a pointer I chose" as "value I chose" turned
  read-modify-write counters into false write-what-where hits.
- the xloader is **ARM32**, and its ELF maps `.text` to VMA `0x21000` from file
  offset `0x100`; searching for file offsets as addresses found nothing.

Current numbers come from capstone operands with a two-level taint model
(`ATK_PTR` = points at the buffer, so content is chosen; `ATK_VAL` = value chosen,
so a load through it is unknown).

<a id="el2_chain_state-4-boot-ordering"></a>
### 4. Boot ordering

From the bootloader log buffer in `recovery-dmesg-private.txt`. Line order is
execution order; the millisecond stamps are **not** usable as wall-clock — the
kernel log shows "Booting Linux" at 0.000 s and its next line at 20.15 s with
nothing between, and the logo plainly does not take five seconds.

```
1847  usb: -- usb_init -- done
1903  module --- usb_switch init end
2188  display: hisi_display_logo: +
2287  preboot: display_logo end
2325  usbloader: hwdog_certify...
```

The USB controller is initialised **before** the logo is drawn and `usbloader`
runs **after** it, so controller objects sit below the scratch buffer and are
unreachable by an upward overflow.

The logo record comes from **oeminfo**, which is what makes the trigger
controllable: the dimensions are read from a partition that can be written, not
fixed by firmware.

<a id="el2_chain_state-5-open-work-in-order"></a>
### 5. Open work, in order

1. **Relocation.** Decide whether the image relocates its absolute pointers at
   load. It governs whether any address planted in a buffer needs `+0x10000000`,
   and therefore whether the `+0x8d290` destination operand is written correctly.
2. **Trigger.** Establish which oeminfo record each boot mode draws — `prepare`
   takes an index, range-checked to 0–2 — and whether one boot can draw two.
3. **Victim.** With a deterministic bump allocator and an attacker-chosen second
   image size, the span is chosen rather than found. Candidates are the
   `usbloader`-era allocations (`+0x213dc`, `+0x23074`, `+0x239f4`, `+0x26908`,
   `+0x26c2c`, `+0x2a1e0`, `+0x2a228`, `+0x2a75c`, `+0x2aab4`, `+0x2b…`), all after
   the logo and mostly command buffers with list linkage at `+0x10`.
4. **`x0` forwarding.** The victim's corrupted field must be the one passed as
   `x0`; `+0x398a8` shows this cannot be assumed.
5. **Writability.** `.text` must be writable at the EL the chain runs at.
6. **BL31.** Whether the supplied SPSR is honoured at all. BL31 is encrypted and
   cannot be checked from this dump.

<a id="el2_chain_state-6-artefacts"></a>
### 6. Artefacts

- `out/anne-device-private/gadgetdb/gadgetdb.py` — gadget database, operand-based,
  two-level taint.
- `out/anne-device-private/el2-chain/base1000.py` — chain at base `0x10000000`,
  patches `0x1010b634`.
- `out/anne-device-private/el2-chain/fullchain.py` — same chain at base 0.
- `out/anne-device-private/findentry/` — dispatch, ops-store and reachability scans.
- Scheduled continuation: task `46f0fe7d-ba96-4493-a5df-573f719cc574`.

<a id="el2_chain_state-7-catch-up-pass-17-october-evening"></a>
### 7. Catch-up pass, 17 October evening

<a id="el2_chain_state-71-the-joined-chain-still-reproduces-unchanged"></a>
#### 7.1 The joined chain still reproduces, unchanged

`.venv/bin/python out/anne-device-private/el2-chain/base1000.py`:

```
base 0x10000000   target 0x1010b634
before 0x528078a1   after 0x52807921   patched=True   err=None
```

Trace `0x10021544 → 0x10021548 → 0x1002154c → 0x1003989c → 0x100398ac → 0x1008d290 → 0x1008d2a0`.
No drift from §2.4.

What the trace also shows is that the model is still the synthetic one: the object the
payload corrupts and the fastboot session are the same block, and `x0` arrives at
`+0x21544` as `0x25c398` because the script put it there. §2.4's caveat stands.

<a id="el2_chain_state-72-relocation-the-image-contains-no-relocator"></a>
#### 7.2 Relocation — the image contains no relocator

Three measurements, all on the 4 MB file:

1. **The dispatch table is static.** In the file, `0x25c398` holds `0x2b86c`, `0x25c3a0`
   holds `0x2cfec`, `0x25c3a8` holds `0x2abb8`. A scan of all 796,272 decoded
   instructions for stores whose computed address lands in `0x25c398..0x25c3c8` finds
   **none**; the same scan over the display ops table `0x25f250..0x25f2a0` also finds
   none. Nothing patches these words at runtime — they are file values.
2. **`0x25c398` is the object the dispatcher dereferences.** `+0x110f44` is the module
   registry lookup: it walks the array at `[0x291a30]`, `strcmp`s `[entry]` against the
   supplied name (`0x144cb8` = `"fastboot_ctrl"`) via `+0x112680`, and returns `[entry+0x18]`
   when `[entry+0xc]` is set. `+0x21544` then does `ldr x1,[x0,#0x10]; blr x1` → the word at
   `0x25c3a8`. So the gate really is reached through a static, file-resident pointer.
3. **There is no pointer fix-up loop.** A scan for the only pattern that could relocate a
   range in place — `ldr xD,[xB]` … `add xD,xD,<reg>` … `str xD,[xB]` — returns 29 hits,
   every one of them array-index arithmetic inside normal functions, none a boot-time pass.

Consequence: at any non-zero base, `blr 0x2abb8` faults on the first fastboot command.
Since the firmware demonstrably serves commands, one of these must be true — and only the
first is consistent with what is in the file: **the image runs at base 0**, an external
agent relocates it, or the table is copied to RAM and fixed outside these 4 MB. Nothing
in the file supports the second or third.

<a id="el2_chain_state-73-the-manifest-field-is-a-flash-offset"></a>
#### 7.3 The manifest field is a flash offset

`factory-manifest.xml` gives `xloader.img address="0x00020000"`. That is the canonical
Kirin BootROM offset — LBA `0x100` × 512 bytes — i.e. a byte offset in the boot area of the
eMMC, not a RAM address. `fastboot.img address="0x10000000"` is the same field, so it is
the fastboot partition's offset in flash (256 MiB in).

So §2.5's "image-relative `+x` is runtime `0x10000000 + x`" has no support: the value was
never a load address. `base1000.py` is a mechanics demonstration, not a model of the device;
`fullchain.py` (base 0) is the only model consistent with the image's own tables.

Practical effect on the chain: the destination operand of `+0x8d290` is 32-bit either way,
so `0x10b634` and `0x1010b634` are both writable by the same gadget — but the *fake table
entry* and any planted code pointer are not, and those must match the real base. Until a
base is read back from the loader or the device, the payload's pointer values are unfounded.

<a id="el2_chain_state-74-a-symbol-listing-is-embedded-in-the-image-and-it-does-not-fit-it"></a>
#### 7.4 A symbol listing is embedded in the image, and it does not fit it

`0x292a40..0x374b06` (925,894 bytes) is an `objdump -t` text dump sitting in the file's
slack and in the region the boot clears: 11,055 symbols, 3,266 `.text` functions, including
`hisi_display_logo`, `hisi_prepare_logo`, `prepare_bl31`, `load_encrypted_bl31_img`,
`cmd_hwdog_certify_relock`, `usr_fastboot_relock`.

It is **not this build's table**. At the listing's own values only 1.5% of function starts
decode as prologues; the best shift over ±0x3000 bytes reaches 3.6%, against a random
baseline of 0.8% in the same window. The listing puts `strcmp_asm` at `0x111680`, which is
the bump allocator. Treat it as a different build's table: useful vocabulary, useless
addresses. It is extracted to `out/anne-device-private/symbols/fastboot.symtab.txt`.

<a id="el2_chain_state-75-what-this-does-to-5"></a>
#### 7.5 What this does to §5

Item 1 is now bounded from the image side: no relocator exists inside the 4 MB, so the base
is either 0 or supplied externally. That is a smaller unknown than before, and it cannot be
closed without the loader or the device.

Items 2–4 are unchanged and still gate everything. The previous session's finding — one
`hisi_display_logo` per boot, cache NULL at first use because the BSS clear covers
`0x293608`, therefore no second larger centring call — remains the sharpest result in this
file, and it is the one that decides whether a chain exists at all. A boot mode that draws
twice (charging boot, or a fastboot redraw after preboot) is the single experiment that
moves it.

<a id="el2_chain_state-8-entry-map-s0-same-evening"></a>
### 8. Entry map (S0), same evening

<a id="el2_chain_state-81-one-route-in"></a>
#### 8.1 One route in

A scan of every module-op invocation in the image (`bl 0x110f44` → `ldr xN,[x0,#off]` →
`blr xN`, 533 sites, `out/anne-device-private/findentry/ops-dispatch.txt`) gives the display
object at `0x25f270`:

| off | fn | role |
| --- | --- | --- |
| `+0x00` | `0x4fac8` | `hisi_display_logo` — the only route to the centring routine |
| `+0x08` | `0x4df8c` | called by `0x4fd00` right after the logo record is fetched |
| `+0x10` | `0x4e508` | called from `0x4fe0c` on one panel branch |
| `+0x18` | `0x4ef74` | the logo draw → `0x4f020` → `0x4ebec` |
| `+0x20` | `0x4e16c` | backlight: 4 call sites pass `0`, `0x32`, `0x96`, `0xfa` |
| `+0x28` | `0x4e320` | used at `0x4fcbc` to init the geometry block |

No dispatch site in the whole image calls `+0x18`. `0x4ef74` is reached only from `0x4fd18`
inside `0x4fac8`. The entry is `display op[0]` or nothing.

<a id="el2_chain_state-82-three-callers-all-index-0"></a>
#### 8.2 Three callers, all index 0

| dispatch | function | identified by |
| --- | --- | --- |
| `0x291a0` | `0x29174` | handler for the oem command `boot`: table entry at `0x25bc10`, name `0x14a470` = `"boot"`, len 5 |
| `0x8a218` | `0x89be8` | rescue → recovery; logs `rescue: prepare to goto recovery!` (`0x171d48`) |
| `0xcae50` | `0xcae24` | sd update; logs `no_module: \n enter update_from_sd mode` (`0x198500`) |

All three pass `w0 = 0`, so the `cmp w21,#2` in `0x4fac8` never selects anything but
record 0 from these paths.

<a id="el2_chain_state-83-where-the-size-comes-from"></a>
#### 8.3 Where the size comes from

`0x4fac8` looks up the `logo` module (`0x15bf10`), then calls its op[0] with
`(index, &src, &geom)`; `0x4fcb0`–`0x4fcfc`. `geom` is `{+8: w, +0xc: h, +0x10: bpp}` and
the byte count is `(w+1)·(h+1)·bpp` (`0x4ec40`–`0x4ec84`), clamped against the panel width
and height at `[[0x291a88]+0x48]+4` / `+8`.

<a id="el2_chain_state-84-a-second-draw-in-one-boot-does-exist"></a>
#### 8.4 A second draw in one boot does exist

`0x29174` — the `boot` oem command — draws the logo and only then proceeds. That is a
second centring call in the same boot, on the same cache word, with the same index. §7.5's
"one draw per boot" holds only for boots with no oem command; it came from logs of boots
with no fastboot session in them.

<a id="el2_chain_state-85-what-is-left-precisely"></a>
#### 8.5 What is left, precisely

The overflow needs draw 2 larger than draw 1. The stock record is 1080 wide and the logged
logo payload is 4,924,880 bytes ≈ `1080 × 1140 × 4` — i.e. shorter than the panel, and the
clamp admits up to full panel height. A second draw from a taller record would run past a
buffer sized for the shorter one by up to `(panel_h − 1140) · 1080 · 4` bytes.

The next measurement, then, is narrow: does the `logo` module re-read the oeminfo record on
each op[0] call, and does the record it returns depend on a mode word that differs between
the preboot draw and the `oem boot` draw? If yes, the trigger is one boot that draws twice
with a taller second record. If no, the two draws are size-identical and the trigger needs a
panel-mode change between them instead — which is measurable in the same harness.

<a id="el2_chain_state-9-the-size-is-the-bmp-headers-s1-same-evening"></a>
### 9. The size is the BMP header's (S1, same evening)

<a id="el2_chain_state-91-logo-op0-0x55adc-parses-the-bitmap-at-draw-time"></a>
#### 9.1 `logo` op[0] = `0x55adc` parses the bitmap at draw time

The module's ops object is `0x25f520`; op[0] is the routine `hisi_display_logo` calls. It:

- reads the record table from `[0x291870]` — base at `+0`, capacity at `+4`, loaded-flag at `+8`;
- accepts indices `0..0x13` (the display side already clamps to `0..2`);
- for index *i*, takes the record offset `[table + i*4]`, checks the `'B','M'` signature
  (`0x55d30`–`0x55d44`), reads width at `+0x12`, height at `+0x16`, bpp at `+0x1c`, and the
  pixel offset at `+0x0a`;
- rejects bpp other than `0x10` (`0x55d88`);
- writes the geometry back at `0x56064`–`0x56084` as `{+8: w−1, +0xc: h−1, +0x10: 2}` and
  returns the pixel pointer as the source.

So the draw size is exactly **`bmp_w · bmp_h · 2`**, read out of the record on every call. The
only bound is the panel clamp in `0x4ec5c`–`0x4ec70`. The record lives in oeminfo, which is
writable, so the size is attacker-chosen — this is the part §1 already called controllable,
now with the exact field offsets.

<a id="el2_chain_state-92-the-fallback-is-a-second-independent-size-source"></a>
#### 9.2 The fallback is a second, independent size source

When the table's loaded-flag is clear (`0x55b48`), op[0] takes the branch at `0x55e14` and
fetches a **built-in resource** instead: module lookup `0x14f778`, then `blr x3` with
`w0` ∈ {`0x15f`,`0x160`,`0x161`} (one per index 0/1/2), `x1 = 0x800000` (an 8 MB cap) and
`x2` = the same buffer. Those three IDs are the firmware's own artwork — the boot logo and the
locked-state screens.

<a id="el2_chain_state-93-where-the-bitmap-lives"></a>
#### 9.3 Where the bitmap lives

The boot log puts it plainly:

```
1985 ms  no_module: [LOGO] get_logo_buffer_from_dtsi: logo_data->fb_base  = 0x5d000000
1985 ms  no_module: [LOGO] get_logo_buffer_from_dtsi: logo_data->fb_size  = 0x03000000
1985 ms  main: module --- logo init end
2188 ms  display: hisi_display_logo: +
```

48 MB starting at `0x5d000000`, initialised 200 ms before the first draw. The bitmap op[0]
parses and the surface it ends up on are the same memory.

<a id="el2_chain_state-94-the-unlocked-warning-is-not-a-second-centring-call-but-it-touches-the-same-buffer"></a>
#### 9.4 The unlocked warning is not a second centring call — but it touches the same buffer

The warning strings are `0x1702d8` (`"Your device has been unlocked and"`) and `0x170270`
(`"Your device has loaded a different operating system."`), referenced at `0x82c20` and
`0x82d3c`; `"Your device is booting now..."` at `0x16f938`. They belong to the GUI screens.
The shared draw helper `0x88480` (five callers, `0x81b64`, `0x821f8`, `0x82ae8`, `0x830c0`,
`0x83634`) goes through display `op+0x08` and `logo op+1` (`0x552d0`), never through `0x4fac8`
— and `0x4fac8` has no direct callers at all, so no GUI screen can enter the centring routine.

The warning screen therefore cannot populate `0x293608`. What it can do is repaint the surface
that `logo op[0]` reads its bitmap from, which is the same 48 MB buffer — that is the bridge
between the warning screen and the overflow, and it is a candidate for the size change rather
than for the call.

<a id="el2_chain_state-95-what-s1-now-is-exactly"></a>
#### 9.5 What S1 now is, exactly

One question with three candidates:

| candidate | what would have to be true |
| --- | --- |
| record re-read per draw | a write to the logo record between the preboot draw and `oem boot` changes `bmp_w·bmp_h` |
| lazy table load | the first draw takes the `0x15e14` fallback (built-in art), the second takes the record |
| surface repaint | a GUI screen between two draws rewrites the header bytes op[0] parses |

All three are decidable in the existing harness by calling the real `0x55adc` and `0x4ebec`
twice and printing the geometry each time — no device needed. That is the next run.

<a id="el2_chain_state-10-the-bitmap-is-read-once-so-the-size-can-only-move-by-a-parse-failure"></a>
### 10. The bitmap is read once, so the size can only move by a parse failure

<a id="el2_chain_state-101-logo-init-0x5533c-and-it-runs-before-the-first-draw"></a>
#### 10.1 `logo init` = `0x5533c`, and it runs before the first draw

It allocates the 12-byte descriptor, checks its own loaded byte (`[x19+8]`), and only then
reads: `blr x3` with `x1` = a handle from the `0x147e30`-named module, `w2 = 0x200000`
(a 2 MB cap), followed by three property lookups (`0x15feb0`, `0x151c68`, `0x151c70`).
The boot log puts this at 1985 ms, the first draw at 2188 ms.

So the record bytes are fetched once per boot. `logo op[0]` never re-reads them: candidate 1
in §9.5 (record re-read per draw) is dead, and candidate 2 (lazy load at draw time) only
survives when init itself failed and the loaded byte stayed clear.

<a id="el2_chain_state-102-therefore-the-only-lever-left-is-the-header-bytes"></a>
#### 10.2 Therefore the only lever left is the header bytes

`0x55adc` derives the geometry from whatever bytes sit at the record offset — and every
failure path converges on `0x55b90`, which sets `{w−1, h−1, 2}` from the **panel**, not from
the record. So the draw has two possible sizes:

| parse | geometry | bytes copied |
| --- | --- | --- |
| `'B','M'`, 16 bpp, w/h > 0 | the BMP's own w, h | `bmp_w · bmp_h · 2` |
| anything else | panel w, h | `panel_w · panel_h · 2` |

The second row is the whole-panel frame — 1080 · 2280 · 2 = 4,924,800 bytes on this panel,
which is the number in the boot log. So a second draw whose parse fails is automatically the
largest possible draw, while the first draw's allocation is whatever the record declared.

**That is the trigger, stated exactly:** get the first draw to succeed with a small record and
the second draw to fail its parse in the same boot. Candidate 3 in §9.5 is the only one still
standing, and it has a natural mechanism — the bitmap lives in the framebuffer region
(`0x5d000000`, 48 MB, §9.3), and §9.4 shows the GUI painting screens (including the unlocked
warning) through `0x88480`. A screen painted between the two draws rewrites the header bytes.

<a id="el2_chain_state-103-the-experiment-run"></a>
#### 10.3 The experiment, run

`out/anne-device-private/el2-chain/twodraw.py` drives the real `0x55adc` and `0x4ebec` twice
in one Unicorn run, against a 64×64 panel and a 32×24 record:

```
draw 1: geom w=31 h=23 bpp=2 src=0x20004840  -> 1536 bytes
  centring 1 ok  scratch=0x20060000  peak write=0x20060600  span=1536
draw 2: geom w=63 h=63 bpp=2 src=0x20004840  -> 8192 bytes
  centring 2 ok  scratch=0x20060000  peak write=0x20062000  span=8192  (alloc was 1536)
```

Between the two calls nothing changes except the first two bytes of the record. The first
draw parses, allocates 1536 and writes exactly 1536. The second fails the `'B','M'` check,
takes the panel branch, and writes **8192 bytes into the same 1536-byte allocation — 6656
bytes past its end**, with no fault, through the firmware's own memset and memcpy.

So the size jump is no longer an inference: it is what the shipped code does, measured. The
ratio is the BMP size against the panel, which on the real 1080-wide panel means anything
from a few kilobytes up to a 4.9 MB frame.

What is still not demonstrated is the real-world cause of the parse failure — the GUI
repaint in §10.2 candidate 3. That is the last piece of S1. Everything downstream (S2 span,
S3 victim) can now be built against a measured overflow end address instead of a hypothetical
one.

<a id="rd_mode"></a>
## Fastboot RD mode — 18 October 2026

<a id="rd_mode-fastboot-rd-mode-18-october-2026"></a>

RD mode bypasses the shared fastboot command-permission gate, but its OEMINFO
representation is an RSA authorization token. Setting a payload byte to one
does not enable it. This is independent of xloader's lifecycle-based image
authentication.

This analysis uses the handset's captured fastboot image with SHA-256
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
Addresses are raw offsets with the disassembler based at zero. No phone commands
or writes were issued for this investigation.

<a id="rd_mode-oeminfo-authorization-path"></a>
### OEMINFO authorization path

1. Getter `0x2bce0` reads 256 bytes from OEMINFO field `0x82` at `0x2bd30`.
2. It constructs the four-byte message `00 00 00 01` at `0x2bd50`–`0x2bd5c`.
3. At `0x2bd8c` it calls `0xd54ec` with hash options `2` and RSA key selector `2`.
4. Hash helper `0xd4f54` sees option bit 1 set and appends four device-derived
   bytes obtained through `0xd488c`. It hashes the resulting eight bytes using
   the SHA-256 path. The suffix comes from the helper named `get_hun`;
   `0xd60b4` derives it from characters `[20:28]` (zero-based, end exclusive) of
   the 32-character storage CID representation supplied by `get_cid` at `0xd5f3c`.
   The parsed 32-bit word is stored little-endian. These are storage identity
   bytes, not the hardware lifecycle value.
5. `0xd3fe8` performs an RSA public operation on the 256-byte OEMINFO token,
   checks type-1 padding (`00 01 FF…FF 00`), and extracts the remaining bytes.
   Wrapper `0xd54ec` requires exactly 32 recovered bytes at `0xd5774`–`0xd577c`
   and compares them with the calculated hash at `0xd5978`–`0xd5984`.
   The recovered value is the raw digest, without an ASN.1 DigestInfo prefix.
6. Only verifier success returns RD state `1` at `0x2bd9c`. A missing OEMINFO
   record, failed read, or failed check follows a return-zero path.

The key table is at `0x2903c0`, referenced through pointer `0x2917a0`. Selector
2 points to the public key at `0x258170`: a 256-byte modulus, length at `+0x100`,
exponent bytes at `+0x102`, and exponent length at `+0x202`. The modulus is
2048 bits and the public exponent is 65537. SHA-256 of the stored modulus bytes:

`ab561e9c512d9f2c8091ec34150ed2673884497bbae2952402f1b411e64fd0e9`

The public key permits verification, not generating a token. Constructing a
normally accepted token requires the corresponding private key, an existing
valid token for the same derived input, or a separately established bypass.
No such token or bypass was established in this investigation.

<a id="rd_mode-effect-and-other-state-paths"></a>
### Effect and other state paths

Initializer `0x2cebc` stores the getter's result into cached state `+0x214` at
`0x2cf00`. The command gate `0x2abb8` checks it first: value `1` branches at
`0x2abd8` directly to the allow result at `0x2ac74`, bypassing its user-lock,
FRP and allowlist checks. Individual command handlers can impose further checks;
this does not create handlers for unsupported commands.

A separate runtime path at `0x2b2ac` accepts a 256-byte RSA response using key
selector 1, requires 16 recovered bytes, compares them with cached challenge
data, and then sets RD state at `0x2b518`. This is another authorization path,
not evidence of an unsigned enable flag. The clear path writes cached RD state
zero at `0x2ba58`.

An initialization fallback at `0x2cfc0` calls `0x2bbf8`, which reads NVME item
`0x138` named `FBLOCK` and interprets a zero data byte as RD enabled. This is a
distinct non-OEMINFO source, selected by initialization conditions. Its presence
does not mean writing OEMINFO field `0x82` can select that path.

<a id="rd_mode-saved-handset-records"></a>
### Saved handset records

Field `0x82` uses the 4 KiB slot at `(0x82 - 1) << 12 = 0x81000`; the backup
slot is at `0x2081000`. Both complete slots are zero in both of these images:

- Original handset backup: `out/anne-device-private/oeminfo-dump-20261017/oeminfo.img`.
- Supplied and flashed replacement: `out/anne-device-private/flash-oeminfo-el2-20261018/oeminfo-el2.img`.

Neither saved image contains an OEMINFO RD token. These observations concern
the saved files, not a new read of the currently running phone.

Evidence and reproducible extraction are in
`out/anne-device-private/rd-mode-20261018/`: `analyse.py`, `analysis.json`, and
`evidence.dis.txt`. Run `.venv/bin/python out/anne-device-private/rd-mode-20261018/analyse.py`.

<a id="rd_mode-focused-verifier-audit"></a>
### Focused verifier audit

One concrete parser defect was reproduced offline: the type-1 padding scanner
reads one byte beyond its 256-byte RSA output allocation when the recovered
block starts `00 01` and every remaining byte is `FF` (no zero delimiter).
At `0xd4318`, the pre-incrementing byte load reaches offset `0x100`; the end
comparison at `0xd4310` is reached after that load when the loaded byte is `FF`.
If the byte beyond the allocation is zero instead, subsequent length checks
reject the resulting invalid remaining length. The offline test observed this
one-byte overread and a nonzero wrapper result (`19`), with no digest comparison.

This is an out-of-bounds read in the parser, not a demonstrated RD authorization
bypass. The test injects a synthetic output at the RSA-operation boundary; it
does not provide an attacker-generated signature producing that output under
the embedded public key. No device execution, crash, disclosure or signature
forgery was demonstrated.

The audit executed the captured AArch64 verifier, key-selection, parser, bounded
copy and comparison instructions in Unicorn across 32 cases. RSA-operation
outputs, hash generation and crypto return statuses were modeled; hardware and
signature arithmetic were not exercised. A matching synthetic digest was the
sole accepting control. Changed header/padding/digest bytes, recovered lengths
of 0, 1, 16, 31, 33, 64, 245 and 246 bytes, injected hash/RSA/key-import errors,
and failure of each of the eight baseline allocations all rejected. The key
allocation failure case models the crypto importer rejecting a null destination;
this does not establish how the real importer handles that argument.

The four public moduli in the key table have pairwise greatest common divisor
one, so this set does not expose a shared-prime factorization. That check is not
a general proof of RSA key quality or strength.

Two further limitations are visible without constituting a demonstrated keyless
bypass:

- The persistent authorization has no fresh challenge or expiry in this path.
  Its device-derived input contains at most 32 bits, so an issued token would
  also match another device with the same derived value and embedded key.
  This observation does not supply a valid token.
- The digest comparison at `0x111b8c` exits on the first unequal byte. This is
  not constant-time. A useful hardware timing oracle or signature forgery based
  on it has not been established; RSA output and expected-digest comparison
  are different operations.

Reproduce the audit with
`.venv/bin/python out/anne-device-private/rd-mode-20261018/audit.py`.
`audit.json` records each case, return status, allocation event, comparison
length and padding-scanner boundary read.

<a id="oeminfo_consumers"></a>
## Fastboot OEMINFO consumers — fresh handset analysis, 2026-10-17

<a id="oeminfo_consumers-fastboot-oeminfo-consumers-fresh-handset-analysis-2026-10-17"></a>

Fresh TWRP read of `/dev/block/platform/hi_mci.0/by-name/fastboot`: 4,194,304 bytes. Device SHA-256 before and after transfer and the host hash all match:

`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`

The inventory contains **37 direct OEMINFO operation sites in 29 functions** (29 reads, 6 writes, 2 erases), resolving to **33 distinct field IDs**. It also records 29 reads and 9 writes of cached OEMINFO-derived lock state.

All addresses below are offsets in this freshly captured binary, disassembled at base zero. Function boundaries are inferred from code and in-image function pointers. Names come from this binary’s diagnostics, command-registration tables, or the described behavior. No previous disassembly, previous consumer list, or external symbol table was used for this inventory. The phone was only read during this analysis.

<a id="oeminfo_consumers-oeminfo-entry-points"></a>
### OEMINFO entry points

| Item | Address | Meaning |
| --- | --- | --- |
| Module descriptor | `0x28d860` | Name pointer `0x14f778` contains `oeminfo`; ops pointer is `0x28d880`. |
| Module registry lookup | `0x110f44` | Returns the named module’s operations. |
| Initializer | `0xa400c` | Acquires storage operations and allocates scratch buffers. |
| Read | `0xa496c` | Ops `+0`: `(field_id, maximum_bytes, output_buffer)`. |
| Write | `0xa45f0` | Ops `+8`: `(field_id, bytes, input_buffer)`. |
| Erase | `0xa48d8` | Ops `+0x10`: `(field_id, bytes, scratch_buffer)`. |

<a id="oeminfo_consumers-every-direct-operation-site"></a>
### Every direct operation site

Lengths below are requested limits, not proof of the stored record’s length. The read implementation copies the smaller of the request and the validated stored length. `arg2` in the generic command handler is its caller-supplied length. The `writeOemInfo` wrapper row resolves its argument through its sole direct caller, as described.

| Call site | Function entry | Operation | Field IDs | Bytes | Consumer and use |
| --- | --- | --- | --- | --- | --- |
| `0x21ca4` | `0x21c58` | erase | `0xa7` | `0x80` | **Crypt-info reset** — erase/flash cleanup. Erase the 128-byte crypt-info record (0xa7); direct callers at 0x22960, 0x22eb0, 0x3a1e0. |
| `0x2714c` | `0x270c4` | read | `0x4e` | `0x80` | **Build-number response** — get-build-number. Append the 128-byte build-number string to the command response. |
| `0x2bd30` | `0x2bce0` | read | `0x82` | `0x100` | **RD-mode state** — fastboot_ctrl initialization. Read 256 bytes of RD-lock material and pass it to helper 0xd54ec with a fixed four-byte input; use the result as a boolean state. |
| `0x2bebc` | `0x2be8c` | read | `0x43` | `0xc` | **Root-state getter** — fastboot_ctrl initialization; relock/lock. Interpret little-endian words at payload +0 and +4; either equal to 1 produces a true root-state result. |
| `0x2c2b4` | `0x2c274` | read | `0x5d` | `0x20` | **User-lock state getter** — fastboot_ctrl initialization. Compare all 32 bytes with helper-generated digests of the two fixed lock-state strings. |
| `0x2c440` | `0x2c3d0` | write | `0x5d` | `0x20` | **User-lock state writer** — oem unlock/relock; flashing unlock/lock. Generate the selected 32-byte lock-state digest and write the whole record. |
| `0x2d3cc` | `0x2d38c` | read | `0x43` | `0xc` | **Root-information response** — oem check-rootinfo. Read three words: payload +0 and +4 select root-state text; +8 goes to helper 0x2d0b4 for the additional reason/status text. |
| `0x32e70` | `0x32e3c` | read | `0x62` | `0x4` | **FRP marker getter** — FRP-state getter; fastboot_ctrl ops. Compare the four-byte payload with 0x00465250; return equality. |
| `0x32ef8` | `0x32ebc` | read | `0x5f` | `0x4` | **FRP state resolver** — fastboot command permissions; fastboot_ctrl initialization. Read the four-byte flag; nonzero returns state 2. Zero or read failure consults the 0x62 marker, then the FRP partition's final byte. |
| `0x332e8` | `0x33210` | read | `0x63` | `0x134` | **FRP-password verifier** — oem frp-unlock. Read a 308-byte password/authentication record and feed it into the crypto/password comparison path. |
| `0x34138` | `0x340ac` | read | `0x5b` | `0x80` | **Product-model response** — get-product-model. Copy and return the model string from the 128-byte record. |
| `0x34388` | `0x3432c` | read | `0xa7` | `0x80` | **Crypt-info response** — get_crypt_info. Copy the first four bytes of 0xa7 as a word and format crypt info is <%d>. |
| `0x345c8` | `0x34574` | read | `0xa7` | `0x80` | **Crypt-enable response** — check_crypt. Read payload byte +8 and format crypt enable is <%d>. |
| `0x34b98` | `0x34b28` | erase | `0x12`, `0x61`, `0x8`, `0xa7`, `0x51`, `0x86`, `0x91` | `0x80` | **Table-driven OEMINFO eraser** — oeminfoerase. Field ID is an argument from the seven-row erase table. Erase 128 bytes; table-derived IDs are recorded in tables.json. |
| `0x352e0` | `0x3507c` | read | `0xa7`, `0x6f`, `0xac`, `0xad`, `0xae` | `arg2` | **Table-driven OEMINFO reader** — oeminforead. Select the field ID from row +0xc in the five-row table. Length and output buffer come from the wrapper; all five rows permit reading. |
| `0x35504` | `0x3507c` | write | `0xa7` | `arg2` | **Table-driven OEMINFO writer** — oeminfowrite. Select the field ID from row +0xc. The access-mode check permits writing only cryptinfo (0xa7) in this table; the four version rows are read-only. |
| `0x35b14` | `0x35a90` | read | `0x8c` | `0x800` | **HOTA response builder** — oeminforead HOTA special case. Read the 2048-byte HOTA information record; check/combine it with device model and initial HOTA digest before returning it. |
| `0x35b84` | `0x35a90` | read | `0x61` | `0x20` | **HOTA device-model input** — oeminforead HOTA special case. Read a 32-byte model string and append it to the HOTA response through hotainfo_check_and_combi. |
| `0x35c1c` | `0x35a90` | read | `0x8b` | `0x40` | **HOTA initial-digest input** — oeminforead HOTA special case. Read 64 bytes identified by the diagnostic as hotainfo_init, compare with the calculated digest in the HOTA combination path. |
| `0x36798` | `0x36748` | read | `0xa7` | `0x80` | **Crypt-enable update: read** — enable_crypt; disable_crypt. Read the whole 128-byte 0xa7 record before updating byte +8. |
| `0x367bc` | `0x36748` | write | `0xa7` | `0x80` | **Crypt-enable update: write** — enable_crypt; disable_crypt. Write the same 128-byte record after setting payload byte +8 to 1 or 0. Wrappers are 0x3686c and 0x36874. |
| `0x36dac` | `0x36d30` | read | `0x96` | `0x42` | **Encrypted-UDID response** — getencryptudid. Require payload byte +0 to equal ASCII '1'; pass the following 64 bytes to helper 0x3687c and format its output. |
| `0x3708c` | `0x37050` | read | `0x7c` | `0x20` | **NFF/test-mode flag** — get-psid helper. Compare the 32-byte string with TRUE; callers at 0x37434 and 0x378e8 use the result to choose the PSID path. Name is inferred from this use. |
| `0x37670` | `0x372e4` | read | `0x57` | `0x111` | **PSID response** — get-psid. Read the 273-byte PSID record, use its leading mode byte and the remaining structured data in the PSID response path. |
| `0x37bec` | `0x37b74` | read | `0x12` | `0x40` | **Vendor/country response** — vendorcountry. Copy the 64-byte vendor/country string into the response. |
| `0x37d70` | `0x37cf8` | read | `0x61` | `0x40` | **Device-model response** — devicemodel. Copy the 64-byte device-model string into the response. |
| `0x37fdc` | `0x37f54` | read | `0xb3` | `0x40` | **System-update-state response** — system_update_state. Copy the 64-byte system-update-state string into the response. |
| `0x42f68` | `0x42f20` | read | `0x5c` | `0x1` | **One-shot initrc-tracer flag: read** — kernel command-line construction. If the byte is nonzero, append initrc_tracer=1 to the kernel command line. |
| `0x42fe0` | `0x42f20` | write | `0x5c` | `0x1` | **One-shot initrc-tracer flag: clear** — kernel command-line construction. Clear the one-byte flag after consuming it. |
| `0x55e60` | `0x55adc` | read | `0x15f`, `0x160`, `0x161` | `0x800000` | **Logo record loader** — logo module prepare operation. Read up to 8 MiB: logo type 0 selects 0x15f, type 1 selects 0x160, type 2 selects 0x161. Parse the returned container/header and BMP, then provide pixels to the display path. |
| `0x55fc0` | `0x55adc` | read | `0x15f` | `0x800000` | **Logo fallback loader** — logo module prepare operation. If the selected new-logo read fails, retry the legacy resource in field 0x15f and continue the logo parser. |
| `0x89368` | `0x892c4` | read | `0x65` | `0x5c` | **Rescue main-version response** — rescue module operation. Read and copy the 92-byte main-version string into the response; emit the missing-version message on failure. |
| `0x8baa8` | `0x8ba6c` | write | `0xa6` | `0x5d` | **Generic sysupdateauth writer** — saveTokenInfo chain. Signature is writeOemInfo(id, buffer, length). The sole direct caller at 0x8c2b4 supplies ID 0xa6 and requires length 0x5d (93 bytes); record purpose is update-authentication token storage. |
| `0x8ef54` | `0x8eefc` | write | `0x85` | `0xd` | **Third-party-recovery marker** — load_kernel module. When its input mode equals 2, write the 13-byte NUL-terminated literal 3rd_recovery to field 0x85. |
| `0xc1d00` | `0xc1bd4` | read | `0x61` | `0x20` | **Modem-ID model input** — modemid module initialization. Read the 32-byte device-model string, map it through the device-model table, and combine the result into modem/product selection. |
| `0xc2174` | `0xc1bd4` | read | `0x5e` | `0x20` | **Modem-ID preferred vendor input** — modemid module initialization. Read the preferred vendor/country string from 0x5e. The adjacent diagnostic names it OEMINFO_VENDER_AND_COUNTRY_NAME_COTA. Empty data or read failure falls back to 0x12. |
| `0xc21a8` | `0xc1bd4` | read | `0x12` | `0x20` | **Modem-ID fallback vendor input** — modemid module initialization. Read the 32-byte vendor/country fallback and use the vendor mapping in modem/product selection. |

The two vendor-related IDs must remain distinct: the modem path reads `0x5e` and calls it COTA in its diagnostic, while the erase table names `0x91` as `vendorcountrycota`. This report preserves both observed IDs without treating them as aliases.

<a id="oeminfo_consumers-table-driven-field-consumers"></a>
### Table-driven field consumers

The generic read/write handler at `0x3507c` loads the table pointer from `0x291bc0`; it points to `0x25c4b8`. There are five 32-byte rows. Row `+8` selects backend, `+0xc` is the field ID, and `+0x18` is the access mode. Backend zero selects OEMINFO. Access mode zero permits reading, and mode two permits reading and writing.

| Table row | Command item | Field ID | Permitted OEMINFO operations |
| --- | --- | --- | --- |
| `0x25c4b8` | `cryptinfo` | `0xa7` | read/write |
| `0x25c4d8` | `SYSTEM_VERSION` | `0x6f` | read |
| `0x25c4f8` | `SYSTEM_VERSION_B` | `0xac` | read |
| `0x25c518` | `ANDROID_VERSION` | `0xad` | read |
| `0x25c538` | `ANDROID_VERSION_B` | `0xae` | read |

The erase handler follows `0x34f14` → one-item `0x34db0` or all-items `0x34ea8` → `0x34d40` → tail call `0x34b28`. It loads its table pointer from `0x2914e0`, pointing to `0x25c558`. There are seven 24-byte rows; backend and field ID are again at `+8` and `+0xc`. Every current row selects OEMINFO.

| Table row | Erase item | Field ID |
| --- | --- | --- |
| `0x25c558` | `vendorcountry` | `0x12` |
| `0x25c570` | `devicemodel` | `0x61` |
| `0x25c588` | `ver` | `0x8` |
| `0x25c5a0` | `cryptinfo` | `0xa7` |
| `0x25c5b8` | `single_sim` | `0x51` |
| `0x25c5d0` | `dm-verity` | `0x86` |
| `0x25c5e8` | `vendorcountrycota` | `0x91` |

`oeminforead` enters wrapper `0x363c8`; `oeminfowrite` enters `0x3567c`. The read wrapper also calls `0x35a90` for its HOTA special case. Command registration slots are `0x25c718`, `0x25c730`, and erase slot `0x25c700`.

<a id="oeminfo_consumers-consumers-of-cached-lock-fields"></a>
### Consumers of cached lock fields

Initializer `0x2cebc` populates the state object addressed by the pointer at `0x292000`. It stores RD mode at `+0x214`, user-lock state at `+0x218`, root state at `+0x21c`, and resolved FRP state at `+0x224`. These values can subsequently be overridden by initialization/error or command paths; the source IDs below describe initial provenance, not an immutable runtime value.

| Initial field sources | State offset | All identified cached read sites | All identified cached write sites |
| --- | --- | --- | --- |
| `0x82` | `0x214` | `0x2abd0`, `0x2ad48`, `0x2afc0`, `0x2b858`, `0x2ba10`, `0x2cf28`, `0x2cfd8`, `0x2d098`, `0x2d764`, `0x2d7dc`, `0x2d7ec`, `0x33138`, `0x34958` | `0x2b518`, `0x2ba58`, `0x2cf00`, `0x2cfc8` |
| `0x5d` | `0x218` | `0x21b08`, `0x2abe0`, `0x2b02c`, `0x2b864`, `0x2c638`, `0x2c944`, `0x2cf44`, `0x2cfe4`, `0x2d0a4`, `0x2d828` | `0x2c6d4`, `0x2ca84`, `0x2cf0c` |
| `0x43` | `0x21c` | `0x2b84c`, `0x2cf5c`, `0x2cf9c`, `0x2d080` | `0x2cf18` |
| `0x5f`, `0x62` | `0x224` | `0x2ac88`, `0x2cf8c` | `0x2cf20` |

The command-permission function at `0x2abb8` reads RD mode (`0x2abd0`), user-lock state (`0x2abe0`), and FRP state (`0x2ac88`). Additional readers serve lock/root status, command dispatch, and unlock/relock paths. `cached-state.json` records each access with its containing function; `cached-state.dis.txt` contains assembly evidence windows around their function entries. This table covers reads of the four derived state fields, not every later use of values copied out of them.

<a id="oeminfo_consumers-oeminfo-header-field-consumers"></a>
### OEMINFO header-field consumers

The common layer consumes record metadata independently of the payload-specific callers above. Each record has a 512-byte header, and the backup region is at primary offset `+0x02000000`.

| Header field | Consumer | Evidence and use |
| --- | --- | --- |
| `+0`, `+4`: magic words | `0xa4100`, `0xa496c` | Compare with `0x5f4d454f` and `0x4f464e49` (`OEM_INFO`). Read checks at `0xa49f0` and `0xa4a04`; age-reader checks at `0xa41a8` and `0xa41bc`. |
| `+8`: version | `0xa45f0` | Writer stores version 6 at `0xa46ec`. No version comparison was found in the read/age helpers. |
| `+0xc`: field ID | `0xa496c`, `0xa45f0` | Reader loads it at `0xa4a18` and compares with the requested ID. Writer stores it at `0xa46f4`. |
| `+0x14`: payload byte count | `0xa496c`, `0xa45f0` | Reader loads at `0xa4a24` and `0xa4a30`, validates the size class through `0xa3e24`, then clamps transfer length. Writer stores it at `0xa46f8`. |
| `+0x18`: age | `0xa4100`, `0xa4240`, `0xa45f0` | Age is read at `0xa4160`, validated against 999,999,999, and compared between primary/backup records by `0xa4240`. Writer increments modulo 1,000,000,000 and stores at `0xa4700`. |
| Field ID and size parameters | `0xa3edc`, `0xa3e24` | Compute slot offsets and enforce ID-dependent payload limits. Logo IDs `0x15f`–`0x161` use 8 MiB slots with a maximum payload of `0x7ffe00`. |
| Erase bookkeeping | `0xa48d8` → `0xa4804` | Common erase path operates on both record regions; it is shared by the two erase call sites above. |

<a id="oeminfo_consumers-evidence-and-coverage"></a>
### Evidence and coverage

All 31 OEMINFO module lookups resolved by the control-flow pass are accounted for by operation sites. An independent PC-relative string-reference pass also checks diagnostics mentioning OEMINFO; its extra hits are common storage helpers or wrapper/caller paths, retained in `coverage.json`. There are no direct `bl` calls to the three OEMINFO API functions, and their only aligned data-pointer references in the captured image are the three operation-table slots. The inventory follows those indirect operations, including reuse of a returned operations pointer for multiple reads/writes.

The reverse direct-call/tail-call closure and in-image function-pointer registrations for each consumer are retained in `inventory.json` under `consumer_caller_routes`. This includes command wrappers and module initialization routes without relying on an older symbol map.

This is a static inventory of the captured firmware. Runtime execution of every command was not attempted. Arbitrary unresolved indirect calls and data copied into unrelated structures prevent a proof that every possible downstream use has been identified. Five non-OEMINFO analysis functions reached the worklist bound; none appears in the independent OEMINFO string-reference sweep. Their addresses are retained in the JSON warnings.

Fresh evidence directory: `out/anne-device-private/oeminfo-consumers-20261017/`.

- `capture.json`, `fastboot.img`: fresh handset capture and checksum evidence.
- `fastboot.dis.txt`: freshly generated AArch64 disassembly.
- `consumers.csv`, `consumers.json`: every operation site, IDs, lengths, command route, and usage notes.
- `fields.csv`: all 33 resolved IDs mapped back to consumer sites.
- `tables.json`: all table-derived IDs, backend selectors, and access modes.
- `inventory.json`, `coverage.json`: data-flow results, caller closure, string references, and analysis limits.
- `contexts.dis.txt`, `cached-state.dis.txt`, `support.dis.txt`: fresh assembly evidence for payload readers, cached readers, wrappers, and header helpers.
- `disassemble.py`, `analyse.py`, `annotations.json`, `write_report.py`: local analysis and report sources.

Rebuild the fresh disassembly, inventory, and report from this captured binary:

```sh
.venv/bin/python out/anne-device-private/oeminfo-consumers-20261017/disassemble.py
.venv/bin/python out/anne-device-private/oeminfo-consumers-20261017/analyse.py
.venv/bin/python out/anne-device-private/oeminfo-consumers-20261017/write_report.py
```

<a id="pre_el1_loads"></a>
## Fastboot eMMC inputs before EL1 — 18 October 2026

<a id="pre_el1_loads-fastboot-emmc-inputs-before-el1-18-october-2026"></a>

Scope: parsers and loads executed by the captured fastboot image before its
normal BL31/kernel handoff. The image SHA-256 is
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
All addresses are raw image offsets, not asserted runtime addresses.
No device commands or writes were performed for this analysis.

<a id="pre_el1_loads-exception-level-boundary"></a>
### Exception-level boundary

This fastboot entry path is EL3 code. It accesses `ACTLR_EL3` at `0xbc`/`0xc4`,
installs `VBAR_EL3` at `0x1a8`, sets `TTBR0_EL3` at `0x1c4`, and enables its
EL3 MMU/cache through `SCTLR_EL3` at `0x1dc`–`0x1ec`. These instructions require
EL3; the mere presence of an `_EL1` register name elsewhere does not imply a
transition to EL1. This is static evidence of the intended execution path,
not a newly sampled CurrentEL value on the live phone.

The normal kernel handoff dispatcher is `0x910d0`. Its trustfirmware operation
call at `0x91158` resolves to `0x10b5b8`. That routine constructs BL31 parameters,
puts SPSR `0x3c5` (EL1h with exceptions masked) into the non-secure entry record
at `0x10b634`/`0x10b640`, and branches to BL31 at physical `0x35e00000` through
`0x10b720` → `0x348`. This branch transfers into BL31; it is not itself an EL1
exception return. BL31's eventual non-secure entry is the later boundary.

The inputs below belong to fastboot's side of that boundary. Firmware copied
for an MCU/DSP has its own execution context; that does not change the EL3
context of the fastboot code reading and parsing it. Linux drivers, filesystem
mounts, Android init and EL1 peripheral-firmware loading are outside this list.

<a id="pre_el1_loads-identified-reads-and-load-paths"></a>
### Identified reads and load paths

| eMMC input | Fastboot use before handoff | Evidence and qualification |
| --- | --- | --- |
| `oeminfo` | Lock/configuration records, model/vendor information, logo containers/BMPs | APIs `0xa496c`/`0xa45f0`; full consumer inventory in `OEMINFO_CONSUMERS.md`. Logo reads at `0x55e60`/`0x55fc0`. Parsing occurs in fastboot, not a kernel display driver. |
| `nvme` | NVE item database supplying boot configuration and flags | Module init `0xa3568`, reader operation `0xa3a84`; preboot lookup at `0x1d100`, and kernel-load configuration query at `0x90058`–`0x9005c`. This module name means Huawei NVE storage, not the NVMe bus protocol. |
| `misc` | Bootloader/recovery command and recovery-state data | Reader `0x97998` issues a 2048-byte storage read at `0x979e0`, then copies its message structure. Preboot consumers include `0x1c344` onward. |
| `dts` | Multi-DTB header/table, selected compressed device tree and associated data; later FDT parsing and edits | Header read at `0x6eb90`, table read at `0x6ec08`, selected-data read at `0x6ed50`, additional read at `0x6e9e8`, decompression at `0x6ed94`. The name is loaded from `0x147e30`. |
| `trustfirmware` | BL31 image/header, loaded by fastboot before entering BL31 | Wrapper invoked at `0x909a0`. Operation `0x10bb04` reads a 256-byte header at `0x10bb48`; its non-encrypted-header path reads `0x40000` bytes at `0x10b984`. Header magic selects the loading/verification path. |
| `teeos` | Secure OS image/header, subsequently passed into the BL31 handoff | Wrapper invoked at `0x909a8`; loader `0x10bfd0` reads 256 bytes at `0x10c008` to `0x37a00000`, then selects the encrypted/other image path. It is fastboot doing this read; the secure OS runs later. |
| `lpm3` | Low-power MCU image | Non-encrypted loader read at `0x9243c`, offset zero, `0x3fc00` bytes to `0x3c0000`; security check follows at `0x92464`. Encrypted-header selection also exists. The auxiliary loader sequence invokes the `lpm3` module at `0x1bb78`. |
| `fw_hifi` | Audio DSP header, section table and sections | Hifi loader reads a `0x660`-byte header at `0x8df00`, then processes sections. Invoked through the auxiliary-loader sequence at `0x1bbc0`. Boot-mode and configuration checks apply. |
| `sensorhub` | IOM3 firmware image | Read at `0x8e860`: offset zero, up to `0x200000` bytes to `0x36000000`, followed by security check at `0x8e8ac`. **Conditional:** `0x8e808`–`0x8e81c` skips this path for chip-family value `0x62502000`; it must not be called an unconditional ANNE load. |
| `vbmeta` and recovery/eRecovery variants | AVB metadata and verification of selected boot partitions | Main boot loader calls the `verify_boot` module before handoff; mode-specific names appear in code at `0xcbe94`, `0xcbf68`, and adjacent recovery branches. Which reads occur depends on mode and AVB path. |
| `ramdisk`, `recovery_ramdisk`/`recovery_vendor`, `erecovery_ramdisk`/`erecovery_vendor` | Selected initramfs images, handled by fastboot/AVB before entry to EL1 | Mode-dependent selection is explicit in `0x8f804`–`0x8f870`. Generic loading/decompression lives around `0x8fc4c`; the explicit path at `0x8fdec` has a Hi6250 skip, so it is not proof that that particular decompressor runs on this handset. |

These rows identify code paths, not a runtime trace asserting every input is
read in every boot. They are not a proof of an exhaustive inventory of all
indirect reads. Image authentication and validation must be traced per path;
this table does not label an input as unauthenticated merely because it is read.

<a id="pre_el1_loads-ordering-relative-to-kernel-loading"></a>
### Ordering relative to kernel loading

"Before EL1" is wider than "before the first kernel read." Fastboot can read
kernel headers while still at EL3. In its main boot-image routine the direct
sequence at `0x90998` onward is:

1. Select/update boot mode (`0x904c8`).
2. Load BL31 (`0x905f0`) and probe the HHEE hook (`0x90708`).
3. Load the secure OS (`0x907a4`).
4. Obtain/check the selected kernel header through `0x8f79c` and run image
   verification/loading through `0x903f4`.
5. Build command-line/device-tree/handoff data, then dispatch into BL31.

Earlier or mode-specific header probes also exist, so this sequence is not a
claim that no kernel byte was read anywhere earlier in startup. Device trees,
NVE, OEMINFO and misc are consumed in earlier initialization/preboot paths too.

HHEE is a hook lookup at `0x9071c`, not an established eMMC image load. Modem
initialization is invoked in the auxiliary sequence, but its post-load operation
`0xfd208` is a bare `ret`; the module name alone is not proof that this fastboot
loads every modem firmware partition before the kernel. These are deliberately
excluded from the confirmed executable-image list.

Fresh evidence windows and their source hash are retained in
`out/anne-device-private/pre-kernel-loads-20261018/`.

<a id="xloader_validation"></a>
## Exact-device fastboot authentication evidence

<a id="xloader_validation-exact-device-fastboot-authentication-evidence"></a>

14 October 2026, ANE-LX1 8.0.0.151(C782), Kirin 659/Hi6250.
This updates the previously unresolved authentication gate in
[FASTBOOT_EL2.md](boot.md#fastboot_el2).

<a id="xloader_validation-result"></a>
### Result

**The existing EL2 instruction patch fails the captured fastboot content hash.
Replacing that stored hash also fails the existing certificate signature.**
This is now reproduced from this handset's own signature metadata, rather
than inferred only from Huawei secure-boot descriptions. It rules out using
the unchanged authentication metadata with the patched persistent image, or
repairing it by merely recalculating a checksum.

No modified fastboot, xloader, VRL or factory loader was executed or written
to the phone. Runtime rejection of a modified image was not deliberately
tested. An authentication bypass remains a separate research question.

<a id="xloader_validation-read-only-capture"></a>
### Read-only capture

The explicitly requested original shared-kernel restoration completed and its
full partition SHA-256 was read back from installed TWRP. The subsequent
capture checked the original device serial, `ro.product.device=anne`,
TWRP `3.7.0_9-0`, and an existing root shell.

This handset has no `by-name/xloader` link. Xloader is in the eMMC boot area;
the matching vendor [partition definitions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/include/linux/hisi/partition.h)
also place it in `EMMC_BOOT_MAJOR_PART`. Both boot areas were captured without
changing their `force_ro=1` state. Reads of VRL, its backup and fastboot were
also captured. Every artifact's size and SHA-256 matched a fresh phone-side
read. The private directory is `out/anne-device-private/xloader-validation-v1/`.

| Artifact | Bytes | SHA-256 |
| --- | --- | --- |
| eMMC boot0 | 4,194,304 | `72b7d1ad7407bb2f56389b1bb7c2a5611a83474c454b1e20970197f7bc00eed1` |
| eMMC boot1, all zero | 4,194,304 | `bb9f8df61474d25e71fa00722318cd387396ca1736605e1248821cc0de3d3af8` |
| VRL and identical VRL backup | 262,144 each | `ce5d2889ae815309ad5d37102073ee64b2f0a56282dfdb938274cba244f9696e` |
| Current fastboot, unchanged from the earlier dump | 4,194,304 | `16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52` |

<a id="xloader_validation-fastboots-authenticated-component"></a>
### Fastboot's authenticated component

The fastboot record is in the VRL slot at `0x6000`. The component descriptor
after its content certificate records flash offset `0x200000`, consistent
with the fastboot partition's location, and a length of `0xa4950` words.
Xloader's length extraction multiplies the word count by four.

That is **`0x292540` bytes (2,696,512)** from the start of the raw fastboot
partition image. The stored 32-byte digest at VRL offset `0x674c` matches
that exact prefix. Bytes beyond this prefix are not established as
authenticated by this particular component record.

| Input | SHA-256 of the authenticated component |
| --- | --- |
| Stored digest and original fastboot | `c5fd7fe49b45e5fd8fa254f1ca1295824ee00a6db765ef314334de823796a39d` |
| EL2h candidate | `365c9589afa5fd8304be4bfc9f5e5dec17ef3129859393c704790eda45281119` |

The changed instruction at raw fastboot offset `0x10b634` lies inside this
authenticated component. Moving a payload into padding alone would not make
the signed bootloader call it; no such entry mechanism has been demonstrated.

<a id="xloader_validation-independent-certificate-checks"></a>
### Independent certificate checks

The verification script uses Python's integer modular exponentiation and
`hashlib`; it needs no external cryptographic package. For this captured
CryptoCell format, the modulus is stored big-endian and the RSA signature
little-endian. The signatures validate as RSA-2048/PSS with exponent 65537,
SHA-256, and a 32-byte salt.

All three certificate signatures in the fastboot slot independently validate:

| Certificate | Signed-data start in VRL | Signature start in VRL |
| --- | --- | --- |
| Root key | `0x6000` | `0x61cc` |
| Intermediate key | `0x62cc` | `0x6498` |
| Fastboot content | `0x6598` | `0x6774` |

The root-to-intermediate and intermediate-to-content key hash links also
match. Each link hashes the next certificate's 256-byte public modulus plus
its 20-byte CryptoCell auxiliary key data.

The content certificate's signed bytes include the image digest. In an
offline copy, replacing that digest with the patched image's digest causes
the original content signature to fail. Truncated signatures are also rejected.
This is a negative authentication test, not a write to either phone VRL copy.

The root key's modulus-plus-auxiliary-data SHA-256 is
`37937554356c88f8d2a25dadbd1d46137a11af131b26f669bff19b6281f339a1`.
Its identity is pinned to the captured stock artifacts. The hardware's fused
root was not read, so this does not independently measure the OTP trust anchor.

Reproduce from the repository root:

```sh
python3 out/anne-device-private/xloader-validation-v1/verify-authentication.py
```

Evidence files: `manifest.json`, `authentication-proof.json`, the private
`xloader-disassembly.txt`, and `factory-loader-authentication.json` in the
same directory. No proprietary firmware is added to Git.

<a id="xloader_validation-what-xloaders-code-adds"></a>
### What xloader's code adds

Xloader is readable Thumb code in boot0 after its 4096-byte certificate header.
The analysis ELF assigns an address base solely to make disassembly readable;
the following offsets are **raw boot0 image offsets**:

- The normal fastboot loading routine at `0x122c` references the `fastboot`
  name and VRL slot `0x6000`. The Hi6250 chip branch skips some direct reads,
  relying on the platform's preloaded buffers; the generic read sequence alone
  is not the whole Hi6250 boot path.
- At `0x12d4`, it invokes the verification wrapper at `0x7248`, passing a
  lifecycle-derived mode. A nonzero verification return takes its failure
  branch. The upstream loading routine can retry the backup VRL.
- The wrapper's modes 0 and 1 call the certificate/component verification
  path at `0x70fc`. Mode 2 returns success without that call. The mode producer
  at `0x6ffc` depends on hardware lifecycle/security information and a bit read
  through the crypto/OTP access helpers. It does not consume fastboot's
  USERLOCK or FBLOCK variables.

The existence of a mode-2 branch does not establish that this production
handset can select it through a supported setting. The existing boot logs
report a secure product. No OTP write, lifecycle change, unknown SMC, or
attempt to trigger a verification failure was performed.

<a id="xloader_validation-single-byte-flag-changes-fresh-verification-17-october-2026"></a>
### Single-byte flag changes: fresh verification, 17 October 2026

Xloader in eMMC boot0, both VRL copies, and fastboot were read again from
the connected phone in TWRP. Each host artifact matches the phone-side
SHA-256 before and after transfer. The hashes match the artifacts documented
above. Boot0 remained `force_ro=1`; this verification made no device writes.

Fresh captures, a standalone verifier, results, and newly generated Thumb
disassembly are in
`out/anne-device-private/fastboot-write-verification-20261017/`.

The fresh VRL confirms that raw fastboot offsets **`0x000000` through
`0x29253f`, inclusive**, are covered by the content certificate's image hash.
The original image hash, all three RSA-2048/PSS/SHA-256 signatures, and both
certificate key-hash links validate. The xloader payload's certificate chain
and content hash validate separately, with the same recorded root key material.

The offline verifier changes exactly one bit in one byte, independently at
each of three offsets:

| Changed offset | Resource | Original image digest still matches | Replacing digest preserves content signature |
| --- | --- | --- | --- |
| `0x1702b0` | Unlocked-warning text | No | No |
| `0x236930` | Unlocked-warning JPEG | No | No |
| `0x292110` | Pointer to the unlocked-warning JPEG | No | No |

An outside-range control at `0x300000` leaves this component's digest and
signature intact. This establishes only the bounds of this particular signed
component; it does not establish that the control byte is a useful flag or
that modifying other components is accepted.

The current xloader bytes independently confirm these raw boot0 offsets:

- `0x12d4` calls verification wrapper `0x7248` before fastboot is executed.
  The result check at `0x12da` sends a nonzero result to the load-failure path.
- `0x7272` calls certificate/component verifier `0x70fc` for verification
  modes zero and one. Mode two skips that call.
- Mode producer `0x6ffc` gets security/lifecycle information through
  `0x798c`, then conditionally reads a hardware word through `0x7a80`.
  This path does not read fastboot's ordinary USERLOCK/FBLOCK state or an
  OEMINFO field to decide whether verification is skipped.

Consequently, a deliberate flag change inside the covered prefix fails the
existing authentication in the enforcing path even if the edited image is
otherwise structurally valid. An ordinary bootloader-unlocked flag does not
select the hardware verification-skip branch shown here.

At this stage the hardware lifecycle/OTP mode had not been measured. A fresh
TWRP dmesg query yielded no matching security-mode boot-log lines. The lifecycle
follow-up below subsequently identified the bootloader-reported mode through
the current boot arguments. No modified firmware
was flashed or booted, so these results do not claim that permanent failure or
a specific emergency recovery outcome was demonstrated on the handset.

Reproduce the cryptographic verification without touching the phone:

```sh
python3 out/anne-device-private/fastboot-write-verification-20261017/verify.py
```

`verification.json` records the original signatures and each single-byte test;
`xloader-verification.dis.txt` records the newly disassembled verification and
mode-selection paths.

<a id="xloader_validation-lifecycle-follow-up-current-boot-reports-enforcing-mode-0"></a>
### Lifecycle follow-up: current boot reports enforcing mode 0

The current TWRP boot reports **`efuse_status=0`** in both `/proc/cmdline` and
the device tree's `chosen/bootargs`, with exactly one occurrence in each.
The same arguments report `userlock=unlocked`, `bootlock=unlocked`, and
`androidboot.verifiedbootstate=ORANGE`. These are different state variables:
normal bootloader unlock and Android's orange verified-boot state coexist
with the earlier bootloader-authentication mode.

Tracing the fresh fastboot image establishes the meaning of that argument:

1. Formatter `0x42bf4` looks up the `secboot` module and invokes operation
   `+8` at `0x42c1c`, passing an output-word address.
2. The descriptor at `0x28ff88` has operations at `0x28ffa8`; its `+8` entry
   is `0x12752c`, a branch to security-configuration selector `0x126114`.
3. After a successful return, the formatter loads that output word at
   `0x42c34` and formats it with `efuse_status=%d` at `0x42c44`.
4. Selector `0x126114` obtains lifecycle state through `0x80160`. Its signed
   byte jump table at `0x13d7b0` directs **only lifecycle values 5 and 7** to
   `0x1261ac`, which stores output mode zero at `0x1261e0`.

This is evidence for the **current bootloader-reported mode**, rather than a
direct read or attestation of the hardware lifecycle register. It establishes
the candidate set `{5, 7}` under the captured firmware's selector; it does not
distinguish which of those lifecycle values the hardware returned.

<a id="xloader_validation-what-xloader-actually-reads"></a>
#### What xloader actually reads

Xloader's lifecycle path is `0x6ffc` → `0x798c` → `0x89a4`.
The final helper polls controller offset `+0xabc`, then reads the low byte of
the lifecycle/status register at **controller offset `+0xad4`**.
The xloader controller base is `0xbf011000` in its execution address space.
The corresponding fastboot getter uses base `0xff011000`, including the
lifecycle helper at `0x7d60c`. These addresses are from the respective firmware
code; no live raw-MMIO read was used to establish the reported mode.

For lifecycle values 0, 1, or 3, xloader additionally calls `0x7a80` with OTP
word index **`0x24`**. The helper at `0x8f4c` submits a read command through
offset `+0xaa4` and retrieves the word through `+0xaac`. The selector tests
**bit 28**, mask `0x10000000`, of that word.

| Hardware lifecycle value | OTP word `0x24`, bit 28 | Selected mode | Wrapper behavior |
| --- | --- | --- | --- |
| `5` or `7` | Not consulted by mode selector | `0` | Calls verifier `0x70fc` |
| `0`, `1`, or `3` | Set | `1` | Calls verifier `0x70fc` |
| `0`, `1`, or `3` | Clear | `2` | Skips verifier in wrapper `0x7248` |
| Other accepted input, or lifecycle-read/validation error | — | `3` / error | Loading aborts or wrapper rejects |

The lifecycle getter performs additional hardware-status checks. For value 5
it checks status bit 8 and validates part of OTP word `0x0a`; for values 1 and
5 it checks status bit 9. Those failures do not enter the verification-skip
path. Polling and validation occur before the selector returns its mode.

Arm's published lifecycle terminology maps `0` to Chip Manufacture, `1` to
Device Manufacture, `5` to Secure Enable, and `7` to Return to Manufacturer.
See the [Arm CryptoCell lifecycle indication table](https://documentation-service.arm.com/static/5e85992bc10d1d3c5b7052d7),
appendix A.7.1. This is naming context from Arm's published CryptoCell
documentation; the numeric behavior above comes from this handset's code.
The older value `3` is accepted by this firmware, but its exact lifecycle
name has not been established here.

<a id="xloader_validation-offline-execution-and-live-evidence"></a>
#### Offline execution and live evidence

`lifecycle/emulate.py` executes the captured Thumb lifecycle getter, hardware
validation, OTP-read helper, and selector with synthetic MMIO. All 18
combinations of lifecycle values 0–8 and the two OTP-bit settings match the
table. Three additional hardware-validation error cases select error mode.
Eight verifier-wrapper runs cover modes 0–3 with a synthetic successful or
failing verifier; modes 0/1 call it and propagate failure, mode 2 skips it,
and mode 3 fails. Logging and clock calls are stubbed; no hardware is exercised.

`lifecycle/emulate_fastboot.py` independently executes fastboot's actual
selector with synthetic lifecycle/OTP helpers. Its 18 cases agree with the
xloader mode matrix. In particular, only input lifecycle 5 or 7 produces the
zero seen in the live boot arguments.

TWRP exposes `/dev/efuse`, but inspection of the pinned Huawei driver/header
found chip-ID, authentication-key, die-ID, and debug-mode operations rather
than a direct lifecycle-register ioctl. Debug-mode readout is not the same
measurement as lifecycle state. No unknown ioctl or fuse-programming
operation was issued. The mode finding comes from the existing boot arguments
and their independently traced producer.

**Conclusion:** the current boot reports the enforcing mode, not mode 2.
For the lifecycle candidates consistent with that report, clearing the tested
OTP bit would not select mode 2: lifecycle 5/7 takes mode zero without
consulting that bit. Nothing in this path makes the ordinary bootloader-unlock
flag a lifecycle-mode switch.

Evidence is in
`out/anne-device-private/fastboot-write-verification-20261017/lifecycle/`:
`live-mode.json`, `emulation.json`, `fastboot-mode-emulation.json`,
`fastboot-live-mode.dis.txt`, and the two emulator scripts. The downloaded
reference driver/header and their source revision and hashes are retained
beside them in `kernel-source.json`.

<a id="xloader_validation-factory-loader-lead-and-its-limits"></a>
### Factory-loader lead and its limits

The ANE-compatible factory images from PotatoNV's
[HiSiBootloaders repository at commit 4755428](https://github.com/kitsuned/HiSiBootloaders/tree/4755428/hisi65x_a)
were downloaded for offline analysis. Their certificate signatures, key hash
links and payload hashes all validate. Both use the same root public-key
material as the captured stock xloader and fastboot VRL record.

| Factory image | SHA-256 | Authenticated payload bytes |
| --- | --- | --- |
| xloader | `07c2ec41717b532b5f8d27968111539901854afacb0c0f35ac8b85dc08be0e81` | 66,816 |
| fastboot | `c019e3e9acebe2a7e835d83efd170f8867973c5f7dba808958c2e02e1054a145` | 3,371,008 |

This makes them a concrete signed-loader research lead, consistent with
[PotatoNV's documented ANE support and RAM-loading method](https://github.com/kitsuned/PotatoNV).
It does not prove this phone accepts every factory certificate's lifecycle,
rollback and platform parameters in the current boot mode.

The factory fastboot also requests EL1h: the corresponding instruction is at
payload offset `0xf9f34` (file offset `0xfaf34`, including its header).
Its authenticated payload covers that instruction, so simply applying the
EL2 immediate change to the signed factory image has the same signature
problem. Static inspection of its `oem boot` handler shows module-based boot
operations, not a demonstrated arbitrary-address execution interface. Commands
named `rw` and `wr` operate through storage/partition modules in the examined
handlers; their names are not proof of unrestricted memory access.

The next concrete research target is what the **unchanged, authenticated
factory loader can do after authentication**, or an exact-device earlier-stage
verification defect. A loader must demonstrate a usable EL2 handoff before
persistent replacement is considered. No working EL2/VM route is claimed.
The phone remains in TWRP with its verified original kernel.

<a id="unicorn"></a>
## Offline fastboot analysis

<a id="unicorn-offline-fastboot-analysis"></a>

Install the optional [Unicorn Engine](https://www.unicorn-engine.org/docs/tutorial.html)
and Capstone dependencies into the project's virtual environment:

```sh
.venv/bin/python -m pip install -r scripts/anne/requirements-unicorn.txt
```

`emulate-fastboot.py` loads a **raw little-endian AArch64 image**. Its default is
the saved stock ANE-LX1 dump at
`out/anne-device-private/el2-firmware-inspection/fastboot.img`, SHA-256
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
The default base is zero, matching the absolute pointers and offsets in this
dump and the existing analysis ELF. Other images need explicit mapping and
entry choices; `--base` maps bytes without relocating embedded pointers.
An ELF or a factory-loader container must first be converted to its raw payload.

Run the existing command-permission function with synthetic FBLOCK=0,
USERLOCK=1, FRP=1, reproducing the inputs discussed in
[FASTBOOT_EL2.md](boot.md#fastboot_el2):

```sh
.venv/bin/python scripts/anne/emulate-fastboot.py \
  --profile permission --command flash:kernel --trace
.venv/bin/python scripts/anne/emulate-fastboot.py \
  --profile permission --command flash:fastboot --trace
```

This preset checks the exact stock hash, starts at `0x2abb8`, initializes only
the three lock fields via the pointer at `0x292000`, and supplies a command
string in synthetic RAM. It executes the image's own comparison functions.
On return, x0=1 permits dispatch and x0=0 rejects it. Other commands can reach
additional dependencies and stop. Change `--fblock`, `--userlock`, and `--frp`
to compare input states. These inputs do not measure current device state.

For a different function, provide the entry, initial registers, extra RAM,
and optional in-memory data. For example, observe the known SPSR instruction:

```sh
.venv/bin/python scripts/anne/emulate-fastboot.py \
  --entry 0x10b634 --until 0x10b638 --trace
```

The resulting x1 is `0x3c5`. General setup options are:

```text
--image PATH                 raw AArch64 input image
--entry 0xADDRESS            start execution at a function or instruction
--until 0xADDRESS            stop before this address
--reg x0=0xVALUE             initialize x0..x30, sp, or nzcv (repeatable)
--map 0xADDRESS:0xSIZE       explicitly map zero-filled RAM (repeatable)
--write 0xADDRESS:HEXBYTES   initialize mapped memory (repeatable)
--dump 0xADDRESS:0xSIZE      save memory after execution (repeatable)
--count 100000              instruction limit; maximum 10000000
--timeout-ms 2000           wall time limit; maximum 60000
--trace                     stream observed instructions to trace.jsonl
--output NEW_DIRECTORY      output directory; existing directories rejected
```

Every run saves `report.json` with the image hash, dependency versions, initial
setup assumptions, memory maps, stop reason, registers, fault information, and
last 32 instructions. A temporary private directory is used unless `--output`
is supplied. Put repository-local results under ignored `out/`; reports and
traces can contain firmware code or data. `--write` changes emulator memory
only. The input file is read without modification.

The stack is synthetic RAM at `0x7ff00000..0x7ff0ffff`, the argument page is at
`0x70000000`, and x30 initially targets the return sentinel `0x7ffe0000`.
FP/SIMD is enabled for library routines. Extra RAM is readable/writable;
the captured image is readable/writable/executable to support its globals.
Unmapped/protected memory accesses, emulator errors, and monitor/trap/wait
instructions stop execution. No MMIO responses or SMC results are invented.
A successful function return or `--until` exits 0; faults, traps, or limits
exit 1 with a report. Setup errors exit 2.

This is a function-analysis harness. Starting at offset zero will encounter
boot-stage CPU and hardware dependencies. It does not model a Kirin SoC,
authenticate the boot chain, or demonstrate a full boot or an EL2 handoff.

Validated on 17 October 2026 using the default stock image: `flash:kernel`
returned x0=1 after 1,995 observed instructions and `flash:fastboot` returned
x0=0 after 5,401, with the default synthetic lock inputs. The instruction at
`0x10b634` produced `0x3c5`; executing through `0x10b640` with an explicit
parameter-block mapping stored `c5 03 00 00` at `0x35e80040`. Entry-point
execution stopped at `0xb8` (`msr CNTVOFF_EL2, x1`) after 44 observed
instructions with an emulator exception. These results are saved locally
under `out/anne-device-private/unicorn-*`. The source image hash was preserved.

On Apple Silicon, Unicorn 2.1.4 queries `hw.cachelinesize`. In the Codex
sandbox on this host that query is blocked; Unicorn's fallback reads CTR_EL0
and crashes with SIGILL. The harness checks the query first and reports a
setup error. Run through approved execution outside the sandbox or in a
normal terminal. The native library and firmware do not need patching.

Run synthetic AArch64 regression tests without any private firmware:

```sh
.venv/bin/python -m unittest discover -s tests -p test_anne_unicorn.py -v
```
