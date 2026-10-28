# Dango Linux appliance builds and integration

The old lavapipe executor and reference-session broker have been removed. The maintained native Vulkan route is documented in [graphics](graphics.md#native-vulkan-frontend).

The [architecture](architecture.md#dango-and-linux-appliances) describes the current host boundary. The phone Panfrost path and QEMU virtio/lavapipe path are different backends. Build-selected 128/512 MiB layouts exist in `shared/include/midix/abi/dango_guest_layout.h`; that alone does not prove complete native lavapipe integration. Phone rendering and scanout evidence is in [graphics](graphics.md).

- [Minimal ANE-LX1 Linux driver appliance](#anne_gpu_appliance)
- [Linux appliance — minimal configuration policy](#linux_appliance_minimal)
- [Phase M — zero-copy / import: verdict and design](#gpu_zerocopy_import)
- [QEMU appliance integration limits](#gpu_appliance_remaining)
- [Guest layout and mapping capacity](#gpu_appliance_guest_memory)
- [Phone acceptance and recovery](#huawei_p20_lite_dango_plan)

<a id="anne_gpu_appliance"></a>
## Minimal ANE-LX1 Linux driver appliance

<a id="anne_gpu_appliance-minimal-ane-lx1-linux-driver-appliance"></a>

`panfrost` is a separate Dango profile for the Huawei P20 Lite's Mali-T830.
The Linux Panfrost driver includes an `arm,mali-t830` match and Midgard MMU
support. Both Panfrost and the generic Dango shared-memory channel driver are
built into the kernel; there are no modules to install at runtime.

The default build supplies a driver image and an Ame/Linux channel acceptance
path with the GPU disabled. The hardware build adds a Hi6250 host mediator,
Linux Panfrost, Mesa EGL/GLES and a native `diag gpu` endpoint. Hardware
rendering and the five-second rainbow triangle were verified on the physical
phone on 22 October 2026. QEMU validates the channel separately; it does not
emulate the Mali-T830.

Mali is render-only. The native EGL path imports a 512x512 SharedGpu DMA-buf
as an EGLImage render target. Ame's framebuffer service copies the completed
image into the panel buffer, displays it for five seconds, then restores the
terminal. This removes GPU readback and format conversion; native display
scanout remains separate. A pbuffer/readback fallback remains for import failure. No Vulkan ICD or software rasterizer is included.

<a id="anne_gpu_appliance-hardware-renderer"></a>
### Hardware renderer

Build the pinned Mesa 24.3.4, libdrm 2.4.124 and zlib 1.3.1 sources, then the appliance:

```sh
scripts/linux/build-panfrost-mesa.sh
DANGO_LINUX_SRC=/path/to/linux-v6.12 \
  DANGO_PANFROST_HARDWARE=1 DANGO_PANFROST_RENDERER=gles \
  DANGO_LINUX_TIMER_HZ=1920000 scripts/linux/build-panfrost-linux.sh
```

Mesa requires Meson, Ninja and an `aarch64-linux-gnu` GCC/G++ toolchain with
its libc sysroot. Only the required AArch64 libraries enter the compressed
initramfs; `mesa-runtime.json` records the runtime closure and hashes.
The default runtime expands those files into RAM. For the CTS allocation stress
tests, `DANGO_PANFROST_ROOTFS=squashfs` opts into a read-only XZ SquashFS runtime
inside the initramfs. Use that setting for both kernel and renderer builds. The
kernel build applies the separate `dango_panfrost_squashfs.fragment` and checks
the explicit `panfrost-squashfs` policy; the ordinary profile still forbids block
support. Only a loop device backed by the packaged runtime file is needed.
The bootstrap mounts the image read-only, supplies a 16 MiB `/tmp` tmpfs, then
executes the same renderer. The guest memory layout remains 128 MiB.
`rootfs-storage.json` records the image, bootstrap and archive hashes.

The maintained compressed build boots the renderer in a CPU-only QEMU VM.
A separate DRM-shim fixture passes the unmodified CTS 10,000-primary-buffer
allocation case at 128 MiB after the lazy recording-state change. The ordinary
RAM-expanded runtime still OOM-kills that case. Compressed storage has not yet
been validated on the phone; see [graphics](graphics.md#anne_vulkan_completion).

Configure the native build with `MOCHI_SYSTEM_PROFILE=huawei-anne-gpu`,
`DANGO_LINUX_PROFILE=panfrost` and `DANGO_LINUX_ARTIFACT_DIR` pointing to these
artifacts. Flash through `scripts/anne/flash-el2.py` as described in
[`FLASH_EL2.md`](../boards/huawei-anne/operations.md#flash_el2).

The native frontend is described in [Native EGL/GLES](graphics.md#native_graphics).
Run `diag gpu` from the Ame shell. The renderer requires Mesa's Mali-T830
renderer string and returns a checksum of the completed shared pixels. The
hardware triangle baseline is `36244b55`. Its reported render time includes
drawing, GPU completion and checksum; it excludes native setup, cache storage
and the five-second display hold. The host mediates the GPU MMU with Mali-format
shadow page tables. Only the registered pixel arena is DMA accessible; command
and cache arenas remain CPU-only. GPU page tables must remain in ordinary owned
guest RAM. External SMMU support is not enabled.

Mesa's bounded EGL blob cache persists through libmochi FS on Ame, rather than
a Linux block/filesystem stack. The manager must start after `io.fs` to establish
its filesystem session. See [optimization status](graphics.md#gpu_optimization_work).

<a id="anne_gpu_appliance-build"></a>
### Build

Use a Linux host with make, Clang/LLD, LLVM tools, Python 3, bc, flex, bison,
dtc, cpio, tar and xz. For the tested source, check out upstream Linux v6.12
(`adc218676eef25575469234709c2d87185ca223a`) separately. The builder stages a
symlink overlay and leaves the supplied source files untouched.

```sh
DANGO_LINUX_SRC=/path/to/linux-v6.12 scripts/linux/build-panfrost-linux.sh
```

Default output: `out/dango-linux-panfrost/`. Override `DANGO_LINUX_OUT`,
`DANGO_LINUX_KOUT` and `DANGO_LINUX_JOBS` as needed. The output includes:

- `Image`: raw ARM64 kernel, with Panfrost and the Dango drivers built in.
- `initramfs.cpio`: device nodes, empty mountpoints, and one stripped static
  freestanding `/init`; no libc, shell or general-purpose distribution.
- `dango-linux.dtb`, `manifest.toml`: guest layout and package inputs.
- `linux.config`, `build-info.json`: resolved configuration, Linux revision,
  dirty-source flag, sizes and SHA-256 hashes.
- `dango-panfrost.tar.xz`: compressed distributable of the above runtime inputs,
  the agent and provenance, excluding intermediate kernel objects.

Configuration uses `KCONFIG_ALLCONFIG=<fragment> allnoconfig`, so unspecified
optional facilities stay off. The build enforces the Panfrost minimality policy.
Networking, block devices, PCI, USB, modules, input, audio, namespaces, debug
information, fbdev and other GPU drivers are disabled. ARM64 forces SMP support;
the boot arguments restrict execution to one guest CPU. `EXPERT` forces the
`DEBUG_KERNEL` umbrella on, but actual optional debug facilities remain off.
The initial guest memory layout remains 128 MiB; image size is not RAM usage.

<a id="anne_gpu_appliance-prove-ame-communication"></a>
### Prove Ame communication

```sh
cmake --preset dango-panfrost-qemu-dev
cmake --build --preset dango-panfrost-qemu-dev --target anko mochi_system
python3 scripts/test/dango-panfrost.py
```

This uses the existing VM-only `dango-hello` service authority profile, with
Panfrost guest artifacts. No GPU capability, PCI function or SMMUv3 is required.
The manager maps the shared channel independently of its virtio-gpu assignment
path. Its native ring probe leaves nonzero counters; the guest and host then
exchange 512 ordered ping/pong messages with ring wraparound, HVC notification,
virtual IRQ wakeups and read/poll checks. Both endpoint success markers are
required. Logs and `panfrost-result.json` go to
`out/dango-panfrost-validation/`. The transport acceptance ends after the
round trips. The hardware renderer uses a separate ongoing GPU request service.

`/init` checks `/dev/dri/renderD128` using `DRM_IOCTL_VERSION` if a GPU has been
assigned. With the supplied DTB it reports `Panfrost built in; GPU unassigned`
and proceeds with the independent channel test. An absent GPU is not reported
as a successful GPU probe.

<a id="anne_gpu_appliance-how-the-numbers-are-held"></a>
### How the numbers are held

Every Hi6250 hardware value lives in `boards/huawei-anne/gpu.h`, each traced to
a pinned vendor or upstream source: the register window and three SPIs from
`hisi_6250_gpu.dtsi`, the g3d regulator command words from
`hisi_6250_powerip.dtsi`, mailbox-14's source/destination bit mask from
`hisi_6250_ipc.dtsi`, and register names and bit positions from Linux's
`panfrost_regs.h`. `kernel/vm/mali.cc` and `kernel/vm/mali_shadow.cc` contain no
raw hardware literals; the page-table entry layout is declared once in
`anko/mali_shadow.h` so the mediator cannot drift from the shadow builder.

The guest-visible side is separate on purpose. `shared/include/midix/abi/anne_gpu.h`
owns the synthetic MMIO window and the three virtual IRQs, and
`scripts/linux/gen-dango-binding.sh` derives the guest device tree's address,
size and SPI numbers from that header. Nothing retypes those numbers in the
template, and `tests/test_panfrost_appliance.py` builds the DTB and reads it
back with `fdtget` to prove the node matches the header.

Two behaviours worth knowing, both verified against the upstream driver:

- `JS_CONFIG.START_MMU` is **not** required of the guest. It is Panfrost's
  `HW_ISSUE_10649` workaround, and `hw_issues_t830` does not carry that erratum,
  so a T830 never sets it. The field that actually selects the page table is
  the address-space index in `JS_CONFIG`'s low nibble
  (`panfrost_mmu_as_get` feeds it into `JS_CONFIG_NEXT`), and that is pinned to
  the single owned space.
- `AS_MEMATTR` does not share the translation-table base layout. Transtab is
  `pa | BIT(2) | 3`; memattr packs `0x88 | 0x8D<<8 | 0x88<<16` from
  `io-pgtable-arm.c`. The host writes both canonically and ignores the guest's
  values.

<a id="anne_gpu_appliance-hardware-acceleration-contract"></a>
### Hardware acceleration contract

Acceleration comes from the guest's own Linux Panfrost driver driving the real
Mali-T830. Ame does not render anything: it owns power, physical MMU roots and
interrupt routing, and emulates the register window the guest programs. Three
facts about T830 were established against Linux v6.12 and shape every check here.

**No firmware.** Midgard is a job-manager design; `mali_csffw.bin` belongs to
Panthor (arch10.x, G710+). There is no `request_firmware` anywhere in
`drivers/gpu/drm/panfrost/`, so the initramfs needs no `/lib/firmware` entry.
Per-fd address spaces, job scheduling and frequency scaling are all kernel-side.

**No `START_MMU`.** `JS_CONFIG_START_MMU` is written only under
`HW_ISSUE_10649` (`panfrost_job.c:234`), which `hw_issues_t830` does not carry,
so a T830 never sets it. The MMU must already be live from the `AS_COMMAND`
UPDATE path. The mediator therefore does **not** require that bit, and pins the
address-space select in `JS_CONFIG`'s low nibble instead.

**Reset completion is polled, not just interrupted.** `panfrost_gpu_soft_reset`
polls `GPU_INT_RAWSTAT` bit 8 with a 10 ms budget and, on timeout, retries a hard
reset and then *fails the probe*. Because the guest reads that register through
the mediator, the mediator latches the completion itself and clears it on the
guest's write-one-to-clear to `GPU_INT_CLEAR`. A guest whose GIC line is not yet
wired can still finish probe; a missing latch would turn a working reset into
`-ETIMEDOUT`.

Guest DTB requirements, all confirmed from the driver: `compatible =
"arm,mali-t830"`, one `reg` region, interrupts named **job/mmu/gpu** in that
order, and `clocks[0]` as the core clock (`devm_clk_get(dev, NULL)` ignores
`clock-names`, so order decides). `mali-supply` references an always-on fixed
regulator with no voltage or GPIO controls; the host owns physical g3d power
through the LPM3 mailbox. This avoids the dummy-supply probe failure without
giving Linux control of the physical supply. Deliberately omitted:
`operating-points-v2` (makes devfreq/OPP mandatory-success), `power-domains`
(two or more is a hard `-EINVAL`) and `iommus` (Panfrost drives the GPU's own
MMU, not an external SMMU). A nonzero fixed-clock rate is sufficient — the
driver only logs it.

Known limits of the current mediation, stated rather than papered over: one
address space is advertised, so a second concurrent MMU client that forced the
allocator onto AS1 would be contained instead of mapped; and the perfcounter
block is refused (it is the only window that can program a DMA address), so
`perfcnt` stays disabled. Probe only writes zeros there, so it still succeeds.

<a id="anne_gpu_appliance-vm-teardown-and-the-gpu"></a>
### VM teardown and the GPU

A VM that owns the GPU is released before any backing the device could still
DMA from is destroyed. `retire_vm` calls `mali::release` first — ahead of the
state change, the device-assignment revoke and `stage2_destroy` — and a refused
reset leaves the VM live and inspectable so a later destroy can retry. The
release path only reclaims the shadow tables after the device acknowledges a
reset, because a table freed under a live MMU is exactly the reuse hazard the
retention exists to prevent. `destroy_vm` reports `E_BUSY` in that case instead
of claiming a teardown that did not happen. `tests/test_panfrost_appliance.py`
asserts the ordering, so the gate cannot be quietly moved below the frees.

<a id="anne_gpu_appliance-physical-phone-gate"></a>
### Physical phone gate

The guest GPU node is present but `disabled` unless `DANGO_PANFROST_HARDWARE=1`,
and the manifest records `mali_hardware`. Enabling it is a statement that the
host mediator is the only authority for the device; it is not evidence that the
physical part has been assigned.

The hardware path uses the phone EL2 route described in
[the Dango plan](linux-appliances.md#huawei_p20_lite_dango_plan) and
[EL2 evidence](../boards/huawei-anne/boot.md#el2). The host mediates registers,
interrupts, power and Mali MMU roots. The external SoC SMMU is not enabled.
Hardware rendering demonstrates the working job path; it does not establish
complete fault containment against a hostile guest. Building this profile does
not flash an image; installation uses the separate EL2 flash helper.

<a id="anne_gpu_appliance-boot-and-compiler-fixes"></a>
### Boot and compiler fixes

EL1 uses cached memory while the EL2 monitor runs with its MMU and data cache
disabled. VM contexts, monitor stacks and guest boot data must reach the point
of coherency before handoff. Host zero-fill cache lines are cleaned and
invalidated before guest execution so they cannot overwrite guest writes later.
The vCPU context is cleaned before entry and invalidated after return.

With the tested GCC 16 toolchain, Mesa's `{0}` shader-key initializer left bytes
in the union uninitialized, crashing Midgard shader compilation. The pinned
Mesa build explicitly clears the full key with `memset` at both initialization
sites. `scripts/test/panfrost-compiler.sh` reproduces shader compilation under
QEMU-user with Mesa's fake DRM shim. It verifies the compiler path without
submitting GPU jobs; the shim is not included in the phone runtime.

<a id="anne_gpu_appliance-recorded-build-and-validation"></a>
### Recorded build and validation

The later native EGL/GLES frontend was also verified on 22 October 2026:
package `out/anne-gpu-native-egl-v3`, two runs at 42,028 µs and 36,613 µs,
checksum `36244b55`, successful context recreation and terminal restoration.
See [Native EGL/GLES validation](graphics.md#native_graphics-validation) for the image
hash, evidence and API limits. The earlier driver-path evidence follows.

22 October 2026, physical ANE-LX1, package `out/anne-gpu-triangle-v10`:

- Installed kernel SHA-256:
  `a9afaf83ac1910917abf26ca7c6e6c94e12d427dbb65cea1e77be52ff2fd88f6`.
  The complete 24 MiB partition readback matched before boot.
- Mesa reported `GL_RENDERER=Mali-T830 (Panfrost) EGL=1.5`, followed by
  `hardware renderer ready gpu_id=830`.
- Two `diag gpu` runs returned status zero and checksum `36244b55`.
  Drawing, completion, readback and conversion took 42,427 µs and 29,564 µs.
  Total command times were 5.062 s and 5.059 s, including the display hold.
  Both restored the terminal; the user confirmed the triangle on the phone.
- Retained hardware evidence is in the ignored private files
  `out/anne-device-private/gpu-v10-boot.log` and `gpu-v10-serial.log`.
  The flash record is `kernel-install-20261022T105838Z` in the same directory.
- 44 appliance, channel, platform, flash-helper and USB tests passed:
  `.venv/bin/python3 -m unittest tests.test_panfrost_appliance tests.test_channel tests.test_platform_probe tests.test_anne_flash tests.test_usb tests.test_anne_usb_sender`.
- The separate native-host QEMU appliance test passed 512 ordered channel
  round trips with the GPU disabled. The CPU-only Mesa compiler regression
  also passed. These checks supplement the physical rendering evidence.

Source references: [Linux v6.12 Panfrost match table](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/panfrost/panfrost_drv.c),
[Panfrost dependencies](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/panfrost/Kconfig),
[Midgard binding](https://github.com/torvalds/linux/blob/v6.12/Documentation/devicetree/bindings/gpu/arm,mali-midgard.yaml).

<a id="anne_gpu_appliance-external-gpu-smmu-investigation"></a>
### External GPU SMMU investigation

The [Anne IOMMU routing audit](../boards/huawei-anne/gpu-isolation.md#anne_gpu_iommu_audit) found no supported
GPU-to-external-SMMU path in the pinned Hi6250 sources. The IVP/ISP/display
SMMUs cannot be assumed to cover Mali. The existing host-owned Mali shadow
MMU remains the isolation mechanism; no external GPU SMMU was enabled.

<a id="linux_appliance_minimal"></a>
## Linux appliance — minimal configuration policy

<a id="linux_appliance_minimal-linux-appliance-minimal-configuration-policy"></a>

Applies to every Dango guest kernel (`linux-artifacts/config/*.fragment`).

<a id="linux_appliance_minimal-principle"></a>
### Principle

The Linux guest exists **only** to run a hardware driver and a paravirtual channel back to Ame
(`docs/architecture.md §16`, "Linux as Driver Appliance"). It is not a general-purpose system. Every
kernel feature that is not required by (a) boot on arm64/QEMU `virt`, (b) the assigned-device flows,
or (c) the Dango channel is **off**. A fat guest is a bug: it enlarges the attack surface, slows
boot, and hides accidental dependencies.

Corollary: **prefer `=n`.** Accepting a subsystem "because defconfig enables it" is not allowed.

<a id="linux_appliance_minimal-mechanism"></a>
### Mechanism

Start from `defconfig`, then layer exactly one fragment with `scripts/kconfig/merge_config.sh`, then
`olddefconfig` — never hand-edit `.config`:

```
make LLVM=1 ARCH=arm64 O=$KOUT -C $DANGO_LINUX_SRC defconfig
$DANGO_LINUX_SRC/scripts/kconfig/merge_config.sh -m -O $KOUT $KOUT/.config <fragment>
make LLVM=1 ARCH=arm64 O=$KOUT -C $DANGO_LINUX_SRC olddefconfig
```

The `panfrost` profile instead uses `KCONFIG_ALLCONFIG=<its fragment>
allnoconfig`. This explicitly opts in to only the boot, render-driver and channel
requirements, avoiding newly visible defaults from a defconfig merge. Its
resolved config is checked by the same enforcement script. See
[the ANE appliance workflow](linux-appliances.md#anne_gpu_appliance).

- One fragment per profile. `dango_hello_arm64.fragment` stays the minimal bring-up profile; the GPU
  appliance is a **separate** fragment so boot regressions bisect cleanly (spec §19).
- Helpers must be pulled in by Kconfig `select`/`depends`, not duplicated by hand (spec §10).

<a id="linux_appliance_minimal-baseline-deny-list-must-be-n-in-every-appliance"></a>
### Baseline deny-list (must be `=n` in every appliance)

`MODULES, BLOCK, NET, PCI †, USB, SCSI, ATA, SOUND, INPUT, WIRELESS, BT, CGROUPS, NAMESPACES, AUDIT,
SECURITY, BPF_SYSCALL, PERF_EVENTS, FTRACE, KPROBES, SWAP, HIBERNATION, SUSPEND, SMP, CPU_IDLE, VT`

† `PCI` is off in the minimal (hello) profile; the GPU profile turns on only `PCI`, `PCI_HOST_GENERIC`,
`VIRTIO`, `VIRTIO_PCI` (plus Kconfig-selected helpers), and nothing else.

Also: no filesystem beyond the initramfs needs, no net, no modules, single vCPU (`SMP=n`), no cpuidle.

<a id="linux_appliance_minimal-known-forced-on-empirically-verified-2026-09-20-linux-v612-arm64"></a>
### Known forced-on (empirically verified 2026-09-20, Linux v6.12 / arm64)

A `=n` in a fragment is only effective if nothing else `select`s the symbol.
These were observed to resist `=n`; the checker reports them as warnings:

| Symbol | Why it stays on | Fix |
|---|---|---|
| `SMP` | arm64 Kconfig: `config SMP def_bool y` — **architecture-forced**, not user-settable | none (arm64 is SMP-only) |
| `CGROUPS` | `SCHED_AUTOGROUP=y` (defconfig default) `select`s it | **fixed**: the hello fragment now sets `SCHED_AUTOGROUP=n` (+ `SCHED_MC`/`SCHED_SMT`/`CPU_FREQ=n`) |
| `CPU_IDLE` | `default y if ACPI`; forcing path not yet removed | investigate (drop ACPI, or find the selector) |
| `PERF_EVENTS` | `default y`; forcing path not yet removed | investigate |

Lesson: when a deny-list entry will not turn off, find its `select`/`def_bool`
parent rather than re-adding the `=n` line.

<a id="linux_appliance_minimal-per-flow-allow-lists"></a>
### Per-flow allow-lists

| Profile | Adds (beyond the arm64 boot baseline) |
|---|---|
| `dango_hello_arm64` | nothing — boot + PL011 console only |
| `dango_gpu_arm64` (virtio-gpu, spec §10, Phases E–F) | `PCI`, `PCI_HOST_GENERIC`, `VIRTIO`, `VIRTIO_PCI`, `DRM`, `DRM_VIRTIO_GPU` (+ helpers such as `DRM_KMS_HELPER`) |
| `dango_panfrost_arm64` (ANE-LX1 driver image) | `DRM`, `DRM_PANFROST`, `PM`, `COMMON_CLK`, `REGULATOR`, shmem/devtmpfs/proc/sysfs for the agent and transport; Panfrost selects its DRM scheduler, GEM shmem, devfreq and internal MMU page-table helpers. No PCI or virtio GPU. Physical GPU assignment remains unimplemented. |
| `dango_gpu_amd64`-equivalent (amdgpu, Phase Q, future) | the virtio-gpu set minus `DRM_VIRTIO_GPU`, plus `DRM_AMDGPU` and its firmware/FW_LOADER deps |

Nothing else is admissible without a written, empirically-grounded justification recorded here.

<a id="linux_appliance_minimal-enforcement"></a>
### Enforcement

The build calls `scripts/linux/check-minimal-config.py` on the resolved `.config`; a violation fails
the build. It asserts the deny-list is off and the flow's required set is on:

```
python3 scripts/linux/check-minimal-config.py --config $KOUT/.config --flow gpu
```

<a id="linux_appliance_minimal-review-checklist-for-any-fragment-change"></a>
### Review checklist (for any fragment change)

1. Does the resolved `.config` pass `check-minimal-config.py` for the profile?
2. Was any `=y` added that is not in the profile's allow-list (directly or via `select`)?
3. Did the change enlarge `.text`/initramfs or boot time measurably? If so, why?
4. Is the helper set pulled in by Kconfig rather than written by hand?
5. Does the minimal `dango_hello_arm64` profile still boot (bisectable)?

<a id="gpu_zerocopy_import"></a>
## Phase M — zero-copy / import: verdict and design

<a id="gpu_zerocopy_import-phase-m-zero-copy-import-verdict-and-design"></a>

Spec: AME GPU appliance §12.3 ("expose a Dango shared region to Linux as a
Linux-importable object … or a documented reason to keep the [bootstrap] memcpy;
must not weaken IOMMU ownership/revocation").

<a id="gpu_zerocopy_import-the-question"></a>
### The question

`dango-gpud` renders the bootstrap frame into the Dango shared staging window and
then **copies** it into a DRM dumb buffer (`gpud_present_finish()` in
`linux-artifacts/gpud/gpud_main.c`, one `memcpy` per row). Can that copy be
dropped by exposing the region to Linux as a dma-buf (or memfd) and importing it
into the assigned `virtio_gpu` as the scanout object?

<a id="gpu_zerocopy_import-verdict-no-for-virtio_gpu-the-bounded-copy-stays"></a>
### Verdict: no, for `virtio_gpu`. The bounded copy stays.

Verified against the exact guest tree the appliance builds (Linux **v6.12**,
`scripts/linux/build-gpu-linux.sh` → `$DANGO_LINUX_SRC`, line numbers below are
that tree):

1. **`virtio_gpu` cannot import a foreign dma-buf at all.**
   - `virtgpu_gem_prime_import()` (`drivers/gpu/drm/virtio/virtgpu_prime.c:145`)
     short-circuits only for a dma-buf this same device exported; for anything
     else it calls the DRM core (`:162`).
   - `drm_gem_prime_import_dev()` (`drivers/gpu/drm/drm_prime.c:934`) requires
     `dev->driver->gem_prime_import_sg_table` (`:955`) and calls it (`:970`).
   - `virtio_gpu` registers the stub `virtgpu_gem_prime_import_sg_table`
     (`virtgpu_drv.c:191`), and that stub is literally
     `return ERR_PTR(-ENODEV);` (`virtgpu_prime.c:165–169`).
   → `DRM_IOCTL_PRIME_FD_TO_HANDLE` on **any** dma-buf that virtio-gpu did not
   export itself returns `-ENODEV`. This is a property of the driver, not of our
   exporter, so no exporter change can make it succeed.

2. **Scanout is hard-wired to a virtio-gpu shmem object.** A dumb buffer is a
   `drm_gem_shmem_create()` object (`virtgpu_object.c:194`) whose pages are
   gathered with `drm_gem_shmem_get_pages_sgt()` (`:145`) and handed to the host
   by `virtio_gpu_object_attach()` (`:230`, `:234`). The scanout update casts the
   framebuffer's GEM object with `gem_to_virtio_gpu_obj(fb->obj[0])`
   (`virtgpu_plane.c:199`) and drives the host from `bo->hw_res_handle` /
   `bo->guest_blob`. A foreign GEM object would be mis-cast, so scanout would not
   accept an imported buffer even if the import ioctl were made to succeed.

3. **The capability is not a gate.** Since Linux 6.6 `DRM_IOCTL_GET_CAP` with
   `DRM_CAP_PRIME` returns `DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT` for
   *every* driver (`drivers/gpu/drm/drm_ioctl.c:246`; `drm.h` documents "always
   advertised"). A capability probe therefore cannot decide this; only the real
   `PRIME_FD_TO_HANDLE` call can. (The probe notes this so the next reader does
   not re-add a useless `GET_CAP` gate.)

4. **`CONFIG_VIRTIO_DMA_SHARED_BUFFER` does not help.** It enables the virtio
   dma-buf *sharing* framework virtio-gpu uses for its own **exports**; it adds no
   import path. The resolved guest config is `CONFIG_DRM_VIRTIO_GPU=y`,
   `CONFIG_VIRTIO_DMA_SHARED_BUFFER=y`, guest `CONFIG_VIRTIO_IOMMU=n` (the host
   SMMU translates IOVA == guest physical address).

There is also a reason specific to Ame that would block the "just make the region
the dumb buffer" variant: the staging region sits **outside guest RAM**
(`DANGO_GPU_STAGING_BASE`, `shared/include/midix/abi/dango_channel_guest.h`) and
its pages are `VmDmaClass::None` — `collect_vm_ram()` explicitly skips
`shared_staging` memory objects (`kernel/vm/vm_device.cc:199`). `virtio_gpu`'s
host resource needs device-DMA-able **guest RAM** pages, which the region is not
(by design, until the host registers it `SharedGpu`).

**So**: the region is the source of truth the host writes; the dumb buffer is the
object the device scans out. For `virtio_gpu` the two cannot be the same object,
and the bounded copy is the correct, documented bridge.

<a id="gpu_zerocopy_import-what-was-still-implemented-useful-and-required-by-any-future-backend"></a>
### What was still implemented (useful, and required by any future backend)

The export path exists end to end and is exercised as far as `virtio_gpu` allows;
it becomes a real zero-copy switch the moment a backend that supports
`gem_prime_import_sg_table` is dropped in (Phase Q/R):

- `shared/include/midix/abi/dango_shm_guest.h` — the region ABI: the read-only
descriptor page (`struct dango_shm_desc` + `struct dango_shm_region`), opaque
boot-generation-scoped ids, guest-physical geometry, and the `DANGO_SHM_*`
ioctls. Fixed capacities, compile-time layout/encoding asserts.
- `linux-artifacts/guest-driver/dango_shm.c` — built-in platform driver
  (`ame,dango-shm-v1`) → `/dev/dango-shm0`: `QUERY` (ids/geometry), `mmap`, and
  `EXPORT_DMABUF` with the guards below.
- `linux-artifacts/gpud/gpud_drm.{c,h}` — `PRIME_FD_TO_HANDLE` import, `GEM_CLOSE`,
  `GET_CAP`, and `gdrm_fb_create_handle()` so a framebuffer can be built from any
  GEM handle, imported or dumb.
- `linux-artifacts/gpud/gpud_main.c` — `gpud_import_probe()` runs once at
  start-up; on `GPU_BUFFER_REGISTER` a buffer whose region matches the probed
  geometry is scanned out **without a copy**, otherwise the dumb buffer + copy is
  the fallback. Absent device/region is not fatal.

<a id="gpu_zerocopy_import-the-test-that-fails-if-the-documented-assumption-stops-holding"></a>
### The test that fails if the documented assumption stops holding

`gpud` prints exactly one verdict line per run, e.g.

```
dango-gpud: zero-copy probe cap=3 verdict=import-rejected errno=19 copy=retained
```

`cap=3` is `DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT` — the kernel advertises
import for the device while the real ioctl refuses it. `errno=19` (`ENODEV`) is
the documented `virtio_gpu` refusal above. The probe is the **real**
`PRIME_FD_TO_HANDLE` call — the advertised capability cannot lie. If a future
kernel or backend starts accepting the import, `gpud` instead prints

```
dango-gpud: zero-copy probe cap=3 verdict=import-available zero-copy TEST FAILED (stale assumption)
```

The phrase `TEST FAILED` is in the shared failure vocabulary
(`scripts/test/ef_constants.py: FAILURE_WORDS`), so every appliance acceptance row
(the gpud/KMS/gpu-appliance rows check `no_failures`) **fails** at that point. The
assumption is therefore asserted, not merely written down: the day it stops
holding, the rows go red and Phase M's copy is revisited deliberately. (The
display keeps working either way: when the import succeeds, the imported path is
used; when it fails, the copy runs.)

<a id="gpu_zerocopy_import-the-iommu-rule-not-weakened"></a>
### The IOMMU rule (not weakened)

- A region is **non-DMA until registered `SharedGpu`.** `dango_shm.c` refuses
  `DANGO_SHM_EXPORT_DMABUF` with `-EPERM` unless the region carries
  `DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU`, and `map_dma_buf()` re-checks the bit,
  so a stale export cannot be mapped. The descriptor page is **read-only at
  stage 2**, so the guest cannot invent a region or set the bit itself: only the
  host, after Anko's explicit registration, can. The non-DMA state is the zero
  flag word (`_Static_assert(DANGO_SHM_REGION_FLAG_NONE == 0)`), so a bug that
  forgets the registration fails closed instead of opening a DMA window.
- Anko is the sole writer of the region's DMA class: `shared_arena_register_dma()`
  (`kernel/vm/vm_shared.cc`) is the only transition to `SharedGpu`, and it refuses
  unless the region is live and guest-mapped (`kernel/include/anko/vm.h`). A
  freshly attached region enters stage 2 as `VmDmaClass::None` and is absent from
  every DMA domain.
- **Revocation stays DMA-safe.** Anko's teardown keeps the detach-before-release
  ordering (`device_revoke_impl()` in
  `kernel/vm/vm_device.cc`: mask routes → clear bus master → detach the requester
  into the quarantine/faulting domain → invalidate+sync → unmap → only then
  release pages). On the guest side a region whose descriptor entry is dropped
  can no longer be exported, and every export owns a bounded slot that is
  released exactly once, so the guest never invents a reference the host has not
  accounted for. The Anko half is exercised by `shared_arena_probe`
  (`tests/kernel/shared_arena_probe.cc`) and the lifecycle row
  (`AME_VERIFY_LIFECYCLE` → `kernel/vm/vm_device_selftest.cc`).

<a id="gpu_zerocopy_import-phase-l-interface"></a>
### Phase L interface

Phase L had already landed its ABI constants and Anko side when this landed
(`DANGO_SHM_GUEST_BASE/SIZE` in `dango_channel_guest.h`, `SharedArena` +
`shared_arena_register_dma()` in `kernel/include/anko/vm.h` / `kernel/vm/
vm_shared.cc`), so `gpud` and `dango_shm.c` are written to that stated shape: a
built-in `ame,dango-shm-v1` platform driver that reads the read-only descriptor
page and exposes each region to userspace through mmap. Two things it still
assumes, until the Phase L driver side lands:

- the `ame,dango-shm-v1` device tree node and its `dango_shm` reserved-memory
  window are already published by Phase L
  (`linux-artifacts/dts/dango-linux-gpu.dts.in`), so the driver binds to the
  window it reads;
- the descriptor page is the first page of that window, with the layout defined
  here in `dango_shm_guest.h` (the region fields mirror Anko's `SharedArenaInfo`:
  id, guest physical base, size, DMA class as a flag). Phase L owns the
  descriptor *writer*; this header is the guest reader's view of it;
- `gpud` assumes the bootstrap profile publishes the scanout region first
  (`QUERY index 0`) and that `GPU_BUFFER_REGISTER.shm_id` becomes the region id
  under the Phase L ABI. Until then `shm_id` is still a staging byte offset, so
  the ids never match and the copy path always runs — the pre-Phase-M behaviour.

<a id="gpu_zerocopy_import-verification-performed"></a>
### Verification performed

- `linux-artifacts/guest-driver/dango_shm.c` compiles against the real v6.12
  kernel tree and links into `vmlinux` (`make drivers/misc/ vmlinux`, container,
  aarch64, kernel `-Werror` flags).
- The whole GPU-profile appliance userspace (`agent` + `channel_test` + `kms_test`
  + `gpud`) links with `-Wall -Wextra -Werror`
  (`scripts/linux/build-agent.sh`, `DANGO_LINUX_PROFILE=gpu`).
- `cmake --build --preset qemu-dev --target anko mochi_system` is green.
- The host ABI regressions pass (`tests/test_gpu_proto.py`, `test_gpu_artifacts.py`,
  `test_channel.py`).
- `scripts/linux/stage-gpu-source.py` now stages both built-in drivers
  (`dango_channel.o`, `dango_shm.o`) from one table.

<a id="gpu_appliance_remaining"></a>
## QEMU appliance integration limits

QEMU's Linux `virtio_gpu` assignment/KMS/channel path is separate from physical
Mali mediation. Recorded checks established Linux DRM binding, KMS patterns and
readable native TTY frames on the assigned GPU. They did not establish full
native Vulkan WSI, dynamic compositor buffers or appliance-death recovery.

The old lavapipe-only transfer executor and reference-session broker have been
removed. Their standalone fill/copy results were not native application
acceptance. Use the [native Vulkan frontend](graphics.md#native-vulkan-frontend)
for the maintained client route. A 512 MiB layout definition does not by itself
establish a complete working QEMU Vulkan appliance.

Remaining acceptance work includes reconnect after appliance death, on-target
assignment revoke/quarantine, broader endpoint transfer, dynamic staging and
native compositor/window interfaces. The foreign DMA-buf import limitation in
virtio-gpu still requires its bounded staging-to-dumb-buffer copy; it says
nothing about the supported phone's direct-scanout path.

<a id="gpu_appliance_guest_memory"></a>
## Guest layout and mapping capacity

The build selects `minimal` or `lavapipe` through the shared
`dango_guest_layout.h` contract. Minimal uses 128 MiB RAM; lavapipe defines
512 MiB and moves fixed channel, staging, shared and MMIO windows above RAM.
Load addresses and real initramfs size must agree with the DTB and manifest.
Changing a service's requested RAM alone does not select a coherent layout.

Aligned RAM and MMIO use 2 MiB stage-2 blocks. A partial split requires 511
additional mapping records and validates capacity before mutation; failed
allocation leaves the original block intact. Whole-range revoke/unmap does
not allocate. Stage-2 regressions independently decode descriptors and map
512 MiB with 256 records; this is a capacity proof, not an appliance boot.

<a id="huawei_p20_lite_dango_plan"></a>
## Phone acceptance and recovery

The original phone port plan is now represented by implemented boundaries in
[architecture](architecture.md#dango-and-linux-appliances). The useful acceptance
sequence remains: establish actual EL2 entry/monitor readiness; package matching
signed guest artifacts and memory layout; require guest execution markers;
exercise at least 512 ordered channel round trips with wraparound and IRQ wake;
then verify ongoing rendering and native shell responsiveness together.

The physical EL2 route is documented in [Anne boot](../boards/huawei-anne/boot.md).
GPU-disabled QEMU channel acceptance does not prove physical Mali execution.
A successful hardware triangle does not prove restart/reconnect, long-run fault
containment or complete application graphics. Retain exact image/readback hashes
and keep private firmware outside the repository. Failed device reset or display
retirement must retain backing until it is safe to release.
