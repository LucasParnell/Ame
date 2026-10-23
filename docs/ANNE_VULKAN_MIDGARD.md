# Experimental PanVK Midgard port for ANE

Implementation status, 23 October 2026: **first hardware frame rendered,
bring-up still incomplete**. There is a reproducible Mesa 24.3.4 v5 ICD, a real
Vulkan triangle client, and a native `diag gpu vulkan` transport. The physical
T830 now produces a verified triangle. It is not a complete translation of every
removed path and is not conformant Vulkan.

Four hardware runs have happened, each flashed with a backup and complete
partition readback verification, and each booting Ame with EL2 on all eight CPUs.

**Run 1 (v1)** failed during physical-device enumeration, before any GPU work.
The failure reproduced locally with a zero-timestamp ICD: Mesa 24.3.4's
`get_cache_uuid()` rejected the deterministic initramfs file timestamp, so
`vkEnumeratePhysicalDevices` returned `VK_ERROR_INITIALIZATION_FAILED` (-3).
The forward port now hashes the linked ELF build identifier, the GPU family and a
format tag instead. The shader/recording regression includes an epoch-timestamp
ICD and passes.

**Run 2 (v2)** confirmed that fix on hardware and advanced the failure to GPU
submission. The enumeration error and the "bogus" cache message are both gone,
and panvk now reaches the non-conformance warning that follows a successful
`get_cache_uuid()`. The triangle still produced no frame: the kernel reported
`js=1` and `js=0` both failing with `DATA_INVALID_FAULT` at
`head == tail` (`0xa11c580`, `0xa11c680`), and `diag gpu vulkan` returned
`status=-1 frames=0 checksum=0 display_restored=0`. This reproduced identically in
two independent flashes. GLES is unaffected: `diag gpu` rendered 296 frames with
`display restored` after the fault in the same boot, so the GPU and MMU recover
and the working baseline is intact.

The v2 package was verified before flashing: `out/panvk-v2-verify/REPORT.md`
disassembles the ramdisk `libvulkan_panfrost.so` and shows the build-ID SHA-1 path
with the `panvk-cache-v1` tag in `get_cache_uuid`, while the v1 control still
writes raw `stat64().st_mtime`. The third image,
`out/anne-panvk-experimental-v3/`, adds `PANVK_DEBUG=startup,trace` and
`PANDECODE_DUMP_FILE=stderr` so the failing job chain is decoded to the console;
it was flashed and produced that decode, summarised below. Per-run records are in
`out/mesa-panvk-midgard/phone-v1/`, `phone-v2/`, `phone-v2b/` and `phone-v3/`.

The original installed kernel is preserved at
`out/anne-device-private/kernel-install-20261023T123557Z/kernel-before.img`,
SHA-256 `32db827fc96c991c6e8242b959eaaeaffaac34b5a809160b4bd77f4fa8ca10fe`.
The flash/readback record is in the same directory. First-run logs are under
`out/mesa-panvk-midgard/phone-v1/`; the validation summary separates flash/boot
success from the missing T830 rendering evidence. Further flashes and tests
are authorized; retries still require physical entry into BootROM mode.

## Reproduce the build

```sh
# Supplies the pinned cross compiler dependencies and working GLES baseline.
scripts/linux/build-panfrost-mesa.sh
# Separate source, build, and install under out/mesa-panvk-midgard/.
scripts/linux/build-panvk-mesa.sh
scripts/test/panvk-midgard.sh

# Separate guest artifact directory; leaves the default GLES artifacts alone.
DANGO_PANVK_PREFIX="$PWD/out/mesa-panvk-midgard/install" \
DANGO_LINUX_OUT="$PWD/out/dango-linux-panvk" \
    scripts/linux/build-panfrost-renderer.sh
```

The Vulkan builder checks the Mesa archive SHA-256, applies the maintained
patch series in `third_party/mesa/panvk-midgard/` in order, records source hashes,
and builds the ICD with assertions enabled. `0001` is the forward port; `0002` is
the v5 tagged-framebuffer-pointer fix that produced the first hardware frame.
Applying both to a pristine 24.3.4 extract reproduces the built tree byte for
byte. The patch targets the **pinned 24.3.4 API**, rather than
claiming to target current Mesa main. `DANGO_PANVK_BUILD` and
`DANGO_PANVK_PREFIX` select alternate build/install directories. A changed patch
requires a clean source directory or an explicit rebase. The renderer also
needs `glslangValidator` and `spirv-val`; SPIR-V targets Vulkan 1.0.

For a complete appliance, copy the existing matching Panfrost `Image`,
`linux.config`, and `dango-linux.dtb` into the separate guest directory, run
`update-initrd-dtb.py`, then regenerate its manifest with
`DANGO_LINUX_PROFILE=panfrost`, `DANGO_PANFROST_RENDERER=gles`, and
`DANGO_PANFROST_HARDWARE=1`. Configure a separate native build with profile
`huawei-anne-gpu` and point `DANGO_LINUX_ARTIFACT_DIR` at that directory. The
manifest retains `renderer=gles` because this is a combined guest renderer;
`mesa-runtime.json` separately records `experimental_panvk=true` and the ICD
hash. No DRM shim or host software Vulkan driver enters the phone rootfs.

## Implemented paths and fixes

- v5 build, dispatch, physical-device selection, queue and device entrypoints.
  Hardware exposure requires T830, `PANVK_MIDGARD_EXPERIMENTAL=1`, and Mesa's
  existing experimental-driver opt-in. Optional Vulkan features/extensions are
  disabled for v5; its API version is capped at 1.0.
- Modern JM vertex/tiler/fragment jobs, MFBD-backed graphics thread storage,
  late polygon-list allocation, and T830's nonhierarchical tiler quirk.
  Clear-only lists remain CPU-mapped and receive the required terminator;
  draw batches receive a dependency-tracked WRITE_VALUE job. Mutable MFBD
  tiler state is restored before resubmission.
- Midgard shader compilation, separate fragment-coordinate/point-coordinate/
  face varying descriptors, and vertex/instance ID attributes. Varying
  allocations count the descriptors actually emitted, including inactive
  entries, rather than just counting active slots.
- Push constants and system values lower to a reserved UBO. The intrinsic base
  contributes to the actual load address. Each draw receives a new UBO table,
  preventing later pushes from overwriting a previous draw's binding.
- v5 texture headers and payloads use pointer tables for image and buffer views.
  Ordinary descriptor copies use mapped descriptor-set contents; v5 does not
  expose update-after-bind. Buffer-view allocation failure is checked before
  packing descriptors.
- Kernel submission/synchronization failures return device loss instead of
  relying on assertions. The invalid CPU index scan is excluded from v5;
  indexed and indirect draws/dispatches report an unsupported operation.
  Allocation and blend failures are propagated from draw preparation.

## Triangle and presentation

`linux-artifacts/panvk-renderer/` implements a Vulkan 1.0 graphics pipeline with
SPIR-V vertex/fragment shaders, an immutable VBO, an angle push constant, and a
linear RGBA8 color attachment. Its geometry, rotation speeds, colors, clear
color, and perspective match Ame's native GLES diagnostic. The vertex shader
converts GLES clip depth to Vulkan's `[0,w]` convention.

The client records layout/access barriers, waits for a fence, invalidates
mapped image memory, observes the returned row pitch, and converts completed
pixels to native ARGB. Static red/green/blue interior probes and a background
probe reject clear-only or unchanged output. The standalone test also requires
animation checksums to change.

The native command sends a bounded angle record through the existing graphics
transport. The guest initializes the ICD lazily, verifies a static frame before
presenting, copies completed pixels into alternating shared targets inside
DMA-BUF CPU-write ownership brackets, and lets the existing manager/display
handshake retire buffers. The native command requests explicit restoration at
the end. Switching back from GLES retires the display before Vulkan reuses a
target. This is an offscreen render plus CPU-copy presentation path, not a
Vulkan swapchain or direct Vulkan DMA-BUF render target.

After booting the experimental image:

```text
diag gpu vulkan
```

The ordinary `diag gpu` command still uses GLES. A build without the optional
ICD rejects the Vulkan request.

Run the flash and both diagnostics in one step with:

```sh
.venv/bin/python3 scripts/anne/panvk-gate.py
```

It verifies the package hash against the approved image, waits for BootROM or a
verified RAM LK without a deadline by default, flashes with a kernel backup and
full readback, then drives `diag gpu vulkan` followed by `diag gpu`. It exits 0
only when Vulkan executed GPU work, restored the display, and GLES still
rendered. `--test-only` re-tests an already-booted handset, `--check` verifies
inputs without USB access, and `--wait-seconds N` bounds the BootROM wait. GLES
is attempted even after a Vulkan fault, because whether the baseline survived is
the result that matters most during bring-up.

## Validation completed

- Fresh extraction of the SHA-256-pinned Mesa archive: all ICD architectures
  built and linked successfully, including v5. The generated patch also passes
  reverse-application checks on the development source.
- AArch64 client and guest renderer compile with `-Wall -Wextra -Werror`.
  The ordinary GLES-only guest renderer also builds.
- Under QEMU user emulation and the T830 DRM shim: both SPIR-V stages compile
  into a graphics pipeline and the complete triangle command buffer records.
  The shim executes no GPU work. Its unsupported sync-file export produces
  device loss on submission and is not reported as successful rendering.
- Host AMD RX 9070 XT: Vulkan client renders the static probes correctly
  (`27c9a375` in this environment), animates for five seconds, and produces no
  Khronos validation-layer errors. This establishes client behavior on that
  host driver; it does not establish T830 driver correctness.
- Relevant native GLES, appliance and shell diagnostic tests: 14 passed.
- Native shell and manager compile; a separate complete native image builds.
- Offline re-run after the v5 framebuffer-pointer fix: SPIR-V compile, command
  recording and the zero-timestamp ICD regression all pass, and the epoch ICD
  still completes recording.
- **Physical T830:** four flashed runs with backup and full readback each time.
  Run 4 rendered the static triangle with pixel probes verified
  (`checksum=355e1a23`), `display_restored=1`, and no GPU or MMU fault, followed
  by 296 GLES frames with the display restored. No CTS result exists.

Logs and development source: `out/mesa-panvk-midgard/`; per-run device records
are in `phone-v1/` through `phone-v4/`. A fresh-source build was checked under
`out/panvk-clean/`; the host image is `out/panvk-host/triangle.ppm`.

## Coverage of the removal commit

Every changed file in the removal has a mapping below. A mapping does not mean
that the corresponding behavior has passed a hardware test.

| Removed file | Modern location / status |
|---|---|
| `meson.build` | v5 target, entrypoint generation and dedicated Midgard files restored. |
| `panvk_device.c` | `panvk_physical_device.c`, `panvk_macros.h`, and JM queue/device selection. |
| `panvk_private.h` | Modern split device, command, shader and descriptor headers. |
| `panvk_vX_cmd_buffer.c` | JM command buffer/draw paths; polygon lists, MFBD/TLS and reissue state ported. |
| `panvk_vX_cmd_buffer.h` | Modern JM batch state. Single-layer bring-up; layered rendering remains rejected. |
| `panvk_vX_cs.c` | Job packing moved into JM draw/dispatch; special varying and ID formats restored. |
| `panvk_vX_descriptor_set.c` | Modern descriptors retained; v5 mapped copies and texture pointer table added. Storage-image copying remains rejected. |
| `panvk_vX_device.c` | Modern per-architecture device implementation builds for v5. |
| `panvk_vX_image.c` | `panvk_vX_image_view.c` and buffer views restore v5 header/payload layout. |
| `panvk_vX_meta_blit.c` | Modern common Vulkan meta path exists, but full v5 blit correctness is unvalidated. |
| `panvk_vX_meta_clear.c` | MFBD render-pass clear used by triangle; general clear/meta paths remain unvalidated. |
| `panvk_vX_meta_copy.c` | Modern common Vulkan meta paths; full image/buffer/depth copy coverage remains unfinished. |
| `panvk_vX_pipeline.c` | Modern shader objects and draw-time state replace old pipeline packing; opaque fixed-function blend only. |
| `panvk_vX_shader.c` | Modern NIR/compiler path, sysvals and UBO lowering; old fragment blend/type-conversion lowering is still unfinished. |
| `panvk_varyings.h` | Shader link and JM special-buffer packing. |

## Work still required

Framebuffer LOAD/preload rendering is explicitly rejected instead of using
Bifrost pre/post-frame shader descriptors. Shader-based blending, general
render-target type conversion, storage-image descriptors, GPU indexed/indirect
preprocessing, layered rendering, broad copy/blit/resolve behavior, queries,
and wider shader/compiler correctness still need implementation and tests.
Some common PanVK code retains upstream limitations. No claim is made that
all unsupported Vulkan operations are comprehensively rejected yet.

## Fault history: DATA_INVALID on both job slots (resolved)

This section records how the submission fault was found, because the dead ends
are as useful as the answer. It is resolved by the fix described below.

The v2 run narrowed the failure to job submission. `js=1` and `js=0` both fault with
`DATA_INVALID_FAULT` and `head == tail`, so the chain was built, submitted and
rejected by the hardware rather than executed. The cause is **not established**.
Two observations were checked and did not survive scrutiny, and are recorded so
they are not re-investigated:

- The retained decode of an earlier host run (`pandecode.dump.ctx-0.0000`,
  13:17, pre-dating the cache-UUID fix and a prior patch revision) prints
  "Unexpected framebuffer pointer settings" on the tiler job. That warning fires
  when the framebuffer tag disagrees with the decoder's expectation, but the tag
  is constructed correctly for this path: `pan_emit_fbd` returns only the low tag
  bits and the caller ORs in the descriptor address, with the XML supplying
  `Type` as a default. Nothing in the current source sets `cfg.pointer`, and it
  should not: the address comes from the OR. This is therefore not evidence of a
  defect and the dump is stale relative to the flashed build.
- The fragment job packs `header.index = 1` before `pan_jc_add_job` assigns its
  own index, and the stale dump shows index 1 on both the vertex and fragment
  jobs. This is not a collision: the fragment job joins `batch->frag_jc` and the
  vertex/tiler jobs join `batch->vtc_jc`, which are separate chains with separate
  index spaces.

**Run 3 (v3)** enabled `PANVK_DEBUG=startup,trace` with `PANDECODE_DUMP_FILE=stderr`
and produced the decode of the chain that actually faulted. The same
`DATA_INVALID_FAULT` reproduced, GLES again rendered afterwards (295 frames), and
the console captured all four job headers with their exception status and fault
pointer:

| Job | GPU VA | Index | Deps | Exception status | Fault pointer |
|---|---|---|---|---|---|
| Write value (zeroed polygon list) | `a11c640` | 2 | 0 / 0 | `1` (complete) | `0` |
| Vertex | `a11c4c0` | 1 | 0 / 0 | `1` (complete) | `0` |
| Tiler | `a11c580` | 3 | 1 / 2 | `0x58` = 88 | `a11c040` (framebuffer) |
| Fragment | `a11c680` | 1 | 0 / 0 | `0x30258` = 197208 | `a208200` (polygon list body) |

The chain itself is well formed: the write-value job points at the polygon list it
zeroes (`a200000`), the tiler depends on the vertex job and the write-value job
exactly as intended, and the vertex and write-value jobs both report complete. The
failure is inside the tiler job, whose fault pointer is the framebuffer
descriptor, and the fragment job then reports the polygon list body. That
ordering matters: the fragment job's fault is consistent with a tiler job that
never populated the polygon list, so the fragment fault is most likely a
consequence, not an independent defect.

Two decoded values were checked and are **not** defects, recorded so they are not
re-investigated:

- `WLS Instances: 2147483648` on the tiler's local storage is the
  `MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM` sentinel printed through a `log2` field,
  not a wild value.
- `TLS Size: 0` with `Varying count: 1` is consistent with an unallocated
  thread-local stack; the fragment varying here is a descriptor, and this build
  sets `batch->tls = batch->fb.desc` on v5 so no separate TLS block is expected.

### Root cause and fix

On v5 the `Draw` descriptor aliases two fields onto the same bits: `Thread
Storage` and `FBD` are both declared at `start="28:0"` in `genxml/v5.xml`.
`pan_jc` therefore needs the *tagged* `FRAMEBUFFER_POINTER` in that slot, exactly
as the Gallium v5 path does in `pan_jm.c`, which packs the pointer and its
`render_target_count` tag before storing it.

PanVK instead assigned the raw descriptor address:

```c
batch->tls = batch->fb.desc;   /* untagged */
```

so the tiler read an untagged framebuffer pointer, which is what the decoder
flagged as "Unexpected framebuffer pointer settings" and what the tiler job
reported as a fault pointer into the framebuffer with `DATA_INVALID`.

The fix is kept as `0002-panvk-v5-tagged-framebuffer-pointer.patch` so a clean
checkout reproduces it. It packs the tag and ORs it into the address, leaving the
CPU-side pointer usable:

```c
struct mali_framebuffer_pointer_packed tag;
pan_pack(tag.opaque, FRAMEBUFFER_POINTER, cfg) {
   cfg.zs_crc_extension_present = false;
   cfg.render_target_count = 1;
}
batch->tls.cpu = batch->fb.desc.cpu;
batch->tls.gpu = batch->fb.desc.gpu | tag.opaque[0];
```

**Run 4 (v4)** carried that fix in `out/anne-panvk-experimental-v4/` (kernel
SHA-256 `d23db0ce02172c36c4d3f667b4c4a84b31b35f77af7cc5cb49b76da3645291cf`) and
**rendered on the physical T830**. The device console shows:

```text
panvk: T830 static triangle pixels verified checksum=355e1a23
GPU: Vulkan status=0 frames=1 checksum=355e1a23 display_restored=1
GPU: Mali-T830 frames=296 ... checksum=c2ee8283; display restored
```

The whole boot contained no `js fault`, no `DATA_INVALID`, no MMU fault and no
GPU quarantine. The trace confirms the tag landed: the tiler job's decoded
framebuffer reads `Framebuffer: 0xa11c041`, with the low tag bit set.

The pixel result is not a clear-only artifact: the client's
`panvk_triangle_verify_pixels` requires a red probe, a green probe and a blue
probe to each dominate their channel by more than 40 counts, plus a near-black
background, at fixed coordinates inside a 512x512 target. GLES then rendered 296
frames with `display restored` in the same boot, so Vulkan and GLES coexist and
the display is handed back.

### The `frames=1` result was serial starvation, not a driver fault

v4 reported one frame, and the first reading of that was a failed second submit.
That reading was wrong. `status=0` proves the submission path never failed,
because every failure returns a non-zero status, so the do-while loop must have
exited on its five-second deadline rather than on `break`.

The cause was the `PANVK_DEBUG=trace` instrumentation added for the fault hunt.
It decodes every submitted job chain to unbuffered `stderr`, and the console is a
115200 baud serial line. The v4 run emitted 49521 bytes of decode, which is about
4.3 seconds of pure serial time, and the client's first present renders two
chains: the static verification frame and the frame that is copied out. Two
chains of decode therefore consumed essentially the whole five-second budget, so
the loop completed exactly one iteration.

**Run 5 (v5)** removed `trace` from the shipped path and added named failure
reporting to every silent `-1` return in `vulkan_diag.h` (request validation,
display retire, ICD creation, DMA-BUF export, static frame, pixel probe,
animated frame, and both DMA-BUF sync brackets). Kernel SHA-256
`5c04c2d8c69d6edcca7705c5f543d219161d9fc2857d5acd9eea4f7a727765a5`. It shows:

```text
panvk: T830 static triangle pixels verified checksum=355e1a23
GPU: Vulkan status=0 frames=37 checksum=85396c08 display_restored=1
GPU: Mali-T830 frames=296 ...; display restored
```

37 frames in five seconds, a checksum distinct from the static frame, no fault
markers, and GLES intact. Three consecutive Vulkan/GLES transition rounds then
gave 38, 37 and 37 Vulkan frames with distinct checksums (`f0496c56`,
`d4e2fe40`, and the initial `85396c08`) and 296-297 GLES frames each, with no
fault markers in the kernel log afterwards. So the animation runs, the display is
handed back and forth repeatedly, and the working GLES baseline is never
disturbed.

Frame rate is bounded by this path being an offscreen render plus a CPU copy into
shared targets, not by the GPU: `diag gpu vulkan` is a bring-up diagnostic, not a
swapchain. This is still not a conformance result.

The next acceptance gate is the full five-second animation rather than one static
proof frame, then repeated Vulkan/GLES transitions, memory-bound and
descriptor-heavy workloads, and the general copy/blit/resolve and blending paths
listed above. Vulkan CTS work is still required before treating this as general
Vulkan support.

---

## Original source assessment

Earlier source audit, 2026-10-23, before the implementation below. Its
statements about no builds refer to that earlier assessment.

## Finding

Old PanVK did implement Midgard v5 paths. Mesa 22.1.0 builds the Vulkan
driver for architectures 5, 6, and 7; its shared GPU model table includes
T830 (`0x830`). This was experimental code, not a conformant Vulkan driver.
It is a credible starting point for a limited hardware rendering experiment.

The upstream removal is commit
[`c0d65398279db02f68b5acc23c3afbf1c34a5204`](https://gitlab.freedesktop.org/mesa/mesa/-/commit/c0d65398279db02f68b5acc23c3afbf1c34a5204),
authored 2022-06-07 and committed 2022-06-08. Its parent is
`3684776f793a4c516cdb6bd85b20933542b5ebb3`. It changes 15 files, adding 43
and deleting 576 lines. The deletion is useful as a map of Midgard-specific
behavior to recover; reversing it is not a complete implementation strategy.

## Sources actually checked

- [Mesa 22.1.0 release archive](https://archive.mesa3d.org/older-versions/22.x/mesa-22.1.0.tar.xz), extracted locally.
- The complete upstream removal patch, obtained directly from GitLab.
- Ame's existing Mesa 24.3.4 source and build script. Vulkan is disabled in
  that production build.
- Current upstream PanVK build, dispatch, physical-device, shader, JM draw,
  image-view, and descriptor-set sources. The fetched upstream VERSION is
  `27.0.0-devel`. These are a dated main-branch snapshot, not a release or
  an immutable commit pin; downloaded file hashes are retained locally.

Artifacts live under `out/panvk-midgard-research/`. The existing production
source under `out/mesa-panfrost/` was only read. A reverse-patch **dry run**
against 24.3.4 found 11 of the 15 original file paths missing; of the
remaining hunks, 15 failed and one applied with fuzz. This is evidence of
API/layout drift, not a compilation or runtime result.

## What can be reused

| Area | Old Midgard implementation | Work for a modern forward port |
|---|---|---|
| Architecture plumbing | `meson.build`, v5 entrypoints and dispatch | Add v5 build/dispatch/device selection without advertising unimplemented features. |
| Vertex/tiler/framebuffer jobs | `panvk_vX_cs.c` and `panvk_vX_cmd_buffer.c` | Adapt Midgard descriptor packing and polygon-list allocation to modern JM command state. Preserve T830's nonhierarchical tiling quirk. |
| Shader lowering | v5 branches in `panvk_vX_shader.c`, including fragment-output/blend conversion | Adapt to current NIR and shader interfaces; reuse the maintained Midgard compiler and shared Panfrost helpers where compatible. |
| Textures/descriptors | `panvk_vX_image.c`, descriptor sets and meta operations | Restore v5 descriptor placement/indirection in modern image views and descriptor state. Bifrost resource layouts cannot simply be assumed valid. |
| Clears, copies and presentation | old meta blit/copy/clear code and framebuffer handling | Adapt to current Vulkan runtime/meta interfaces; validate image layout, synchronization, and ownership before scanout integration. |

Current PanVK retains JM support for architectures 6 and 7; Midgard uses
the same broad job-manager submission family, but different job/descriptor
formats. Current Panfrost still implements Midgard compiler and hardware
helpers for GLES. These are useful foundations; neither fact makes enabling
an architecture-5 Meson target sufficient.

The old source itself exposes correctness gaps. In particular,
`panvk_index_minmax_search()` reads index buffers on the CPU while recording
indexed draws and prints a warning that this is invalid in Vulkan. Restoring
that path unchanged would mishandle GPU-produced indices and synchronization.
Modern JM GPU preprocessing is worth investigating, but its suitability for
Midgard has not been established. The removal commit's compiler/conformance
concerns are historical evidence; they are not proof that every listed
compiler problem remains unfixed in today's GLES compiler.

## Recommended experiment and acceptance boundary

1. Build the old v5 driver separately and run an offscreen clear plus a simple
   non-indexed SPIR-V triangle in the existing Linux appliance. Check GPU ID,
   actual rendered pixels and hardware execution. Enumeration alone is not
   proof of rendering. Keep the working GLES build as the installed baseline.
2. Use that as a reference while implementing a distinct v5 path in Ame's
   pinned modern Mesa. Bring up shader compilation and fixed draw state before
   descriptor-heavy, indexed, or indirect workloads. Avoid unrelated modern
   Mesa upgrades during the initial hardware bring-up.
3. Validate memory bounds, GPU faults, fences/barriers and teardown; integrate
   the native Vulkan session bridge and presentation only after rendering works.
   Keep host shadow page-table validation and pinned-memory ownership active.
4. Expand features with targeted tests and Vulkan CTS. A triangle is an
   experimental milestone, not proof of Vulkan 1.0 completeness or conformance.

This does not depend on discovering an external GPU IOMMU. A working revived
driver would still need to operate within Ame's existing host-controlled GPU
address translation and isolation. Compatibility of the old driver's exact
submission patterns with that host backend remains a hardware-test question.
