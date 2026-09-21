# GPU appliance — what is left (amdgpu work excluded)

Grounded in the tree at `c986df8` (after Phases A–O). This is the honest list of remaining work for the
**Linux `virtio-gpu` / lavapipe appliance**. The amdgpu phases (**P** physical backend, **Q** amdgpu
appliance, **R** RADV) are deliberately out of scope here — the plumbing stays backend-agnostic for them.

Legend: **blocker** = needs something not in the tree yet · **hardening** = works, should be tightened ·
**polish** = cosmetic/maintainability.

| # | Item | Phase | Kind | Why it is left |
|---|---|---|---|---|
| 1 | Real Vulkan execution | N/O | **partial; integration blocker** | A libc/lavapipe rootfs now builds on macOS via Docker, and the real transfer executor passes in a container chroot and standalone Linux (`AME_VERIFY_VULKAN=1`). The native client/broker tests still use the reference backend. Remaining: fit the rootfs into Dango's memory/load layout, bind per-session arenas through the manager and guest channel, and implement graphics/WSI before the native triangle acceptance. See the execution notes below. |
| 2 | Drop the staging→dumb copy | M | **blocker** | `dango_shm` exports a dma-buf and `gpud` tries `PRIME_FD_TO_HANDLE`, but the import is impossible for `virtio_gpu` (documented in `GPU_ZEROCOPY_IMPORT.md`: import stub returns `-ENODEV`, foreign GEM mis-cast in scanout). The path is also dormant until `GPU_BUFFER_REGISTER.shm_id` carries a **Phase L region id**. Pays off only for a backend that can import (lavapipe/Vulkan, or a physical backend). A one-shot probe prints `TEST FAILED` if the documented assumption stops holding. |
| 3 | Remove the fb.memory→staging mirror | J | **hardening** | The J acceptance is **met** (`AME_VERIFY_FB_TTY=1` passes and decodes readable tty text from the appliance frame), but `io.framebuffer` still renders into its own `fb.memory` and the manager mirrors it into the staging window on every present. Rendering straight into a shared arena removes the copy (see #10). |
| 4 | TTY acceptance row | J | **done** | `scripts/test/fb-tty.py` (`AME_VERIFY_FB_TTY=1`) boots the appliance, asserts `gpu service: frame staged` and decodes the captured frame cell-by-cell against the console font to prove readable tty text reached the appliance. |
| 5 | `drv.gpu.linux` as a real service | J | **hardening** | Spec §15.1 wants its own manifest service; the bootstrap hosts the service inside `dango.manager` because the appliance channel endpoints/caps cannot be passed between native services yet. Needs cross-service cap passing (or `init` granting the channel endpoints directly). |
| 6 | Assignment death path on-target | K | **hardening** | The quarantine/DMA-safe teardown is covered host-side; exercising the SMMU assignment death path needs the opt-in testdev topology (`AME_VERIFY_ASSIGN=1`). Add the on-target test. |
| 7 | Latent MSI route (`host_irq=0`) | D/K | **hardening** | MSI routes are created with `host_irq = 0` and never installed; the INTx-only profile drops MSI caps so it is latent. Either wire host MSI lines or return a phase-named `E_NOTIMPL` with a failing test. |
| 8 | Appliance restart/reconnect end-to-end | K | **hardening** | Generation/stale-handle logic exists and is unit-tested; a real "kill Linux → reconnect" acceptance run is not yet a gate row. |
| 9 | Reconcile `dango_shm` ownership | L/M | **hardening** | L writes the descriptor, M reads it; both authored the guest driver (M's superset landed). Confirm one writer per field and that the layout matches the writer. |
| 10 | Staging on a dynamic arena | L | **hardening** | The bootstrap staging is a fixed window; moving it to a Phase L dynamic arena is what makes #2/#3 natural. |
| 11 | Guest minimality | cross-cutting | **polish** | The guest is `defconfig`-derived (~6,250 steps, ~48 MB `Image`). Base on `allnoconfig` + an explicit enable set (PL011, GIC, timer, initramfs, PCI/virtio/DRM). |
| 12 | Phase S — zero-copy Vulkan/compositor | S | **blocker** | Swapchain/compositor buffers crossing the boundary without CPU readback where safe, preserving ownership, fence transfer and IOMMU registration lifetimes. Depends on #1. |
| 13 | CI / provenance / release | cross-cutting | **polish** | No CI; a provenance gate exists (`generated/provenance.json`). Release signing needs a key kept outside the repo (`scripts/make-release-key.sh`); the no-key release fails closed by design. |
| 14 | Docs currency | cross-cutting | **polish** | `docs/FEATURE_STATUS.md` and `docs/CLEANUP_STATUS.md` should be refreshed per landed phase; `docs/GPU_APPLIANCE_TODO.md`'s status table is updated alongside this note. |

## Suggested order

1→(#1 appliance integration of the libc guest rootfs) unlocks 2, 12.
3/4 are small and improve the J milestone's evidence.
5/6/7/8/9 are hardening that does not block anything.
10–14 are polish.

## Real Vulkan execution progress (2026-09-21)

`scripts/linux/build-vkd.sh` now builds the libc `dango-vkd`, packages the AArch64
ELF dependency closure (loader, libc, Vulkan loader, lavapipe and its dependencies),
and writes `build/dango-vkd/initramfs.cpio.gz`. On macOS it delegates to the arm64
Docker builder. The compiler and validation layer stay in the build container;
`rootfs-manifest.json` records package versions and shipped ELF hashes.

The actual lavapipe executor creates a device and a preallocated host-coherent
arena buffer, snapshots and validates command records, executes ordered fill/copy
commands, waits for a fence, and verifies readback. The regression covers malformed
sizes, overflow, alignment, command-region access, overlapping copies, command
quota, repeated submits, failed ICD initialization and teardown/reinitialization.
Graphics/compute commands are refused; WSI is not advertised and cannot claim a
copy that did not happen. Wire decoding and arena bounds live in the shared
C/C++ `midix/abi/vulkan_commands.h` header. No Vulkan parsing was added to Anko.

Run:

```sh
scripts/linux/build-vkd.sh
python3 scripts/test/vulkan-executor.py --output out/validation/vulkan-executor
AME_VERIFY_VULKAN=1 scripts/verify.sh
```

This proves **standalone Linux executor execution**, not native N/O acceptance.
The remaining integration is larger than adding libc:

- The measured Ubuntu closure is about 186 MiB unpacked / 58 MiB compressed.
  Its standalone acceptance uses 512 MiB RAM. Dango currently has 128 MiB RAM,
  a 16 MiB initramfs load window and bounded page-mapping metadata. The rootfs
  is deliberately built separately; expanding the VM requires reconciling these
  bounds and the fixed channel/shared-memory guest addresses, or reducing the
  runtime footprint. Simply changing the manifest RAM size is insufficient.
- `dango.manager` still initializes `VulkanBackend{}`. It needs the actual
  per-session arena/channel bridge and channel ownership coordinated with `gpud`.
  The standalone executor probe does not exercise that bridge.
- The executor implements transfer commands only. Real graphics pipelines,
  swapchain image transfer into the compositor/`gpud` path, a native triangle
  screendump and appliance-death recovery remain acceptance requirements.

## Explicitly out of scope here

Phases **P** (physical GPU backend), **Q** (amdgpu appliance) and **R** (RADV). The generic
`IommuDomain`/assignment contract, the Midix GPU ABI, the Dango channel and the session transport were
kept backend-agnostic so they can be reused unchanged when that work starts.

## Render backends: software (here) vs virtio-gl (Linux)

Both are the QEMU *paravirtual* path; the choice is host policy, not a fork.

- **software** (default, `AME_GPU_RENDER=software`) — plain `virtio-gpu-pci`, the guest renders with a
  guest-side ICD (Mesa lavapipe). Works anywhere, including macOS. This is what the tree runs today.
- **virtio-gl** (`AME_GPU_RENDER=virtio-gl`) — `virtio-gpu-gl-pci` with `venus=on`: the guest's Mesa
  venus/virgl driver forwards the render stream and the **host** GPU executes it. Host-side selection is
  implemented (`scripts/test/ef_constants.py`, and the harness derives the device string from it); it
  errors explicitly on macOS rather than falling back, because a Mac cannot instantiate the device.

What the virtio-gl path still needs **on the Linux machine** (not implementable here):

1. Host QEMU built with **virglrenderer** (venus), plus a GL/egl-headless display backend and a DRM render
   node; the harness should add `-display egl-headless` / `-vga none` (or `virtio-vga-gl`) when the render
   backend is virtio-gl, and probe `virglrenderer` availability so a missing feature is a clear error.
2. Guest side: stage Mesa's **venus** ICD (`virtio_gpu`/venus) in the guest rootfs alongside lavapipe, and
   have `dango-vkd` pick the ICD from the render backend (venus when virtio-gl, lavapipe when software).
   The rootfs staging script already resolves an ICD closure; this is a second ICD entry plus a selector.
3. Re-run the acceptance on that host to prove the venus path end to end (the same rows, same markers).

The backend-agnostic layers (session transport, brokerage, arena lifecycle, WSI, lifecycle hardening) are
unchanged by the choice — that is the point of keeping the vendor out of the ABI.
