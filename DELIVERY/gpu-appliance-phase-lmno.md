# Phases L, M, N, O

Base `9df8560`. Commits `98d4849` (L), `58448be` (M), `3b3d6cd` (N/O).

P/Q/R were skipped: they are the amdgpu/physical work, out of scope per the request.

## Phase L — dynamic shared arenas (done)

`SYS_VM_SHARED_MEMORY_ATTACH` attaches a pinned arena to a VM at a guest PA, plus
`..._REGISTER_DMA` / `..._INSPECT`. The attachment object retains the **host arena and the guest mapping
lifetimes together**; a fixed-capacity pool bounds attachments; the region enters stage 2 as
`VmDmaClass::None`, and a single writer (`shared_arena_register_dma`) is the only path to `SharedGpu`.
The guest gains the `dango_shm` built-in driver and a single-sourced DT window. A host probe compiles the
real kernel sources and proves an **unregistered** region is refused by a real IOMMU domain while a
registered one maps. Opt-in row `AME_VERIFY_SHARED=1`.

## Phase M — zero-copy / import (attempted; documented verdict: keep the copy)

`dango_shm` exports a registered region as a dma-buf (guest PA == DMA address, per the platform SMMU
convention) and `gpud` tries `PRIME_FD_TO_HANDLE` before falling back to the dumb-buffer copy. Verified
against the appliance's own Linux v6.12 tree that the import **cannot** work for `virtio_gpu`
independent of our exporter: `virtgpu_prime.c` returns `ERR_PTR(-ENODEV)` from its import-sg-table stub,
it only short-circuits its own exports, scanout mis-casts a foreign GEM, and `DRM_CAP_PRIME` cannot gate
it. Written up in `docs/GPU_ZEROCOPY_IMPORT.md`. A one-shot probe prints `TEST FAILED` (a word every
acceptance row checks) if that assumption ever stops holding, so the documented reason is itself tested.
Export fails closed unless the region is `SharedGpu`-flagged.

## Phases N/O — remote Vulkan session transport + WSI (transport done; real Vulkan staged, not run)

- `protocols/vulkan_session.h` — versioned, bounded, endian-asserted wire ABI (C and C++ probes).
- Per-client **session brokerage** with quotas (ring size, shared memory, object count, outstanding
  requests) and generation-scoped object ids; no cross-session object access.
- Native `ame_remote` client + command recorder and the WSI path (bounded copy into a Dango shared
  buffer; scanout stays with the native compositor / `dango-gpud`).
- The transport is verified end to end — instance/session open, host-visible allocation, record, submit,
  sync, validate the offscreen result, and a WSI triangle rasterised and copied — **against a reference
  software backend speaking the same wire protocol**, not real Vulkan: the host is macOS (no Vulkan) and
  the guest is a no-libc freestanding initramfs that cannot `dlopen` the staged lavapipe ICD.
- `linux-artifacts/vkd/` + `scripts/linux/build-vkd*.sh` stage a `dango-vkd` and the lavapipe artifacts;
  running them needs a libc guest rootfs, which the build script **reports instead of faking**.

## Verification

- `scripts/verify.sh` — **all checks passed**.
- `ctest --test-dir out/verify` — **100% tests passed** (includes the new shared-arena and Vulkan probes).
- `dango-lifecycle` — `{"success": true, "reason": "iommu lifecycle probe ok"}`.

## Open items

- N/O real execution: needs a libc guest rootfs (+ Mesa lavapipe) — the single blocker.
- M: the import path stays dormant until `GPU_BUFFER_REGISTER.shm_id` carries a region id, so behaviour
  is exactly pre-M today (the copy runs).
- L/M both wrote to the guest `dango_shm` driver; the combined file is the superset and builds, but the
  descriptor layout ownership (L writes, M reads) is worth a second pair of eyes.
