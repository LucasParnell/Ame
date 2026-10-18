# Fastboot eMMC inputs before EL1 — 18 October 2026

Scope: parsers and loads executed by the captured fastboot image before its
normal BL31/kernel handoff. The image SHA-256 is
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
All addresses are raw image offsets, not asserted runtime addresses.
No device commands or writes were performed for this analysis.

## Exception-level boundary

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

## Identified reads and load paths

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

## Ordering relative to kernel loading

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
