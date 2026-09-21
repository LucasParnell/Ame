# Phase M — zero-copy / import: verdict and design

Spec: AME GPU appliance §12.3 ("expose a Dango shared region to Linux as a
Linux-importable object … or a documented reason to keep the [bootstrap] memcpy;
must not weaken IOMMU ownership/revocation").

## The question

`dango-gpud` renders the bootstrap frame into the Dango shared staging window and
then **copies** it into a DRM dumb buffer (`gpud_present_finish()` in
`linux-artifacts/gpud/gpud_main.c`, one `memcpy` per row). Can that copy be
dropped by exposing the region to Linux as a dma-buf (or memfd) and importing it
into the assigned `virtio_gpu` as the scanout object?

## Verdict: no, for `virtio_gpu`. The bounded copy stays.

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

## What was still implemented (useful, and required by any future backend)

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

## The test that fails if the documented assumption stops holding

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

## The IOMMU rule (not weakened)

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

## Phase L interface

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

## Verification performed

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
