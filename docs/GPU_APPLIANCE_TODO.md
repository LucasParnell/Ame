# Ame GPU Driver Appliance — what remains, per phase

Task list for `AME-GPU-APPLIANCE-1` (v2), grounded in the tree at `master` (post-Phase D +
the Phase E build pipeline). Status legend: **Done** · **Partial** · **Not started**.

## Scope (current decision)

**Target now: the Linux `virtio-gpu` driver appliance** — Phases **E–J**. A real Linux VM owns an assigned
QEMU `virtio-gpu-pci` function behind an Ame SMMUv3 domain, is driven over Dango rings/shared memory, and
is presented through a native Midix GPU service. **The plumbing stays backend-agnostic**: the Midix GPU
ABI, the Dango channel, object lifetime, supervision and the async completion model must not change when a
different Linux GPU driver is dropped in later.

**Deliberately deferred (plumbing-ready, NOT to be started):** `amdgpu` and the physical/RADV work —
Phases **P** (physical backend), **Q** (amdgpu appliance), **R** (RADV), and the physical halves of N/O.
No AMD-specific code, firmware, platform interrupt-remapping, or physical reset logic until the virtio
appliance milestone (Phase J) is proven end to end. Phase E §E.3/§E.4 already carry the plumbing (MSI
interrupt routing, device-memory stage-2, filtered config) that amdgpu would reuse — that is the point of
building it now.

Master reference points: `kernel/iommu/{core,smmuv3,faults}.cc`, `kernel/vm/vm_device.cc`,
`kernel/pci/{enumerate,assignment,msi}.cc`, `shared/include/midix/abi/protocols/{gpu,dango_channel}.h`,
`scripts/linux/build-gpu-linux*.sh`, `linux-artifacts/config/{dango_hello_arm64,dango_gpu_arm64}.fragment`,
`docs/LINUX_APPLIANCE_MINIMAL.md`, `scripts/verify.sh` (gate) + opt-in rows.

| Phase | Status |
|---|---|
| A host PCI + IOMMU inventory | **Done** |
| B generic IOMMU core | **Done** |
| C SMMUv3 backend + hostile DMA | **Done** (22 checks, `iommu-testdev`) |
| D PCI assignment + IRQ virtualization | **Done** (32 checks) |
| E Linux virtio-gpu-pci probe | **Partial** — guest kernel + pipeline exist; INTx-profile ECAM mediation proven; Dango construction + Linux probe remain |
| F standalone Linux KMS | Not started |
| G guest-visible Dango channel | Not started (ABI only) |
| H `dango-gpud` control plane | Not started |
| I shared staging framebuffer | Not started |
| J framebuffer/TTY migration | Not started |
| K lifecycle hardening | Not started |
| L dynamic shared arenas | Not started |
| M zero-copy/import | Not started |
| N remote Vulkan session transport | Not started (ABI only) |
| O Vulkan WSI bootstrap | Not started |
| P physical GPU backend | Deferred (by decision — virtio appliance only) |
| Q amdgpu appliance | Deferred (by decision — virtio appliance only) |
| R RADV backend | Deferred (by decision — virtio appliance only) |
| S zero-copy Vulkan/compositor | Not started |

---

## Phase A — host PCI + IOMMU inventory — Done
Inventory (`PciFunctionInfo`, BARs, requester id), all `virtio,mmio` transports, SMMUv3 DTB parse,
opt-in `AME_QEMU_IOMMU=1` topology + fixtures. *No remaining work.*

## Phase B — generic IOMMU core — Done
`IommuDomain` (now a plain record + `domain_*` free functions), DMA classes, ownership-validated batch
map/unmap, quarantine domain, counters, backend hooks. *No remaining work.*

## Phase C — SMMUv3 backend + hostile DMA — Done
Stream table (default-deny), STEs, CMD/EVT queues, scoped TLBI, fault→quarantine; verified with
`iommu-testdev`. See `DELIVERY/gpu-appliance-phase-c.md`. *No remaining work.*

## Phase D — PCI assignment + IRQ virtualization — Done
`MX_CAP_VM_DEVICE`, secure-attach / DMA-safe-revoke, filtered config model, BAR stage-2 **Device**
mappings, mediated config writes, MSI/MSI-X + INTx level routes → vGIC. See
`DELIVERY/gpu-appliance-phase-d.md`. *No remaining work.*

---

## Phase E — Linux `virtio-gpu-pci` probe — **Partial**

**Done**
- Guest kernel builds: `build/dango-linux-gpu/Image` (48.7 MB) via the Linux container; minimal-config
  checker passes for flow `gpu` (`linux-artifacts/config/dango_gpu_arm64.fragment`).
- Build pipeline: `scripts/linux/build-gpu-linux.sh` (+ `build-gpu-linux-docker.sh`) produces
  Image + agent + initramfs + DTB + manifest; `build-dtb.sh` now takes a template/name; `build-agent.sh`
  builds natively on an aarch64 Linux host.
- GPU guest DTS drafted: `linux-artifacts/dts/dango-linux-gpu.dts.in` (synthetic ECAM PCI host bridge,
  INTx→GIC map, assigned-function MMIO window, channel reserved-memory).

**Ordered substeps**
1. **Mediated guest PCI config — done for the INTx bootstrap profile.**
   `SYS_VM_ASSIGN_EXPOSE_ECAM` reserves one trap-only bus at an ownership-checked IPA.
   Actual AArch64 stage-2 aborts use bounded byte/halfword/word config emulation, fixed BAR
   sizing/restoration, and ACTIVE-domain BME mediation. Only mapped virtio MMIO capabilities
   are published; MSI/MSI-X and indirect PCI config capabilities stay hidden. MSI-X table/PBA
   pages cannot be mapped raw. Host MSI delivery remains a later extension of this profile.
   Proof: sanitizer probes plus `AME_VERIFY_ASSIGN=1` (43 checks, including actual guest
   ECAM loads/stores and BAR removal after the VM has run). See
   [Phase E.1 delivery](../DELIVERY/gpu-appliance-phase-e1.md).
2. **Dango GPU appliance construction.** `userland/dango/manager/main.cc` must, for the GPU profile:
   create the VM, **assign the virtio-gpu-pci function** (`SYS_VM_ASSIGN_DEVICE`), reserve the synthetic ECAM trap
   + map assigned BARs as stage-2 **Device**, install the INTx routes (`SYS_VM_ASSIGN_ROUTE_IRQ`), and only then
   resume the vCPU. Today the manager only creates VM/vCPU/channel and has no PCI knowledge.
3. **Artifact packaging for the GPU profile.** `userland/CMakeLists.txt` hardcodes
   `build/dango-linux` names and the hello fragment; add a GPU artifact dir/flow so `linux/Image`,
   `linux/initramfs.cpio`, `linux/dango-linux.dtb`, `linux/manifest.toml` are the GPU ones, without
   breaking the hello profile. Then `DANGO_BUILD_LINUX_ARTIFACTS=ON` + `AME_QEMU_IOMMU=1`.
4. **vGIC SPI delivery for INTx.** Route host INTx → guest SPI 12–15 and confirm the Linux `virtio_pci`
   ISR fires (extend `kernel/vm/vm_device_irq.cc`).
5. **Unblock the native macOS build gap** (already solved via the container) and re-run end to end:
   boot the appliance and assert Linux binds `virtio_gpu`, `/dev/dri/card0` exists, and IOMMU counters
   show no unexpected faults / no global bypass.

**Acceptance:** Linux binds `virtio_gpu`, creates a DRM primary device, no unexpected IOMMU faults, no
global-bypass mapping.

## Phase F — standalone Linux KMS — Not started

**Remaining**
1. A small static appliance userspace (`linux-artifacts/kms_test/`, raw DRM ioctls — no libdrm) added to
   the initramfs: open `/dev/dri/card0`, `GET_RESOURCES`/`GET_CONNECTOR`, `CREATE_DUMB` + `MAP_DUMB`,
   fill a pattern, `ADD_FB` (+ legacy `SETCRTC` or atomic), then keep it displayed.
2. Host capture: extend `scripts/test/` to `screendump` the QEMU display to PPM and assert the pattern
   (this proves device assignment + MMIO + IRQ + DMA + Linux DRM + `virtio_gpu`, with **no** virtio-gpu
   register logic in Ame).
3. devtmpfs (or explicit mknod) so `/dev/dri/card0` exists; keep the guest minimal
   (`docs/LINUX_APPLIANCE_MINIMAL.md`).

**Acceptance:** the QEMU display shows the Linux-generated pattern; DMA stays inside the appliance domain
under sustained redraw.

## Phase G — guest-visible Dango channel — Not started

**Remaining**
1. Guest ring mapping: map a `VmChannel`'s two rings into the guest at the DTS reserved-memory address;
   keep `VmDmaClass::None`.
2. Host→guest notification as a **virtual IRQ** (reuse the Phase D route mechanism); guest→host via a
   reserved **HVC** range (`HVC_DANGO_CHANNEL_NOTIFY/ARM`) implemented in the EL2 exit path
   (`kernel/arch/arm64/el2.S`, `kernel/vm/vcpu.cc`) — must not collide with PSCI.
3. `linux-artifacts/guest-driver/dango_channel.c` → `/dev/dango-channel0` (mmap both rings, blocking
   read/poll, notify, version/ring-order query). No GPU logic.
4. DTB: channel node + `no-map` region (drafted in `dango-linux-gpu.dts.in`).
5. Proof: ordered bidirectional ping/pong — Ame-side first (Mochi ↔ host endpoint, testable without
   Linux), then Linux ↔ Mochi once E boots.

**Acceptance:** a Linux test process and a Mochi test process exchange ordered ping/pong without UART.

---

## Phase H — `dango-gpud` control plane — Not started
`linux-artifacts/gpud/{main,drm_kms,protocol}.c` implementing `GPU_HELLO`, `GPU_ENUM_OUTPUTS`,
`GPU_GET_MODES`, `GPU_ERROR` over the Phase G channel; a Mochi probe enumerates the virtio-gpu output.

## Phase I — shared staging framebuffer — Not started
Fixed shared staging region in host+guest; `GPU_BUFFER_REGISTER`, async `GPU_PRESENT` /
`GPU_PRESENT_COMPLETE`; `dango-gpud` copies into a DRM dumb buffer. Staging stays non-DMA until
registered `SharedGpu`.

## Phase J — framebuffer / TTY migration — Not started
`userland/drivers/framebuffer/main.cc` gains a `drv.gpu.linux` backend (`FB_PRESENT`→`GPU_PRESENT`),
ramfb kept as fallback. `tty.fb0` reaches the shell through the appliance.

## Phase K — lifecycle hardening — Not started
`boot_generation` invalidation, bounded outstanding-op tables, ring backpressure, failure propagation,
DMA-safe teardown tests, IOMMU-fault quarantine, appliance restart/reconnect.

## Phase L — dynamic shared arenas — Not started
`sys_vm_shared_memory_attach` + host plumbing + `linux-artifacts/guest-driver/dango_shm.c`; explicit
`SharedGpu` registration; buffers registered/unregistered without reboot.

## Phase M — zero-copy / import — Not started
`dango_shm` exposing shared pages via a Linux-importable object (dma-buf/memfd) so the bootstrap memcpy
can be dropped — or a documented reason to keep it. Must not weaken IOMMU ownership/revocation.

## Phase N — remote Vulkan session transport — Not started (ABI only: `protocols/vulkan_session.h` to add)
Native loader + `ame_remote` ICD, per-client session brokerage in `drv.gpu.linux`,
`linux-artifacts/vkd/` running Mesa **lavapipe**. Acceptance: a native test creates a Vulkan
instance/device, allocates host-visible memory, records/submits a command buffer, waits on sync, and
validates an offscreen result — with no Vulkan parsing in Anko and per-session shared rings, not one
RPC per call.

## Phase O — Vulkan WSI bootstrap — Not started
Surface/swapchain integration with a bounded copy into a Dango shared buffer; scanout stays with the
native compositor/`dango-gpud`. A native Vulkan triangle presents through Linux `virtio_gpu`; killing
the appliance yields clean device/session loss.

## Phase P — physical GPU backend — **Deferred (by decision)**

Not in scope until the virtio appliance milestone (Phase J) is proven. The generic `IommuDomain` /
assignment contract stays ready for it.
AMD-Vi/VT-d backends, physical PCI reset quirks, real isolation-group topology, hardware interrupt
remapping; validate the same `IommuDomain` contract on a physical test device.

## Phase Q — amdgpu appliance — **Deferred (by decision)**

Not in scope yet. The `amdgpu` flow exists in the minimal-config checker for when it is picked up, and the
virtio appliance exercises the same PCI ownership / interrupt-routing / mandatory IOMMU contract it will need.
`CONFIG_DRM_AMDGPU` + firmware (`linux-artifacts/config/dango_gpu_amd64`-equivalent fragment, flow
`amdgpu` in the checker), assign the AMD GPU/IOMMU group, boot the same `dango-gpud`+`dango-vkd`.
IOMMU fault tests must prove the GPU cannot address pages outside the appliance domain.

## Phase R — RADV backend — **Deferred (by decision)**

Not in scope yet; lavapipe remains the QEMU Vulkan reference when Phase N lands.
Mesa RADV in the physical appliance selected by `dango-vkd` with no native Vulkan ABI change.

## Phase S — zero-copy Vulkan/compositor — Not started
Swapchain/compositor buffers cross the boundary without CPU readback where safe, preserving ownership,
fence transfer, and IOMMU registration lifetimes.

---

## Cross-cutting

- **Minimality.** The guest is still `defconfig`-derived (~6,250 compile steps, 48.7 MB `Image`). To be
  genuinely stripped, base the profile on `allnoconfig` + an explicit enable set (PL011, GIC, timer,
  initramfs, PCI/virtio/DRM) instead of `defconfig` minus a deny-list. Then `SMP` (arm64 `def_bool y`),
  `CPU_IDLE`, `PERF_EVENTS` are the only leftovers to investigate.
- **Build environment.** macOS cannot build the guest kernel natively (`elf.h`); the Linux container
  (`scripts/linux/build-gpu-linux-docker.sh`, colima) is the supported path and is already set up.
- **Harness.** Each phase's acceptance should become a `scripts/verify.sh` opt-in row (pattern established
  by `AME_VERIFY_IOMMU` / `AME_VERIFY_ASSIGN`).
- **Docs.** Keep `docs/FEATURE_STATUS.md` and `docs/CLEANUP_STATUS.md` current as phases land.

## Immediate next actions (in order)
1. Phase E.2/E.3 — Dango GPU appliance construction + GPU artifact packaging; boot it.
2. Phase E.4/E.5 — vGIC SPI delivery + end-to-end assertion.
3. Phase F — KMS self-test + `screendump` capture.
4. Phase G — channel guest mapping + vIRQ/HVC + `/dev/dango-channel0` + ping/pong.
