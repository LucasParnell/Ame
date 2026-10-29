# Huawei Anne GPU isolation research

This consolidated routing/firmware audit is evidence for retaining host-owned Mali shadow translation. No verified external GPU SMMU route is enabled. Later hardware probes and negative observations do not authorize dropping mapping validation. See [architecture](../../docs/architecture.md#memory-and-dma-ownership) for the current mechanism.

- [Anne GPU IOMMU routing audit](#anne_gpu_iommu_audit)

<a id="anne_gpu_iommu_audit"></a>
## Anne GPU IOMMU routing audit

<a id="anne_gpu_iommu_audit-anne-gpu-iommu-routing-audit"></a>

22 October 2026. Requested change: replace software reconstruction of Mali
translations with a host-controlled external SMMU stage for the Linux appliance.

**Result: no supported GPU-to-external-SMMU route was established. An external
GPU SMMU backend has not been implemented or enabled.** The existing hardware
Mali MMU, using validated host-owned shadow tables, remains active. Finding
other SMMUs on the SoC does not establish that GPU transactions pass through them.

<a id="anne_gpu_iommu_audit-sources-and-findings"></a>
### Sources and findings

Vendor kernel: `hisi-oss/android_kernel_huawei_hi6250-8`, commit
`65e545cbaa11d132e243287b150146bc2a51c45e`. The repository tree is complete
(`truncated: false`). Audited all 357 `.dts`/`.dtsi` files under
`arch/arm64/boot/dts/auto-generate/config/hi6250/`, plus the GPU platform code,
Mali MMU implementation, Hisilicon IOMMU driver and SoC address definitions.
Downloaded source/evidence stays in ignored `out/anne-gpu-research/`.

- [GPU node](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/arch/arm64/boot/dts/auto-generate/config/hi6250/hisi_6250_gpu.dtsi):
  Mali registers at `0xe8970000`, JOB/MMU/GPU interrupts and clock/power
  resources. No external IOMMU reference or GPU stream identifier is supplied.
- [Mali AArch64 MMU](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/gpu/arm/r20p0-01rel0/mali_kbase_mmu_mode_aarch64.c):
  programs a physical page-table root and AArch64 4 KiB mode. The hardware
  backend writes the Mali AS translation registers. This establishes the
  internal GPU MMU, not an external stage that can translate guest physical
  addresses into host physical addresses.
- [Shared Hisilicon IOMMU node](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/arch/arm64/boot/dts/auto-generate/config/hi6250/hisi_6250_memory.dtsi):
  `phy_pgd_base = 0x34a78000` is a reserved page-table allocation, not SMMU MMIO.
  [Its driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/iommu/hisi/hisi_smmu.c)
  allocates/manages shared mappings; `hisi_attach_dev` returns the shared table
  and IOVA geometry. It does not establish a GPU stream route or program a
  GPU-specific second-stage context.
- [IVP SMMUv2](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/arch/arm64/boot/dts/auto-generate/config/hi6250/hisi_6250_ivp.dtsi):
  at `0xe8dc0000`, described with the image/vision processor. This is not proof
  of a GPU route and cannot be treated as a system-wide GPU SMMU.
- Camera definitions also describe an ISP SMMU at `0xe8406000`; display has
  its own SMMU registers. Neither establishes GPU containment.
- Ame currently implements an ARM SMMUv3 backend. It is not register-compatible
  with the IVP's stated SMMUv2 controller, and inventing a DT node would not
  establish a physical transaction route.

<a id="anne_gpu_iommu_audit-consequence"></a>
### Consequence

Keep the Mali shadow MMU and its full ownership/permission validation. Removing
that validation or forwarding guest physical PTEs directly would not implement
nested translation: it could produce incorrect DMA addresses or expose host
memory. The presence of a hardware GPU MMU already accelerates GPU VA-to-host
PA translation; software currently constructs and validates its tables.

Enabling an external stage requires additional hardware evidence: a GPU AXI
route through a specific SMMU, its GPU stream/master identity, supported
translation stages and the controlling register/context-bank interface. The
sources audited here do not supply that combination. This is a limitation of
available hardware evidence, not a claim that missing DT properties alone
prove the silicon cannot support it. No experimental MMIO writes, replacement
kernel or phone flash were needed for this audit.

<a id="anne_gpu_iommu_audit-follow-up-interconnect-registers-and-firmware-evidence"></a>
### Follow-up: interconnect registers and firmware evidence

Checked beyond DT bindings on 22 October 2026: Hi6250 PCTRL, PMCTRL, CRG,
DMSS, ACPU/IOMCU/LPMCU address maps, NoC initiator/target decode tables,
DDR-protection driver, and strings/symbol listings in the phone's private
stock LK and decrypted TEE images. Firmware string/symbol absence is weak
negative evidence, not proof that an undocumented block is absent. Embedded
LK symbol offsets did not reliably identify the requested routines in the
uploaded image. Subsequent instruction/string cross-references identified the
actual GPU power and display SMMU routines; see the firmware follow-up below.

Concrete additional findings:

- [PCTRL definitions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/ap/platform/hi6250/soc_pctrl_interface.h)
  expose GPU read/write master-ID fields: `PERI_CTRL8` at offset `0xc0c`,
  `g3d_armid0`, `g3d_awmid0`, `g3d_armid1`, `g3d_awmid1` (five bits each).
  These establish configurable bus master identities, not SMMU StreamIDs.
- PCTRL `PERI_CTRL25` at `0x68`, bit 11, is named
  `smmu_integ_sec_override`. Adjacent fields include GPU AXI clock gates, but
  adjacency in one peripheral register does not establish that the SMMU sees
  GPU traffic. No GPU translation-context selection is described there.
- [DMSS definitions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/ap/platform/hi6250/soc_dmss_interface.h)
  define per-interface security regions: `ASI_SEC_RGN_MAP` (base, size, enable),
  `ASI_SEC_RGN_ATTRIB` (master/security matching), and separate
  `ASI_SEC_MID_RD`/`ASI_SEC_MID_WR` bitmaps. This describes address-region
  access protection, not a page-table translation stage.
- The [DDR-protection driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/ddrc/hisi_ddr_secprotect.c)
  receives protection-fault information through BL31 shared memory/FIQ and
  refers to BL31 logs. Therefore secure-firmware ownership/access must be
  established before treating these controls as available to Ame.
- NoC tables describe config, video-codec and display/ISP flows; they do not
  identify a GPU SMMU context. Stock LK strings identify display SMMU setup and
  DDR security setup, but no GPU SMMU setup was identified.
- The optional IVP SMMU definition is not included by another file in the
  audited config directory. Its presence in that source tree is not itself
  proof that the corresponding block is active on this phone.

<a id="anne_gpu_iommu_audit-potential-mpu-alternative-unimplemented-hypothesis"></a>
#### Potential MPU alternative (unimplemented hypothesis)

The GPU master-ID fields plus DMSS region controls are a plausible lead for
hardware DMA *protection*. They do not establish a usable external GPU IOMMU.
In principle, physical-address-preserving appliance RAM plus verified hardware
region protection could avoid guest-to-host PTE rewriting, while Mali retains
its GPU VA translation. That would require a different memory layout and DMA
backend; it cannot be enabled by switching off today's shadow validation.

Before implementation, establish the actual GPU-to-DMSS interface/master
mapping, region priority/granularity, coverage of every GPU read/write port,
secure-firmware register access, reset behavior, and denied-access fault
containment. Then test allowed/denied accesses using dedicated disposable
buffers. No such protection test has yet been performed. This follow-up made
no MMIO writes and did not replace or reboot the working phone image.

<a id="anne_gpu_iommu_audit-latency-follow-up"></a>
### Latency follow-up

The renderer still uses the Mali hardware MMU with host-owned shadow tables;
no external SMMU routing or nested-translation backend has been enabled. The
shadow builder now validates and translates each guest page-table page once
per rebuild, then loads its bounded entries atomically through that mapping.
It retains leaf-by-leaf backing/permission checks, shared-buffer pinning, the
retired-table pools, and hardware flush/root-update barriers. Guest page-table
addresses never become hardware roots. Host regressions exercise changed PTEs,
revoked table access and denied leaf mappings on this path.

The GPU already writes the imported shared render target directly. The native
CPU copy into inherited panel scanout remains: removing it requires explicitly
owned scanout buffers and a validated display-controller flip/fence path. This
change does not claim direct panel scanout or measured hardware speedup.

<a id="anne_gpu_iommu_audit-on-device-routing-attempt-22-october-2026"></a>
### On-device routing attempt, 22 October 2026

An actual read-only hardware attempt was made after closing the detached PC
serial console. This did **not** enable an external IOMMU. The new
`diag gpu-iommu` command captures a fixed list of documented registers with
32-bit reads under the kernel address space; it accepts no arbitrary addresses
and writes no routing or SMMU control registers.

| Register | On-device result |
| --- | --- |
| GPU_ID | `0x08301000` |
| GPU_MEM_FEATURES | `0x1` |
| GPU_MMU_FEATURES | `0x2821` (33-bit GPU input, 40-bit output) |
| GPU_AS_PRESENT | `0xff` |
| PCTRL PERI_CTRL8, GPU bus master IDs | Read fault; no value available |
| PCTRL PERI_CTRL25 | `0x0760e800`; `smmu_integ_sec_override=1` |

Snapshot validity mask was `0x2f` with one caught read fault. The fault does
not by itself establish secure-firmware denial, and the security-override bit
is not evidence of a GPU stream route or a second translation stage. Neither
identifies a GPU SMMU base, StreamID/context, or nested-translation setup. No
external-stage passthrough or protection bypass has been enabled.

The initial generic `el3-rprobe` attempt exposed a separate recovery bug: the
vector entry overwrote x4 with the vector index and used x9/x10 as scratch
before preserving them. When a probe load faulted, the corrupted x4 was later
used as an output pointer, producing a secondary write fault at address 4 and
a reset. The existing kernel was recovered via verified RAM LK/TEE boot without
rewriting its partition, and the retained console was saved before reboot.
The vector paths now save these registers before diagnostic scratch use and
restore them before returning. A standalone QEMU test executes actual page
faults through both production early and runtime vector paths and checks live
registers; the original vectors fail that regression and the fixed ones pass.

A verified probe image was then installed:
`out/anne-package-20261022T182834Z`, SHA-256
`e453ee24e553f6158ab74d3df3f5e9640a0e95f5485fde434e2ae5f5f1bd2c26`.
Full partition readback matched. On this image, `diag el3-rprobe 0` reports one
caught translation fault and returns to msh; `diag gpu-iommu` also returns to
the shell despite the master-ID register fault.

Private hardware evidence: `out/anne-device-private/iommu-probe-20261022T182153Z/`
contains the retained original fault, recovery record, register snapshot,
recovery test, and post-probe GPU run. Installation evidence is in
`kernel-install-20261022T183023Z`. No working configuration was claimed for an
external IOMMU based on these reads.

Additional architectural cross-check: Arm's [Mali GPU Virtualization Guide,
issue 20, page 108](https://documentation-service.arm.com/static/6807606e79c4c17ad8036586)
explicitly requires an external SMMU for guest-physical to machine-physical
translation; Mali's own MMU provides a single stage. That describes the required
architecture, not evidence that Hi6250 implements a routable external stage.

Validation: the focused vector recovery, shell diagnostics, framebuffer tests,
and two deliberate early-fault full-kernel QEMU tests pass. Two broader QEMU
boot tests stop at `kernel/loader: stack allocation failed`, before the new
snapshot runs; they are not counted as passes. The actual phone boots and the
hardware snapshot completed without another reset. The subsequent GPU command
produced no captured serial output, so it is not a verified post-probe render.

<a id="anne_gpu_iommu_audit-decrypted-secure-firmware-follow-up"></a>
### Decrypted secure firmware follow-up

The user's read-only USB drive contains both encrypted trustfirmware and an
already decrypted BL31 under `p20re/payloads/anne-trustfirmware/capture/`.
The plaintext's SHA-256 matches the digest pinned in that project's decryption
script: `27c06575c5af056fdbedc9385d4bd3c460459f5075ee79dcb65762f71fa5eb85`.
It is 113408 bytes, loaded at `0x35e00000`. Analysis used this verified
plaintext; no fuse or key material was needed or copied into the repository.

Reproduce the non-secret evidence extraction with:

```sh
python3 scripts/anne/audit-bl31-gpu.py /private/path/trustfirmware.decrypted.bin
```

The script rejects other firmware hashes. Function descriptions below are
manual instruction-level findings for this image, not automatic proofs of
hardware capability. Addresses in this section are **file offsets** unless
explicitly identified as physical addresses.

<a id="anne_gpu_iommu_audit-gpu-identities-and-ddr-security"></a>
#### GPU identities and DDR security

BL31's fault-decoder table starts at `0x1b100`, contains 61 records of 24
bytes, and is reached through the pointer at `0x1b870`. The lookup routine
at `0x4a44` compares the master ID at record offset 16, prints the name and
traffic strings, and returns the classification at record offset 20.

| Full master ID | BL31 name | Traffic | Record offset |
| --- | --- | --- | --- |
| `0x3b` | GPU0/GPU1 | normal data | `0x1b688` |
| `0x3c` | GPU0/GPU1 | write TileBuffer | `0x1b6a0` |

Both have fault classification 4. This classification is shared by CPUs, USB,
and other masters; **it is not ASI port 4**. Likewise these full fault IDs must
not be substituted directly for the five-bit PCTRL fields or an SMMU StreamID.
The actual GPU ASI route remains unidentified.

The interrupt handler at `0x4c04` reads physical DMSS global status at
`0xfffc6030`, then per-interface security status at `0xfffc0424 + 0x800*ASI`
and failure information at `0xfffc0480 + 0x800*ASI`. Thus the table is actual
DDR security fault decoding, rather than an unused GPU identification string.

The region-programming routine at `0xf44` writes the documented DMSS region
registers at physical `0xfffc0500 + 0x800*ASI + 0x10*region`: address/size,
access attributes and master read/write masks. The region-count array at
`0x13040` is `{14,14,14,16,16,15,16}` for its seven supported interfaces.
The size encoding handled here starts at 64 KiB and requires corresponding
base alignment. These are physical address protection regions, with no page
table root or translated output address programmed by this routine.

<a id="anne_gpu_iommu_audit-secure-register-callbacks"></a>
#### Secure register callbacks

SMC `0xc500aa01`, dispatched at `0xb768`, validates the cookie family
`0x55bbcce0 | index` and invokes an installed function pointer. It passes
original x1 as callback x0 and original x2 as callback w1. It is not a generic
secure register-read service. Registration at `0xb860` installs these slots:

| Index | Function offset | Reviewed operation |
| --- | --- | --- |
| 1 | `0x57c0` | DDR statistics enable/disable |
| 2 | `0x58e8` | Copy selected DDR statistics to a constrained shared buffer |
| 3 | `0x55f0` | Copy DDR controller status to caller buffer |
| 4 | `0x5634` | Write physical DDR registers `0xfffc9270/0xfffc9274` |
| 6 | `0x5570` | Issue a DDR controller command and poll completion |
| 7 | `0x5a10` | Clock control or constrained register read-modify-write |
| 8 | `0x1778` | Configure DDR protection for preapproved memory ranges |
| 10 | `0x19a0` | Clean up DDR protection |
| 14 | `0x1b2c` | Enable EVS secure DDR protection |
| 15 | `0x1d80` | Clean up EVS secure DDR protection |

Index 7/subcommand 2 checks a 12-byte shared request, requires a word-aligned
target, and bounds its physical target to `0xe8600000..0xe867fffc`. It performs
a bitfield read-modify-write, with no readback result. This cannot read the GPU
master register at `0xe8a09c0c`, and is not an external SMMU configuration API.
Index 0 is not installed by this registration routine; previous diagnostic
comments proposing an index-0 read oracle are not supported by this image.

Do not use `diag el3-aasweep` for routing discovery: the existing research
command invokes every callback with guessed arguments, including operations
that write memory/controller registers or change protection. It is not a
read-only scan. None of these callbacks was invoked during this firmware audit.

<a id="anne_gpu_iommu_audit-lk-cross-check-and-remaining-gap"></a>
#### LK cross-check and remaining gap

The actual LK GPU power-up routine at `0xa7ff8..0xa8140` manipulates CRG/PMCTRL
power and clock registers; it does not establish a GPU translation context.
Its display SMMU configuration at `0x5e0f0` obtains the display controller
base from a framebuffer structure and adds `0x8000`; it is a display-specific
block, not evidence of GPU routing.

This follow-up establishes GPU fault identities and an available secure DDR
protection API. It still supplies no GPU external SMMU base, stream route or
second-stage context. DDR protection alone cannot translate the appliance's
fragmented guest backing. A possible identity-addressed RAM/MPU backend still
needs verified ASI coverage, region precedence and denied-access containment;
the secure API also constrains changes to bootloader-approved memory ranges.
The existing shadow backend remains necessary with the current VM layout.

<a id="anne_gpu_iommu_audit-mapped-dmss-hardware-reads"></a>
### Mapped DMSS hardware reads

After the USB console was released, a generic `el3-rprobe fffc6000` returned
`ESR=0x96000007`: a level-3 **translation fault**. This command runs under the
caller's address space, so that result did not establish hardware denial.

Added `diag gpu-dmss` (fixed diagnostic index 32), with an explicit kernel-only
Device mapping of physical DMSS `0xfffc0000..0xfffc6fff`. The mapping is installed
once under the EL3 diagnostic lock; the command excludes local IRQs while using
the kernel TTBR and restores the caller's TTBR before copying the result out.
PCTRL's existing bootstrap mapping is reused. All accesses are fault-recoverable
32-bit loads. No caller-supplied addresses, routing writes, protection writes,
counter enable/reset or interrupt acknowledgement are performed.

The snapshot reads DMSS global RTL version/info, PCTRL GPU master configuration,
and eight documented words for each of BL31's seven supported security
interfaces: RTL region count, statistics mask/cycle/filter0/filter1, write/read
flux and security interrupt status. It reports validity per word and the first
fault's syndrome/address, so an inaccessible register is never reported as a
successfully read zero.

Installed and fully readback-verified `out/anne-package-20261022T190220Z`, SHA-256
`5c609fb0fd469d5c380243f2345b20f3ee5114d69818781fbd6f6e073df4958d`.
Two consecutive snapshots returned:

- Global validity `0`, all seven per-interface validity masks `0`.
- 59 recovered faults per snapshot.
- First `ESR=0x96000210`, `FAR=0xfffc6000`. DFSC `0x10` is a synchronous
  external abort, not a translation-table fault.
- Both commands returned to msh without a reset. This also exercised the
  mapping's reuse on the second invocation.

Thus the corrected, mapped EL1 access path still cannot read these registers.
The syndrome does not identify which interconnect/security control rejected
the transactions and does not prove the presence or absence of an external
GPU SMMU. The reviewed BL31 callbacks do not expose arbitrary reads of this
aperture or a GPU translation setup. A secure-side observation mechanism and
evidence of the actual GPU translation route are still missing.

Post-probe `diag gpu` succeeded: 298 frames in the five-second 3D rotation,
average submission 3732 us, maximum submission 47718 us, average rendering
2292 us, checksum `6a66069c`; display restoration completed and msh returned.
These are one run's observations, not a controlled speedup measurement.
The existing Mali shadow backend remains enabled; no external IOMMU was enabled.

The shell regression and full native build pass. The first diagnostic package
stopped before register reads because it redundantly mapped PCTRL; that mapper
rejects existing PTEs. The installed package corrects this and has the repeated
hardware snapshot evidence above. Private console and result records are in
`out/anne-device-private/iommu-live-followup/`; installation evidence is in
`out/anne-device-private/kernel-install-20261022T190306Z/`.

<a id="anne_gpu_iommu_audit-ram-only-secure-monitor-probe"></a>
### RAM-only secure-monitor probe

Following the user's authorization to modify the secure monitor, REA 6.7.0
and official Ghidra 12.1.4 were installed locally under ignored `out/re-tools`.
The CLI works without changing agent configuration. Analysis of the pinned
BL31 dispatcher and actual stock LK handoff is retained as full evidence in
`iommu-firmware-analysis/rea-{dispatch,handoff}.json` under the private tree.
Their evidence IDs are
`ev_5279c118172c6b8582d4e9b88267c183586ec212aa300d439c9ae16eecb13974`
and `ev_92b79bb46ba1fe2d4683f3caafea93b6df7cd0c43baef009f9b32049fdc37f8e`.

Complete traversal of BL31's static mapping table corrects an intermediate
assumption: record `0x13550` already maps PCTRL `0xe8a09000` for 4 KiB, as
well as the separate adjacent mapping at `0x136f0`. Both GPU master IDs
`0xe8a09c0c` and integration control `0xe8a09068` are covered. No mapping
extension is necessary. DMSS is also mapped by stock BL31. This establishes
secure virtual mappings, not successful live accesses or a GPU SMMU route.

`scripts/anne/build-monitor-probe.py` accepts only the exact pinned BL31 and
RAM LK hashes. It replaces callback 2's DDR statistics copy routine with a
bounded read-only diagnostic, leaving DDR protection services unchanged.
SMC `0xc500aa01`, callback cookie `0x55bbcce2`, accepts an independent argument
cookie `0x414d4552`. Selector 1 returns signature `0x414d0001` without MMIO;
2/3 read DMSS global RTL_VER/RTL_INF0; 4/5 read the two named PCTRL words;
16..71 choose the same seven interfaces and eight registers as the previous
kernel snapshot. All other selectors, including upper-bit aliases, fail.
There are no caller-supplied pointers, arbitrary register addresses or writes.
The stock dispatcher sign-extends the 32-bit result: interpret the low 32 bits.

The experimental LK replaces its final branch at `0x10b720`, after normal
BL31 loading, with a trampoline in the previously used kernel-preload slot
`0x2bc100`. It copies the staged plaintext to `0x35e00000`, synchronizes D/I
caches using CTR_EL0 line sizes, preserves the handoff arguments, and branches
to the original entry. `boot-monitor-probe.py` regenerates and compares the
profile, verifies RAM loader/TEE/monitor staging, and boots the installed
kernel. It contains no eMMC write command. A reboot discards the experimental
loader and monitor. Callback 2's normal statistics service is unavailable
during this experimental boot.

An AArch64 QEMU userspace test executes the actual probe and preload code,
checking every whitelisted selector, rejected aliases/cookies, complete copy,
and preserved handoff arguments against mock memory. It does not establish
secure privilege, real controller behavior, or external IOMMU isolation.
The first experimental boot passed RAM readback and reached msh, but the CDC command
path became silent and acknowledged vendor keyboard injections did not reach
the shell. The user confirmed the normal msh prompt remained on screen. USB
bus and configuration resets did not restore command delivery. The signature
and real register reads could not be verified on that boot. Private records are in
`iommu-monitor-probe/boot-20261022T232655Z.json` and
`iommu-firmware-analysis/live-status.json`.

After the user rebooted into BootROM and requested the modified monitor again,
the second boot passed RAM verification and the signature call returned
`0x414d0001`. This establishes execution of the modified EL3 callback. The
console remained responsive for all 61 whitelisted selectors. Reads returned:

| Secure word | Value |
| --- | --- |
| DMSS global RTL version, `0xfffc6000` | `0x01100021` |
| DMSS global RTL information, `0xfffc6004` | `0x096060a2` |
| PCTRL GPU master IDs, `0xe8a09c0c` | `0x063e737b` |
| PCTRL integration control, `0xe8a09068` | `0x0760e800` |

The four five-bit GPU read/write master fields decode to 27,27,28,28. Their
low five bits agree with the BL31 fault IDs `0x3b/0x3c`; this does not identify
the physical ASI route. Integration override bit 11 remains set. Per-interface
RTL security-region counts were 14,14,14,16,16,15,16, matching the pinned BL31
table. The sampled statistics mask/cycle/filters/counters and security status
were zero on all seven interfaces. Disabled counters cannot establish which
interface carries GPU traffic. No controller configuration was changed.

The secure access obstacle is now resolved for these whitelisted registers.
An external GPU translation base, stream routing, page-table context and
contained denied-DMA test remain unestablished. DMSS region protection is not
address translation, so it cannot replace the existing shadow backend for
fragmented appliance backing merely because EL3 can now read it.

The same modified boot passed `diag gpu`: 296 frames in five seconds, 297
acknowledged scanout transitions including restore, average GPU render
2124 µs. Records are `iommu-monitor-probe/boot-20261022T233519Z.json`,
`iommu-firmware-analysis/retry-console.jsonl`, and `secure-snapshot.json`
under the private directory. No external GPU IOMMU was enabled.

<a id="anne_gpu_iommu_audit-gpu-path-counter-experiment"></a>
### GPU-path counter experiment

The next experimental monitor revision returns signature `0x414d0002` and
adds selector 6 to read DMSS `GLB_STAT_CTRL` at `0xfffc6260`. Selectors
`0x100..0x17f` replace only bits 0..6 of that fixed register and return its
previous value. Upper bits are preserved; invalid selectors and cookies
cannot write. These are statistics controls, not protection or translation
controls. QEMU executes the actual probe to check bounds, cookie rejection,
upper-bit preservation, previous-value return and restoration.

This revision passed RAM readback and live signature verification. The initial
statistics value was zero. Enabling the seven counters returned zero and
read back `0x7f`; both experiments restored it to zero and verified readback.
With the same graphics image, idle ASI 6 read/write counts did not advance.
During the cold triangle, ASI 6 write/read deltas were 324M/78M count units
approximately. A warm repeat returned write delta 312845440 over 5.210 s and
read delta 75716032 over 5.220 s. ASI 2 carried the display's read traffic;
ASI 4 remained active during idle. This is strong evidence associating ASI 6
with the GPU workload, rather than treating a firmware fault classification
as a physical interface number. No master-ID filter was programmed, so it is
not proof that ASI 6 has no other masters or that both GPU ports are isolated.
Records: `iommu-firmware-analysis/route-{measurement,warm-measurement}.json`
and `route-console.jsonl` under the private directory.

The broader source check also reviewed the complete HiSilicon IOMMU driver:
it manages shared page tables without hardware MMIO. The GPU platform wrapper
exposes power/clock controls and a GATOR bypass, not a discovered external
translation setup. The T830 register schema exposes one translation root per
address space; CCI setup configures coherency/QoS. Arm's
[Mali virtualization guide](https://documentation-service.arm.com/static/6807606e79c4c17ad8036586),
page 108, describes the external SMMU required for guest-physical to machine
translation in its reference design. That newer GPU reference is not an
Hi6250 register recipe. An undocumented Hi6250 route remains unknown.

Revision 3 returns `0x414d0003` and adds read-only selectors `0x80..0xbf` for
the 64 region-policy words on ASI 6 (`0xfffc3500..0xfffc35fc`). The statistics
write scope remains unchanged. Its machine-code bounds and all 64 reads pass
QEMU tests. Reading existing MPU policy is distinct from enabling translation
or relaxing the host's existing shadow-MMU validation.

<a id="anne_gpu_iommu_audit-ivp-identification-probe-2026-10-23"></a>
#### IVP identification probe (2026-10-23)

`diag gpu-smmu` (diagnostic index 33) maps only the kernel's device page at
`0xe8dc0000`, the SMMUv2 candidate from the pinned `hisi_6250_ivp.dtsi`.
It reads the three architected 32-bit identification registers at offsets
`0x20`, `0x24`, and `0x28`, using fault recovery and restoring the calling
address space before releasing interrupt exclusion. It writes no device
registers and exposes no device mapping to msh. The register definitions
are corroborated by Linux v6.12 `drivers/iommu/arm/arm-smmu/arm-smmu.h`.

Live results on the release build after formatting:

```text
smmu.candidate_base=0xe8dc0000
smmu.valid=0x0
smmu.read_faults=3
smmu.first_esr=0x96000010
smmu.first_far=0xe8dc0020
```

These are synchronous external aborts, not successful reads of zero. They
do not distinguish a missing block from power, clock, or security restrictions.
The vendor IVP node still supplies no evidence of a GPU connection. This
probe therefore does not establish either a usable external GPU translator
or the physical absence of one. The console remained responsive afterward.
The installed kernel partition passed full readback verification (SHA-256
`32db827fc96c991c6e8242b959eaaeaffaac34b5a809160b4bd77f4fa8ca10fe`).

A bounded static check of the decrypted ARM32 TEE kernel's immediate MMIO
address constructions found PCTRL, GPIO, audio, display, GIC, and TZPC pages,
but no additional GPU translation route. This is a limited code-pattern
check, not an exhaustive proof about all trusted applications or computed
addresses. Private probe logs and static-analysis excerpts remain under
`out/anne-device-private/iommu-firmware-analysis/`.

<a id="anne_gpu_iommu_audit-tfresearch-and-guarded-secure-reads-2026-10-23"></a>
#### TFResearch and guarded secure reads (2026-10-23)

The supplied `/home/lucas/Code/TFResearch` contains the exact pinned BL31
image, upstream TF-A v1.3/v1.5 reference sources, and assembly attribution.
It does not contain recovered Hi6250 platform C or a GPU SMMU route. Its
upstream Juno SMMU setup is for different hardware. Binary inspection is
still required before applying its layout claims: `out/merge/layout.h` and
`PROVENANCE.md` label `0x1b000..0x1bb00` as zero padding, but that range
contains 977 nonzero bytes, including live GOT entries used by this image.
No patch uses that alleged padding.

The new `smmu-id` monitor variant uses those upstream exception-flow
references together with the actual vendor instructions. Its changes are
bounded to callback 2, the current-EL SP0 synchronous exception slot, and
one redundant translation-map record. The original DMSS mapping at
`0x13570` already covers `0xfffc0000..0xfffccfff`; replacing its duplicate
at `0x13630` with a secure read-only Device mapping of `0xe8dc0000` does
not remove DMSS coverage or change MPU/device policy. No device register
writes or eMMC writes are performed by this variant.

The exception guard returns only from a same-EL **read** data abort whose
ELR equals the probe's single fixed `ldr w0, [x4]` instruction and whose FAR
matches the selected address. Other exceptions take the stock fatal path.
Its two scratch registers are caller-saved; it uses no shared recovery state
or exception stack writes. Its fallback crash report has modified x9/x10.
It does not recover asynchronous SError or bus transactions that never finish.

`tests/test_anne_monitor_smmu.py` runs the production callback and guard in
QEMU at EL3 with translation enabled. It checks fixed 32-bit ID reads,
cookie/upper-selector rejection, repeated real translation-fault recovery,
and that another faulting PC remains fatal. A synthetic pinned-image test
also verifies all changed bytes remain within the three declared patches
and the original input-hash checks reject unknown firmware.

The RAM profile booted successfully on the phone. Live results:

| Selector | Operation | Low 32 bits of return |
|---|---|---|
| 1 | signature | `0x414d0004` |
| 5 | intentional unmapped read at `0x20000000` | `0x96000006` (L2 translation fault) |
| 2 | secure ID0 read at `0xe8dc0020` | `0x96000010` (external abort) |
| 3 | secure ID1 read at `0xe8dc0024` | `0x96000010` (external abort) |
| 4 | secure ID2 read at `0xe8dc0028` | `0x96000010` (external abort) |

The three candidate reads also faulted after a successful GPU animation.
Thus a simple nonsecure-access restriction alone does not explain this
candidate's failure. Power/clock gating, an absent block, or another fabric
restriction remain possible; these reads do not distinguish them. The
animation rendered 296 frames in five seconds, restored the display, and
all eight CPUs still reported EL2 monitor readiness after fault recovery.
Private evidence: `smmu-secure-console.jsonl`, `smmu-monitor-boot.log`, and
`iommu-monitor-smmu-probe/boot-*.json` under the private directory.

Reproduce the bounded variant with `build-monitor-probe.py --variant smmu-id`.
The existing policy profile remains the default, unchanged at signature
`0x414d0003`. Neither profile implements external GPU address translation.

<a id="anne_gpu_iommu_audit-fault-cause-identified-by-the-noc-error-logger-2026-10-23"></a>
#### Fault cause identified by the NoC error logger (2026-10-23)

The external-abort syndrome alone was ambiguous. A subsequent read-only
snapshot of the system-bus NoC error logger identifies the recorded cause:
**reserved-address access** for the secure CPU read at `0xe8dc0020`.
This narrows the earlier power/clock/firewall possibilities for this fault;
it does not prove that no alternate fabric configuration could expose a block.

The pinned `hisi_6250_noc.dtsi` places the system-bus error probe at
`0xe9840000 + 0x1c00`. Its register offsets and the vendor error decoder
(`hisi_noc_info_hi6250.c`, `hisi_noc_err_probe.c`) give:

| Register | Value | Interpretation |
|---|---|---|
| FAULTEN `+0x08` | `0` | error interrupt disabled |
| ERRVLD `+0x0c` | `1` | valid latched error record |
| ERRLOG0 `+0x14` | `0x80030100` | error bits 10:8 = 1, read opcode 0 |
| ERRLOG1 `+0x18` | `0x0004ba00` | CPU initiator 4, target 23, subrange 1 |
| ERRLOG3 `+0x20` | `0xe8dc0020` | fault address low word |
| ERRLOG4 `+0x24` | `0` | fault address high word |
| ERRLOG5 `+0x28` | `0x1011` | raw user/master metadata |
| ERRLOG7 `+0x30` | `0` | secure transaction under the vendor decoder |

The matching vendor route entry is `{0x04, 0x17, 0x1, 0x0}`. Adding its
zero address base to ERRLOG3 reproduces `0xe8dc0020`, without assuming
that the logged address was already absolute. The route's vendor label
is `HKMEM(noc_sys2hkmem)`, not an IVP target. Error code 1 is explicitly
`Master: access reserved memory space`; power-down, permission, and
firewall errors have distinct codes 3, 4, and 5. The valid flag remained
set after inspection. No error-clear, enable, power, clock, or protection
register writes were made.

This identifies an address-decoding rejection in the current fabric
configuration. The inactive IVP device-tree fragment is not evidence of a
reachable external GPU IOMMU. The pinned clock/power headers contain no
IVP controls, its NoC tables contain no IVP target, and the IVP Kconfig
platform choices cover Hi3650, Hi3660, and Kirin970 rather than Hi6250.
These corroborate an unused template/address mismatch, without establishing
the physical absence of every possible undocumented translation block.

Private evidence: `iommu-firmware-analysis/smmu-noc-fault.json` and the
corresponding reads in `smmu-secure-console.jsonl`. Additional pinned vendor
sources and hashes are in `out/anne-gpu-research/fault-origin/sources.json`.

<a id="anne_gpu_iommu_audit-programmable-routing-check-and-online-comparison-2026-10-23"></a>
#### Programmable routing check and online comparison (2026-10-23)

An additional online check compared these complete, nontruncated repository
trees and selected vendor register headers:

- OpenKirin Hi6250, commit `a5c1fdbf21d0750a69166acf65b21d324b882d60`.
- OpenKirin Hi3650, commit `73c2ab7bed5ae7925965d2df79ca26a761f6cd1d`.
- hisi-oss Hi6250 EMUI 9, commit `366ffa4b724589d95e942ec6e369a1013128f961`.

Hi3650's CPU address map explicitly defines IVP's SMMU at `0xe8dc0000`
and its service target at `0xe9850000`. Both Hi6250 address maps omit
these entries. Hi3650 CRG PEREN4 assigns IVP core/sysbus/TCXO clocks to
bits 3/5/7. Hi6250 assigns bit 3 to IPF0 and leaves bits 5/7 reserved.
Hi3650 PMCTRL names an IVP NoC power-idle control at bit 14; Hi6250
does not. Borrowing the Hi3650 enable sequence would therefore program
different or reserved controls on the phone.

The old Hi6250 tree contains an IVP subsystem header byte-identical to
Hi3650's, and an IVP DTS fragment with the same SMMU address. Unlike the
Hi3650 fragment, its real IVP regulator reference is commented out and
its named IVP gate clock is replaced with a fixed PLL reference. Combined
with the absent address-map/clock entries and measured reserved-address
fault, this supports an unused copied-template interpretation. It does
not prove that every undocumented hardware variant lacks that block.

The vendor `cfgsys_routeid_addr_tbl` is a const software lookup table.
`noc_find_addr_from_routeid()` searches it to decode logged addresses;
it does not install hardware routes. No IVP/GPU address-decoder or DMA
path selection control was found in the reviewed PCTRL, SCTRL, PMCTRL,
CRG, or IVP subsystem schemas. The named remaps found concern eFuse,
ISP/DSP-local memory, or memory interleaving, not a GPU-to-external-SMMU
connection. Making a CPU MMIO page reachable would also be insufficient
without proving that GPU DMA passes through that translator.

No hardware routing changes were attempted on this evidence. The remaining
blocker is a real Hi6250 translator and programmable connection, not a
missing software entry in the NoC error decoder. Repository snapshots,
source paths, and hashes are retained under
`out/anne-gpu-research/routing-online/`.
