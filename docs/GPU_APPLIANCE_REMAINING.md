# GPU appliance — what is left (amdgpu work excluded)

Grounded in the tree at `c986df8` (after Phases A–O). This is the honest list of remaining work for the
**Linux `virtio-gpu` / lavapipe appliance**. The amdgpu phases (**P** physical backend, **Q** amdgpu
appliance, **R** RADV) are deliberately out of scope here — the plumbing stays backend-agnostic for them.

Legend: **blocker** = needs something not in the tree yet · **hardening** = works, should be tightened ·
**polish** = cosmetic/maintainability.

| # | Item | Phase | Kind | Why it is left |
|---|---|---|---|---|
| 1 | Real Vulkan execution | N/O | **blocker** | The session transport is verified against a *reference software backend on the same wire*, not Vulkan. The guest is a no-libc freestanding initramfs, so it cannot `dlopen` the staged lavapipe ICD; the macOS host has no Vulkan. Needs a **libc guest rootfs** + Mesa lavapipe linked into `dango-vkd`, then re-run the N/O acceptance (instance/device, host-visible alloc, record/submit/sync, offscreen validation) and the WSI triangle presenting through `virtio_gpu`. `linux-artifacts/vkd/` + `scripts/linux/build-vkd*.sh` already stage it; the build reports the missing rootfs instead of faking it. |
| 2 | Drop the staging→dumb copy | M | **blocker** | `dango_shm` exports a dma-buf and `gpud` tries `PRIME_FD_TO_HANDLE`, but the import is impossible for `virtio_gpu` (documented in `GPU_ZEROCOPY_IMPORT.md`: import stub returns `-ENODEV`, foreign GEM mis-cast in scanout). The path is also dormant until `GPU_BUFFER_REGISTER.shm_id` carries a **Phase L region id**. Pays off only for a backend that can import (lavapipe/Vulkan, or a physical backend). A one-shot probe prints `TEST FAILED` if the documented assumption stops holding. |
| 3 | Remove the fb.memory→staging mirror | J | **hardening** | `io.framebuffer` renders to its own `fb.memory` and the bootstrap mirrors it into the shared staging window, because a native process cannot map `MX_VM_MEMORY` and the arena cap is 64 pages. Giving the framebuffer a live mapping of the staging window removes the copy. |
| 4 | TTY acceptance row | J | **polish** | There is no screendump row asserting the **tty frame** through the appliance (the `gpud` row asserts the bootstrap 4-bar pattern). Add one. |
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

1→(#1 libc guest rootfs) unlocks 2, 12.
3/4 are small and improve the J milestone's evidence.
5/6/7/8/9 are hardening that does not block anything.
10–14 are polish.

## Explicitly out of scope here

Phases **P** (physical GPU backend), **Q** (amdgpu appliance) and **R** (RADV). The generic
`IommuDomain`/assignment contract, the Midix GPU ABI, the Dango channel and the session transport were
kept backend-agnostic so they can be reused unchanged when that work starts.
