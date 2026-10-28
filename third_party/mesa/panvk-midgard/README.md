# Experimental Midgard PanVK

Twenty-three patches apply in order to Mesa 24.3.4, SHA-256
`e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025`:

1. `0001-panvk-midgard-bringup.patch` — the architecture-5 forward port.
2. `0002-panvk-v5-tagged-framebuffer-pointer.patch` — the fix that made the
   first hardware frame render.
3. `0003-panvk-v5-color-preload.patch` — injected Midgard wallpaper jobs for
   single-layer, single-sample color LOAD with fixed-function blendable formats.
   T830 pixel preservation passed on v6b, followed by 35 animation frames and
   295 GLES frames, with display restoration and no fault markers.

4. `0004-panvk-v5-depth-stencil-msaa-preload.patch` — depth/stencil and per-sample
   preloads. Depth, stencil and 4x MSAA now pass T830 pixel tests.
5. `0005-panvk-v5-blend-compiler-compute.patch` — blend equations, shader fallback,
   typed conversion, private-array lowering and embedded compute TLS.
6. `0006-panvk-v5-storage-image-descriptors.patch` — storage image descriptors and
   VS table/ID attribute separation. Compilation and recording tested.

7. `0007-panvk-jm-fault-only-tracing.patch` — decode failed job chains on demand.
8. `0008-panvk-v5-gpu-index-expansion.patch` — GPU index/VBO expansion,
   push-range metadata and varying-allocation overflow fixes.
9. `0009-panvk-v5-polygon-list-alignment.patch` — page alignment matching
   Gallium; removed the expanded suite's DATA_INVALID faults on T830.
10. `0010-panvk-v5-gpu-indirect-dispatch.patch` — GPU descriptor/sysval patching,
    zero dispatches and changed-input resubmission.
11. `0011-panvk-v5-minmax-blend-destination.patch` — correct MIN/MAX destination loads.
12. `0012-panvk-v5-occlusion-queries.patch` — occlusion, availability, reset and
    CPU/GPU result retrieval. Four new query cases pass on T830.
13. `0013-panvk-v5-submit-bo-residency.patch` — pin user and internal BOs during
    JM submission, retaining references while the kernel takes ownership.
    Conservative device snapshots apply only to v5's single-queue, no-external-
    extension configuration. The v16 T830 run passes the 42-case pixel matrix.
14. `0014-panvk-v5-sequential-layered-rendering.patch` — separate physical layer
    batches, layer-filtered vertex positions, per-layer LOAD and partial clears.
    Six new array/mip/MSAA cases pass the software reference and record on v5;
    The v17 T830 pixel run passes all six cases.
15. `0015-panvk-v5-disabled-point-line-limits.patch` — report unit ranges and zero
    granularity while large points and wide lines are disabled, matching CTS.
16. `0016-panvk-v5-robust-indexed-gather.patch` — bounded 64-bit indexed VBO
    source offsets and per-byte guards, plus candidate robustBufferAccess
    reporting. Two overflow/negative-index tests pass the software reference
    and record on v5. The v19 pixel predicates passed, but retained logs contain
    a GPU page fault during the overflow-index case; this is not a robustness pass.

17. `0017-panvk-v5-lazy-command-descriptor-slabs.patch` — allocate GPU slabs on
    first use instead of reserving 64 KiB for every empty command buffer. The
    exact CTS 10,000-primary-buffer allocation case passes the DRM shim.
18. `0018-panvk-v5-compute-workgroup-abi-limits.patch` — keep image-copy
    workgroups and public limits within Midgard's 128-thread compiler ABI.
    Fixes the compiler assertion seen in the v17 CTS triangle's image readback.

19. `0019-panvk-v5-lazy-layer-view-storage.patch` — retain per-rendering layer
    clones in the existing descriptor arena rather than every empty command
    buffer. The v5 command-buffer object shrinks from 10,752 to 9,960 bytes;
    recording checks pass and fresh-archive reproduction has zero mismatches.
   With patch 20 and the compressed runtime, the v37 phone allocation check
   creates all 10,000 primary command buffers successfully.

20. `0020-panvk-v5-lazy-recording-state.patch` — allocate v5 recording state at
    `vkBeginCommandBuffer`, reducing an unused command buffer from 9,960 to
    6,168 bytes. Public API checks cover injected allocation failure, primary
    and secondary buffers, implicit reuse and explicit pool/buffer resets.
    The exact 10,000-buffer CTS allocation case now passes in a 128 MiB CPU VM
    with the private compressed-rootfs prototype (80 MiB peak process RSS).
    This uses the DRM shim and establishes no GPU execution. Phone validation
   of patches 19–20 now passes on v37: the exact unmodified allocation case
   creates 10,000 primary command buffers, with complete QPA and no captured
   faults. This is allocation coverage, not execution of those buffers.

21. `0021-panvk-v5-drop-cpu-index-mappings.patch` — v5's GPU index gather no
    longer creates the obsolete CPU index-search mapping. The shared legacy
    mapping also uses the correct within-page offset and readable protection.
    The previous mask could make destruction unmap the page after the actual
    mapping for suballocations at odd page offsets. Build, lifecycle, recording
    and fresh-archive reproduction checks pass; phone validation remains pending.

22. `0022-panvk-jm-command-batch-allocation-errors.patch` — check command-batch
    allocation before initializing its arrays, retain host OOM in the command
    buffer, and stop event/draw setup when allocation fails. A public API failure
    injection previously crashed the driver process; it now returns host OOM
    from `vkEndCommandBuffer` and resets cleanly. Recording and 22-patch source
    reproduction pass. The frozen v27 package contains patches 1–21.

23. `0023-midgard-lower-wide-integer-comparisons.patch` — lower integer 64-bit
    comparisons before Midgard condition scheduling. The gather's bounds check
    previously became a scalar `CMP.le.i32`, discarding the high word of a
    +4 GiB source offset. Compiled NIR now checks both words; all seven gather
    variants pass the maintained output check, and the full suite records
    51 cases with two S8 skips. Fresh-archive reproduction: 23 patches, 81 files,
    zero mismatches. The v32 T830 run passes all 51 supported pixel cases,
    including the overflow/negative-index cases, 37 Vulkan frames and 296 GLES
    frames with restoration and no fault markers in the post-Vulkan or
    post-GLES retained logs. This is selected hardware coverage, not conformance.

The v16 native Vulkan frontend run passes 42 supported pixel cases, then 38
triangle animation frames and 296 GLES frames, with display restoration and no
GPU faults. Two S8 format cases are skipped as unsupported. That frozen package
contains patches 1–13. Current source adds layered rendering and corrected limits.
The v17 phone run (patches 1–15) passes all 48 supported pixel cases,
38 Vulkan frames and 295 GLES frames. The first object-management CTS group
has 42 passes, 5 unsupported skips and no failures. The expanded native frontend
software reference has 53 cases, including guarded tiled image readback; tests now require
its complete summary and reject validation errors. Full API coverage and CTS
remain open; see [completion work](../../../docs/graphics.md#anne_vulkan_completion).

The v19 physical run (patches 1–18) reports 51 supported pixel cases, 38 Vulkan
frames and 296 GLES frames, but its raw log contains a GPU page fault. The same
fault occurs in v20, v22, v24 and v29. The old gate missed `Unhandled Page fault`;
the corrected gate rejects it independently of pixel results, including faults
in GLES output. Official CTS smoke has 6 passes; info has 17 passes
and 2 unsupported skips. These are selected development groups, not conformance.

Build with `scripts/linux/build-panvk-mesa.sh`.

This is a partial architecture-5 forward port using modern PanVK and the
maintained Midgard compiler. It is neither a revert of the 2022 removal nor a
complete/conformant Vulkan implementation. Hardware rendering on ANE **is now
verified** for one static triangle: `diag gpu vulkan` on the T830 reports
`status=0 frames=1 checksum=355e1a23 display_restored=1` with the client's
red/green/blue interior probes verified. The later v5 hardware image ran 37–38
frames per five-second animation and repeated Vulkan/GLES transitions. Broader
descriptor and copy paths, more LOAD formats and Vulkan CTS still need
hardware coverage. Unsupported
paths and evidence are tracked in
[the status document](../../../docs/graphics.md#anne_vulkan_midgard).

`0002` exists separately because `0001` was already applied to the working tree
when the fault was found. The complete patch series is checked by reversing it against the
development tree; source provenance includes shared compiler changes.
`scripts/linux/check-panvk-reproduction.py` also verifies the recorded source
against a fresh pinned archive plus the complete series.

Mesa source files retain their upstream MIT notices. New Midgard glue uses
SPDX MIT identifiers. The build script's glibc C11-name compatibility adjustment
is separate from the driver patch and is included in source provenance.
