# ANE-LX1 EL2 path audit

14 October 2026; ANE-LX1 8.0.0.151(C782), Kirin 659/Hi6250.
This audit uses saved phone captures, the dumped fastboot image, matching Linux
source, and the primary references below. No phone command, reboot, or flash
was performed during this audit. It is not proof that every firmware interface
or vulnerability has been exhausted.

## Finding

There is no demonstrated way to install Dango's monitor from the phone's
current EL1 kernel entry. There **are other unresolved paths** besides the RPMB
callback research: the board-software USB download loader, modifications to
the boot chain, and the bootloader's HHEE/BL31 handoff machinery. None has yet
produced a measured EL2 entry on this handset.

The CPU supports EL2. The saved native diagnostic reports EL2 feature field 2,
EL1 entry on all eight CPUs, and no installed Dango monitor. Recovery Linux
independently reports that all CPUs started at EL1. An empty EL2 or an absent
HHEE image does not let EL1 install a replacement hypervisor.

## Paths and limits

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

## Exact fastboot evidence

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

## Correction to the existing canary evidence

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
does not retroactively validate v5/v6. See [RPMB_AUDIT.md](RPMB_AUDIT.md) for
source provenance, exact request offsets, and the remaining EXT_CSD assumptions.

The earlier truncated serial capture also does not demonstrate a secure-world
fault: a subsequent full capture returned 0xff01 and shell status 1 normally.
USB enumeration delays alone do not establish a panic or failed boot.

## Next investigation

The user has excluded a testpoint-dependent system and bootloader replacement,
and requested that the RPMB canary be left alone. Investigation therefore
continues through the existing stock firmware, with no new phone probes in
this audit. A working route must demonstrate both primary and secondary EL2 entry,
monitor installation, and actual guest execution; finding a loader or a
privileged branch is insufficient.

## Follow-up: resolved normal handoff

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

## Primary references

- [Linux AArch64 boot requirements](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html): firmware entry level, SCR_EL3.HCE and CPU consistency.
- [TF-A PSCI reference implementation](https://github.com/ARM-software/arm-trusted-firmware/blob/master/lib/psci/psci_common.c): psci_get_ns_ep_info selects EL1/EL2 using SCR_EL3.HCE, not CPU_ON's context ID.
- [PotatoNV author documentation](https://github.com/kitsuned/PotatoNV): ANE support, testpoint/download mode, board-software loader, and distinction between USERLOCK and FBLOCK.
- [PotatoNV loader repository](https://github.com/kitsuned/HiSiBootloaders): hisi65x_a and hisi65x_b artifacts.
- [Impalabs' HHEE research](https://blog.impalabs.com/2212_huawei-security-hypervisor.html): vendor hypervisor purpose and boot flow; its analyzed binaries are not this ANE firmware.
- Matching Hi6250 Linux revision used locally: `65e545cbaa11d132e243287b150146bc2a51c45e` in `hisi-oss/android_kernel_huawei_hi6250-8`. Reviewed local files: `/tmp/hi6250-src/emmc-rpmb.c`, `emmc-rpmb.h`, `hisi_rpmb.c`, `bootdevice.h`, `share.dtsi`, and `global_ddr_map.h`.
