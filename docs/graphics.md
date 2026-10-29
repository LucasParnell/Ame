# Native graphics and phone rendering

Start with [the current graphics architecture](architecture.md#graphics-native-apis-guest-drivers-native-presentation). The direct-scanout section supersedes older CPU-copy presentation descriptions on supported Anne pipelines. API and hardware checkpoints are retained separately: an experimental PanVK result is not native Vulkan API completeness or CTS conformance. Build recipes are in [Linux appliances](linux-appliances.md).

- [Native Vulkan frontend](#native-vulkan-frontend)
- [Anne native direct scanout](#anne_direct_scanout)
- [Native EGL/GLES through Dango](#native_graphics)
- [Midgard Vulkan completion work](#anne_vulkan_completion)
- [Experimental PanVK Midgard port for ANE](#anne_vulkan_midgard)
- [GPU optimization work in progress](#gpu_optimization_work)

## Native Vulkan frontend

The current tree provides the freestanding `libVulkan` CMake target, linked to
`ame_graphics` and libmochi. `scripts/vulkan/generate-frontend.py` derives Vulkan
1.0 entry points and RPC metadata from the pinned `third_party/vulkan/vk.xml`.
The client is in `userland/libvulkan/`; the Linux decoder is in
`linux-artifacts/panfrost-renderer/vulkan_rpc.cc` with generated dispatch.
Mesa/PanVK is loaded inside the appliance, never into a native application.

Link `libVulkan`, provision the graphics channel/command arena in the signed
service descriptor, and connect it with `ame::graphics::connect(boot)` before
using the Vulkan entry points. `userland/msh/msh_gpu.cc` is the current example:
`diag gpu vulkan` creates the native triangle resources and submits the
five-second animation through this frontend.

The codec bounds packet size, pointer recursion/fixups and output copies.
Native pointer graphs become packet offsets and Vulkan objects use typed remote
handle tokens. Unsupported extension chains, callbacks, layers and extension
requests are rejected rather than forwarded as native pointers. Mapped memory
is a local staging view with explicit flush/invalidate transfers; guest memory
properties mask host-coherent support. These bounds are experimental API limits,
not a complete Vulkan implementation or hostile-client security audit.

`ameVkPresentDiagnosticImage` from `<ame/vulkan.h>` displays a bound, linear
512×512 `R8G8B8A8_UNORM` image with one sample and mip/layer zero in GENERAL
layout; its queue must own the image. The final call restores the console.
Standard surface/swapchain WSI is not exposed. The generated entry-point set
does not imply that the underlying Midgard driver supports every operation.

Run `python3 -m unittest discover -s tests -p test_native_vulkan.py` for the checked codec probe.
The real software-ICD frontend test is opt-in via
`AME_TEST_VULKAN_SOFTWARE_ICD` and requires an explicit software ICD. Those
checks have different evidence boundaries from installing a phone package.
The first frontend hardware run is now verified: package
`out/anne-native-vulkan-frontend-v14`, kernel SHA-256
`c3a7134ffcd5fc8bab35ff880722f6cecd6e69307c7d22f65a945a771f702773`.
After backup and complete 24 MiB readback, the native frontend passes 42 pixel
cases (two unsupported S8 cases skip), static triangle checksum `355e1a23`,
LOAD checksum `6babfed3`, 38 animated frames (`1d87c9eb`) and display restoration.
GLES then completes 295 frames with no GPU fault markers. Evidence:
`out/mesa-panvk-midgard/phone-native-frontend-v14/phone-test.json` and
`hardware-test-console.log`. The obsolete triangle-only graphics API id 2 is
removed; id 3 carries the native Vulkan API. Earlier phone results below are
historical driver evidence. The older reference broker and
lavapipe-only executor are no longer part of the maintained source path.

<a id="anne_direct_scanout"></a>
## Anne native direct scanout

<a id="anne_direct_scanout-anne-native-direct-scanout"></a>

The native GLES window surface uses two independently owned 1 MiB arenas.
Panfrost imports both as 512×512 ARGB8888 EGLImages. DSS reads the presented
arena directly; the native framebuffer service does not copy pixels on this
path. The surface is centred on the 1080×2280 panel, surrounded by black.
The console backing remains intact and its original register plan is restored
when the diagnostic ends. This is a fixed diagnostic surface, not arbitrary
window composition or a general display modesetting interface.

`boards/huawei-anne/scanout.h` accepts only the verified inherited physical,
linear, uncompressed RGB32 video-mode pipeline with one enabled overlay layer.
Unsupported pipelines retain the framebuffer service's CPU presentation path.
Register offsets and sequences follow the Hi6250 vendor kernel at commit
`65e545cbaa11d132e243287b150146bc2a51c45e` in
`hisi-oss/android_kernel_huawei_hi6250-8`: `soc_dss_interface.h`,
`hisi_overlay_utils.c`, `hisi_overlay_utils_hi6250.c`, `hisi_fb_isr.c`, and
`hisi_overlay_utils.h`.

The display capability binds exactly two nonoverlapping, page-aligned, owned
arenas of the required size below the DSS 32-bit physical address limit. The
kernel pins their object records. Userland cannot supply arbitrary register
offsets or physical addresses to a flip. Only these arenas are registered as
SharedGpu; command uploads and shader-cache storage remain CPU-only. Existing
Mali shadow translation ownership and permission checks remain enforced.

Presentation completes GPU work and the DMA-buffer cache synchronization before
programming DSS. A shadow update under the MCTL mutex is followed by a fresh
global ITF0 VSYNC, live ACTIVE0 timing state, and matching source-address
readback. Only then does the service acknowledge retirement of the previous
front buffer. It polls through short nonblocking syscalls with timed sleeps;
there is no vblank wait with interrupts disabled. The renderer cannot reuse
the retired slot before that acknowledgement. Rendering remains serialized
with presentation; explicit asynchronous GPU fences are future work.

Timeouts and underflows stop new presentations and retain buffer references.
An ordinary restore requires the same retirement proof. If the display owner
exits while scanout may still be live, the kernel programs the preserved
console and quarantines the display with both buffers pinned until reboot.
An emergency register write alone never authorizes freeing a DMA source.
The framebuffer service also restores a stalled animation after five seconds.

Context creation and destruction perform a bounded main-thread retirement
handshake through the manager and framebuffer service before changing imported
targets. A failed handshake prevents context replacement. EGL window unbind
requests restoration too. The obsolete legacy CPU-triangle manager operation
is unsupported because it cannot obey this target ownership protocol.

Graphics ABI version 2 carries separate target-index and completed-scanout
metadata; GL query values remain independent. `diag gpu` reports
`scanout_flips`, including the final restore. On the direct path the second
target replaces the former 1 MiB console-copy snapshot. Per-frame display
state is fixed-size and requires no allocation.

Initial hardware validation used package `out/anne-package-20261022T230028Z`,
SHA-256 `777ffdca5ce27ab93679505fb58a4422797ec4ac6271aaa0feb16ce0a55a0528`.
The first five-second run completed 151 frames and 152 scanout transitions,
average GPU render 2036 µs. Its conservative event sequence waited an extra
refresh. The user confirmed the repeated animation, black surround, and
successful console restoration. The global DSS status after the first run
had no underflow bit set. Logs are under the ignored private
`out/anne-device-private/{direct-scanout,gpu-speedup}` directories.

The final implementation uses live ACTIVE0 timing after fresh VSYNC and skips
the diagnostic's software pacing sleep when scanout already provides pacing.
Installed package `out/anne-package-20261022T232046Z`, SHA-256
`0923f8f11e04db607d7c4a838ea1c420225f81d27102f32c4a1e7494de20a841`, passed full
partition readback in `kernel-install-20261022T232151Z`. Cold, warm, 49 KiB
upload, and reset/recreate each
completed 298 frames in five seconds, with 299 acknowledged transitions per
run including console restore. Average GPU render times were 2100, 2050,
2084, and 2130 µs. The final global DSS status was `0x1fb`, underflow bit clear.
The capture is `gpu-speedup/scanout-paced.jsonl` under the private directory.

Thirty focused host tests pass, with six affected tests repeated after the
pacing change. They cover stale events, source programming, restoration,
timeout, underflow, failed lifecycle retirement, and distinct protocol IDs.
An intermediate revision reused shader-cache message IDs for retirement;
hardware context creation rejected it. Distinct IDs fix that collision, with
a compile-time regression check covering the complete shared channel namespace.
CPU submission time now includes display-boundary waiting and cannot be
compared directly with the earlier CPU-copy submission metric. No measurement
of input-to-panel latency has been made.

<a id="native_graphics"></a>
## Native EGL/GLES through Dango

<a id="native_graphics-native-eglgles-through-dango"></a>

Ame applications can link the freestanding `libEGL` and `libGLESv2` CMake targets.
They use `ame_graphics`, which uses libmochi for capability mapping, SPSC rings,
doorbells and deadline waits. No Linux or Mesa library is linked into an Ame
application. Mesa and Panfrost remain inside the Linux appliance.

```text
Application → EGL/GLES client → API-neutral graphics stream (libmochi)
            → Dango manager → guest GLES decoder → Mesa/Panfrost → Mali
                                        ↘ shared render target → Ame framebuffer
```

The public libraries are alongside `userland/lib` in `userland/libegl`,
`userland/libgles` and `userland/libgraphics`. `libmochi` is an interface target
for the existing Mochi headers and freestanding runtime dependencies.

<a id="native_graphics-application-integration"></a>
### Application integration

```cmake
target_link_libraries(my_app PRIVATE libEGL) # transitively GLESv2, graphics, libmochi
```

The service descriptor must provision the client endpoint's ring and doorbell
capabilities. The current phone profile provisions these for `msh` under `diag`.
Bind them once before initializing EGL:

```cpp
#include "ame/graphics.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>

if (!ame::graphics::connect(boot, "diag")) { /* unavailable */ }
EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
eglInitialize(display, nullptr, nullptr);
// Choose the ES2 config, create context/surface, make current, then call GLES.
```

The standard C entry points support direct linking. `eglGetProcAddress` resolves
implemented EGL/GLES entry points and returns null for unsupported names; an
application may supply it to a generated GLAD loader. GLAD is optional and is
not vendored. The Khronos public headers are copied from the pinned Mesa
24.3.4 installation, retaining their licenses. The platform type header uses
`<cstdint>` in C++ for Ame's freestanding SDK; C uses `<stdint.h>`.

<a id="native_graphics-current-api-boundary"></a>
### Current API boundary

This is an experimental EGL/GLES 2 subset, not a conformant complete
implementation. It currently supports:

- Display initialization, config selection, ES2 context creation, make-current,
  teardown, error reporting, current-object queries and function resolution.
- Shader source/compilation, shader status, programs, attribute bindings,
  linking/status, buffer allocation/uploads, vertex attributes, viewport,
  clear, draw-arrays, flush/finish, errors and implementation strings.
- One thread, context, surface and provisioned client. Shared contexts and
  additional simultaneous objects are rejected. Context destruction releases
  the actual guest EGL context and its resources.
- A fixed 512×512 surface, backed by an imported shared render target on the
  phone and a pbuffer fallback when import is unavailable. The `AME_EGL_DIAGNOSTIC_WINDOW` token from `<ame/egl.h>` selects the existing
  diagnostic overlay: swap displays the frame for five seconds and restores
  the console. It is not yet a general native window system. Pbuffer swaps
  submit work without displaying it. `ameEglSwapDiagnosticFrame` presents animation
  frames immediately; its final `EGL_TRUE` call restores the console. The display
  service keeps the original console snapshot across frames and restores it after
  five seconds without an update if the client stalls.
- Shader source up to 1,394 bytes, attribute names up to 255 bytes and buffer
  allocations up to 16 MiB. Larger buffer uploads are split across bounded
  batches. Vertex pointers are offsets into bound VBOs; client arrays are
  rejected. Textures, uniforms, indexed draws, read-pixels to application
  memory, shader logs and general window surfaces are not implemented yet.

Use `userland/msh/msh_gpu.cc` as the complete working example. `diag gpu`
creates a native EGL context, uploads native shader strings and vertex data,
issues GLES draw calls, and rotates the triangle for five seconds at up to 60 fps.
The vertex shader rotates about X and Y and applies perspective projection using
a constant angle attribute (`glVertexAttrib1f`). Geometry is uploaded once; frames
update only the angle, clear and draw. No per-frame VBO allocation or upload is
needed. Submission/render averages and maximum submission latency are reported;
these exclude the deliberate pacing sleep and are not panel scanout latency.
Streaming frames retain one bounded GPU clock lease and defer image checksumming
and shader-cache persistence until the final restore. GPU completion and DMA-buf
cache synchronization still precede every native display copy. Repeated calls retain the
context, shaders and VBO; `diag gpu reset` releases them. It no
longer asks the appliance to execute a hardcoded triangle operation.

<a id="native_graphics-batched-object-creation-and-cpu-access"></a>
### Batched object creation and CPU access

Native shader/program/buffer creation allocates opaque client names locally and
queues creation with subsequent setup commands. It no longer requires a reply
for each object. Mesa names remain private to the decoder's compact 128-entry
table. The table is bounded, checks object kind and duplicate names, reuses only
deleted slots, and clears on context destruction. Client names are monotonic,
so an old deleted name does not acquire the identity of a newly created object.
Legacy create/query clients remain supported in a separate mode; a context
cannot mix legacy raw names and named creates. Queries still drain earlier work.
Resource exhaustion or a failed deferred create is reported at batch completion.

The triangle batches both shader compilations and checks their combined result
through program link status. `diag gpu upload` tests a 49,152-byte VBO, placing
the actual vertices in its final chunk to exercise asynchronous command-slot
reuse before drawing. Normal diagnostics retain the small immutable VBO.
The reported `setup` time is separate from per-frame submission/render times.

The shared dma-buf CPU-access callback waits for the applicable reservation
fences **before** invalidating CPU caches. Linux v6.12's generic CPU-access
function waits after calling the exporter callback, which alone is too late
for that invalidation. The exporter uses a ten-second interruptible timeout;
timeout/error prevents pixel consumption. The renderer retains `glFinish()`
and the paired DMA_BUF_IOCTL_SYNC operations. This change does not remove
the framebuffer copy or introduce display/GPU overlap.

<a id="native_graphics-modularity-and-transport"></a>
### Modularity and transport

`protocols/graphics.h` defines versioned batch envelopes, fragmentation,
completion and presentation flags, with no GL types or pointers. The transport
implementation does not inspect API commands. `protocols/gles2.h` and the guest
`gles_dispatch.h` implement the separate GLES encoding. A future API can add
its own encoder and decoder behind this envelope. No Vulkan implementation is
added by this work; existing Vulkan-related repository work is unchanged.

Commands accumulate in bounded 16 KiB batches. A CPU-only 32 KiB shared arena
provides two command slots; one descriptor replaces per-fragment IPC. glFlush
and capacity flush enqueue asynchronously. Queries, glFinish and presentation
drain pending batches. Replies retire command storage; they are not GPU fences.
Without the arena, a 1,536-byte inline fallback limits the client to one in-flight
batch. The receiver validates sequence, sizes, protocol version, flags and the
entire GLES layout before execution. Transport errors surface as GL/EGL errors.

Native pointers never enter the command stream. Upload data is copied and
resource names are guest GL names. Only the pixel arena is registered SharedGpu;
command and cache arenas remain CPU-only. The host shadow MMU validates owned
SharedGpu mappings and pins their backing until GPU reset. Page tables must stay
in owned ordinary guest RAM. The guest imports the pixel DMA-buf as an EGLImage
in the native EGL context, retains its FBO, waits for GPU completion and
invalidates CPU caches before reading the checksum. This removes glReadPixels
and format conversion. The native framebuffer still copies the image into the
panel buffer, so this is not full zero-copy scanout.

Mesa's EGL blob cache uses a bounded dictionary and a separate CPU-only transfer
arena. The native manager persists opaque snapshots at `/data/mesa-blobs-v1`
through libmochi FS. No guest filesystem/block storage is needed. The guest main
thread checkpoints at command boundaries; worker callbacks use a mutex and never
write to SPSC channels. Invalid snapshots are cache misses. Mesa supplies keys
containing its driver build/GPU identity.

<a id="native_graphics-validation"></a>
### Validation

```sh
.venv/bin/python3 -m unittest tests.test_native_gles
scripts/test/panfrost-compiler.sh
cmake --build out/anne-usb-native-release --target anko mochi_system provenance -j12
```

The ASan/UBSan frontend test runs the production native EGL/GLES encoder and
guest decoder together with a mock graphics backend. It covers repeated context
lifecycle, shaders, 8 KiB uploads spanning batches, draw/present, function lookup,
truncated and malformed batches, duplicate fragments, unsafe client pointers
and transport failure. This is CPU/ABI evidence; physical rendering requires
installing the matching native bundle and Linux appliance and running `diag gpu`.

22 October 2026: the native phone build and all 46 native-graphics, appliance,
channel, platform, flash-helper and USB tests passed, as did the Mesa shader
compiler check. Package `out/anne-gpu-native-egl-v3` was flashed and its complete
24 MiB partition readback verified. Kernel SHA-256:
`8016874f4d1de107b31e47b5c3e021ae8abf301cbb655dad5265f3494b792cad`.

Two physical `diag gpu` runs through the new native EGL/GLES API succeeded,
including context destruction and recreation. Rendering/completion/readback
and conversion took 42,028 µs and 36,613 µs. Both returned checksum `36244b55`,
matching the earlier hardware triangle, displayed for five seconds and restored
the console. The post-test diagnostic recorded zero EL0 faults and no guest
shutdown. Evidence is in ignored private files
`out/anne-device-private/native-egl-v3-serial.log` and
`kernel-install-20261022T112622Z/result.json` in the same directory.

The renderer-only build also refreshes the DTB's page-aligned initrd end after
rebuilding the compressed archive. Manifest generation rejects a stale bound.
A regression test grows the archive across page boundaries and verifies that
refreshing the bound preserves the GPU enablement and measured timer frequency.

The shared-target image `out/anne-direct-fixed-v1` subsequently passed cold/warm
physical rendering at 10,565 / 5,123 µs with the same checksum; reset and recreation
passed at 6,646 µs. Its DVFS logs confirm 480 MHz during render and restoration to
360 MHz. See `docs/graphics.md#gpu_optimization_work` for current optimization status and
private evidence paths.

The cache-enabled `out/anne-direct-cache-v3` then loaded a snapshot written by
v2 before reboot and recorded five blob-cache hits with zero misses. The warm
triangle took 5,300 µs with checksum `36244b55`. USB stayed connected and diag
reported zero EL0 faults. The later fixed-surface direct-scanout implementation
and physical validation are described in
[ANNE_DIRECT_SCANOUT.md](graphics.md#anne_direct_scanout).

<a id="anne_vulkan_completion"></a>
## Midgard Vulkan completion work

<a id="anne_vulkan_completion-midgard-vulkan-completion-work"></a>

Recorded implementation/acceptance checkpoint, 24 October 2026. Local patches may extend this checkpoint; they require separate acceptance evidence.

<a id="anne_vulkan_completion-current-hardware-baseline"></a>
### Current hardware baseline

**v12 passes 38 supported pixel cases, then the triangle and animation.**
Two standalone S8 cases are correctly skipped by the T830 format query.

- Package: `out/anne-panvk-minmax-indirect-v12`.
- SHA-256: `cfd5c40973000ea880dbef7973525fae0a13973fd22addcc6969c9979c833e27`.
- Backup, kernel-only flash and complete 24 MiB readback verified.
- Evidence: `out/mesa-panvk-midgard/phone-minmax-indirect-v12/phone-test.json`
  and `hardware-test-console.log`; watcher log `out/panvk-completion/hardware-v12.log`.
- Static triangle checksum `355e1a23`, LOAD checksum `6babfed3`.
- Animation: 38 frames, checksum `91c41c7a`, display restored.
- Subsequent GLES: 295 frames. No GPU fault markers.
- Pixel coverage: linear/tiled RGBA8 LOAD, depth/stencil LOAD, 1x/4x MSAA,
  sampled readback through compute/SSBO, blending including MIN/MAX and
  nonuniform constants, float/integer render targets, storage image read/modify/
  write, image atomics, GPU-expanded indexed draws (16/32-bit indices, negative
  base vertex, instancing, VBOs, primitive restart), GPU-written indirect dispatch
  dimensions, zero dispatches, changed-input resubmission, distinct-sample
  MSAA LOAD history, and two-color-attachment LOAD.
- Reference: 40 cases pass on extracted, signature-verified Arch Lavapipe
  26.2.5 with Khronos validation and LP_NUM_THREADS=1. Two additional cases are
  S8 formats unsupported on T830. Log: `out/panvk-completion/v12-software-reference.log`.
- T830 DRM shim: 38 cases record / 2 skip; this is compilation/recording only.

The checkpoint used the BootROM retry route. Package backups and full partition readback evidence are retained with the named artifacts. Installation mechanics are documented in [Anne operations](../boards/huawei-anne/operations.md).

<a id="anne_vulkan_completion-maintained-source"></a>
### Maintained source

Pinned Mesa 24.3.4, archive SHA-256
`e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025`.
Working source/build/install: `out/mesa-panvk-midgard/`.
All twenty-three patches are maintained under `third_party/mesa/panvk-midgard/`:

1. Architecture-5 bring-up.
2. Tagged graphics framebuffer/TLS pointer.
3. Single-sample color LOAD.
4. Depth/stencil and per-sample LOAD.
5. Blend equations/shader fallback, typed conversion, private-array lowering,
   and tagged compute MFBD with embedded TLS.
6. Storage image descriptors, mapped CPU aliases and reserved VS ID attributes.
7. Fault-only synchronized JM decoding (`PANVK_DEBUG=trace_fault`).
8. GPU index/VBO expansion, optional push-range metadata repair, allocation
   overflow checks. No CPU index-buffer reads while recording.
9. Page-aligned polygon lists: **the fix that removed the suite's DATA_INVALID
   faults**. Gallium allocated BOs; the port suballocated with only 64-byte alignment.
10. GPU indirect dispatch patching, NULL/non-NULL resubmission, recorded
    invocation templates, num-workgroups sysvals and bounded WLS concurrency.
11. MIN/MAX factor normalization: both operands are read regardless of supplied
    factors, so the destination-load flag must be enabled.

12. Occlusion queries, availability, GPU reset/copy and CPU result retrieval;
    four query cases pass on the physical T830.
13. Submission BO residency, retaining all live user/internal BOs while the
    kernel takes ownership. The single-queue v5 boundary has no external extensions.
14. Sequential layered rendering, independent per-layer framebuffer/polygon
    lists/LOAD, partial render areas and default layer-zero vertex filtering.
    All six added cases pass on the v17 T830 image.
15. Unit point/line ranges and zero granularity for disabled optional features.
16. Bounded 64-bit indexed vertex-source offsets and per-byte source guards.
    Candidate robustBufferAccess feature reporting; software/recording tests pass,
    physical robustness tests remain pending.
17. Lazy GPU descriptor slab allocation for empty command buffers. The previous
    eager 64 KiB slab per buffer OOM-killed CTS's 10,000-buffer allocation case.
    The exact case passes the DRM shim and, with patches 19–20 and the compressed
    runtime, passes on the v37 phone image.
18. Image-copy helpers use 16×8 workgroups on v5, within the compiler's 128-thread
    ABI, and public compute limits are clamped accordingly. This removes the
    compiler assertion reproduced by CTS `api.smoke.triangle` image readback.
19. Lazy layer-view CPU storage in the descriptor arena, avoiding 792 bytes per
    empty command buffer. v5 object size is 9,960 bytes; 10,000-object allocation
    passes the host DRM shim, and the 53-case suite still records (51/2 skip).
    The v37 phone allocation check passes with patches 19–20 together.
20. Lazy v5 recording state, allocated at `vkBeginCommandBuffer`. An unused
    command buffer is now 6,168 bytes. Primary/secondary failure injection,
    reset and implicit reuse checks pass without leaked device allocations.
    The exact 10,000-buffer CTS case passes in the private 128 MiB compressed
    rootfs VM, with peak process RSS 81,588 KiB. This is CPU-only DRM-shim
    evidence. On v37, the exact unmodified allocation case also passes on the
    phone, creating all 10,000 primary command buffers; this checks allocation,
    not GPU execution of those buffers.
21. Drop unused CPU index-search mappings on v5, whose indexed draws gather
    source data on the GPU. Correct the legacy mapping's page-offset mask and
    read protection. Build and recording checks pass, and all 21 patches
    reproduce the recorded source from the pinned archive without mismatches.
    The v29 and v30 phone packages contain patches 1–22.
22. Check command-batch allocation before dereferencing the result, preserving
    host OOM through `vkEndCommandBuffer`. Public event-command failure injection
    reproduced a driver-process crash before the fix and passes afterward.
    Recording and fresh-archive reproduction pass; this patch is newer than the
    frozen v27 package, which contains patches 1–21.
23. Lower 64-bit integer comparisons before Midgard condition scheduling.
    The indexed gather's 64-bit bounds check previously became a scalar
    `CMP.le.i32`, ignoring the high word and admitting +4 GiB source offsets.
    The compiler now compares both words; a maintained compiled-NIR check
    rejects the old output and accepts all seven current gather shaders.
    The 51-case recording suite passes with two S8 skips. The v32 phone run
    passes all 51 supported pixel cases, 37 Vulkan frames and 296 GLES frames,
    with restoration and no fault markers through the final post-GLES retained
    log. Evidence: `out/mesa-panvk-midgard/phone-wide-compare-usb-baseline-v32/`.
    This verifies the selected overflow regression on T830, not full robustness
    or Vulkan conformance.

Source snapshots for patch generation: `out/panvk-completion/before-*`.
Fresh-archive reproduction: `out/panvk-completion/twenty-three-patch-reproduction.json`
(23 patches, 81 source files, zero mismatches).
The build records shared compiler changes as well as Vulkan source hashes.
Do not blindly regenerate patches from nested git: most Mesa files are untracked.

The client suite is `linux-artifacts/panvk-renderer/suite.c`, with probe shaders.
`probe.vert` retains a dynamically indexed private array that caught an actual
Midgard compiler assertion; it was fixed instead of replacing the reproducer.
The suite runs before the existing triangle on first initialization. The native
animation timer starts after initialization. Debug tracing is an opt-in build
(`DANGO_PANVK_SUITE_TRACE=1`) and emits only failed chains.

<a id="anne_vulkan_completion-remaining-acceptance-areas"></a>
### Remaining acceptance areas

- General buffer/image copy, fill, blit, resolve and clear coverage, including
  offsets, row/image strides, mips, formats, depth/stencil and layers.
- Layered rendering/preloads: implemented as separate layer batches; verify the
  additional depth/stencil/resolve layer combinations. The six new array/mip/
  MSAA/partial-render-area cases pass on v17.
- Indirect draws and indexed indirect draws remain unsupported. Direct indexed
  and indirect compute now work on hardware.
- Broader query coverage, robust buffer access, feature/limit reporting, compiler
  edge cases and optional logic-op exposure need systematic tests.
- Vulkan CTS fixes: the first physical `info.*` run has 14 passes, 2 unsupported
  skips and 3 failures (mandatory robust buffer access and disabled-feature limits).
  No full CTS/conformance claim exists.
- Repeated warm transitions, failure/teardown/resource checks after the broader
  paths are enabled. No weakening of Mali shadow-MMU ownership, translation,
  cache synchronization or shared-buffer retirement is allowed.

<a id="anne_vulkan_completion-cts-and-host-resource-limits"></a>
### CTS and host resource limits

The PC rebooted during the first CTS dependency build. Cause is not established:
the preceding journal records no OOM or panic. VVL's helper overrode the outer
job limit and spawned 16 compilers. The maintained CTS builder now wraps nested
cmake --build calls to one job, and all large builds run in memory/CPU-capped
systemd user services/scopes. Do not restart unrestricted builds or host hardware
Vulkan tests. Mesa's default job count was reduced to two; current work uses one.

CTS source: `out/vulkan-cts-midgard`, official Khronos repository, tag
`vulkan-cts-1.3.10.0`, commit `7aa3551652743f11e77cc28dd8c19c9f872ecf79`.
Builder: `scripts/linux/build-panvk-cts.sh`. It explicitly selects DE_CPU_ARM_64
(the old CTS defaults to x86 based only on pointer width), and applies the small
Amber integer-header compatibility patch under `third_party/cts/`. CTS test
sources are unchanged. Build runs as user service `ame-panvk-cts.service`, logs
`out/panvk-completion/cts-build-resumed.log`, capped at 4 GiB / one CPU.

The standard Khronos Vulkan loader build is complete, pinned SDK 1.3.296.0.
CTS is launched through the native diagnostic and standard loader. A grouped
artifact registers `info,api,memory,query_pool,compute` without changing test
implementations; full CTS does not fit the current guest/package memory layout.
`build-panvk-cts-group.py` records provenance and each run reports its exact pattern.
The v16 `dEQP-VK.info.*` evidence is under `out/mesa-panvk-midgard/cts-info-v16/`.
Subsequent console access stalled on v16; its cause is unresolved. The launcher
now uses `posix_spawn` instead of libc setup in a forked child of the multithreaded
renderer. On v17, `dEQP-VK.api.object_management.single.*` passes 42 cases, skips
5 unsupported cases and reports zero failures/fault markers. Evidence:
`out/mesa-panvk-midgard/cts-objects-v17/`. The buffer fill/update group passes
24 cases and skips 48; descriptor pool/set tests pass 11 and skip one. The
indirect-compute-offset group skips all 64 cases because no dedicated compute
queue is exposed; it supplies no execution evidence. The info group now passes
15, skips 2 and fails only the 2 robustBufferAccess requirements. The full v17 hardware gate passes 48
supported pixel cases, 38 Vulkan frames and 295 GLES frames with restoration;
package SHA-256 `d9e3e1e2cecf0ad55d1a613fc8bd621452134d4b20ca80767d23137cb3591720`.

The v19 image (patches 1–18) reports 51 supported pixel cases, 38 animation
frames and 296 GLES frames with restoration. A later raw-log audit found an
`Unhandled Page fault` / `JOB_BUS_FAULT` at the overflow-index case, also present
in v20, v22, v24 and v29. The previous gate missed that spelling. Pixel predicates
alone did not detect the fault; these runs are not clean robustness passes.
The corrected gate independently rejects page/translation/bus faults, including
faults printed during GLES or retained afterward, with maintained regression
tests. The historical reports were corrected while preserving the original
verdict; audit: `out/panvk-completion/historical-page-fault-audit.json`.
Its official CTS `api.smoke.*`
group passes all 6 cases; `info.*` passes 17 and skips 2 with no failures. Evidence:
`out/mesa-panvk-midgard/phone-copy-cts-v19/`, `cts-smoke-v19/`, `cts-info-v19/`.
Package SHA-256 `d58d4b0a2e0ff416b11794c4549887e1354faad0b3954f0c28a337710ebd8fd5`.
The native reference now requires all 53 cases, zero skips and no validation errors.

Before patches 19–20, the CTS allocation stress retained roughly 107 MiB of
command-buffer CPU state, beyond the compact guest's remaining memory even
after lazy GPU slabs.
A 256 MiB test-layout experiment was held back: ANE currently limits ordinary
kernel RAM to roughly 220 MiB below the fixed user image window, so that guest
would not fit alongside native services. The trial package remains under `out/`
and was not flashed; its source layout was removed. More host-addressable RAM or
lower empty-command-buffer CPU storage was required for this stress case.
Patch 20 reduces the unused objects to roughly 59 MiB in total. With the private
compressed rootfs, the unmodified allocation case passes at 128 MiB. The ordinary
RAM-expanded runtime still OOM-kills that case. An explicit compressed renderer
build option is integrated and boots both in a CPU VM and on the phone.
The v37 physical run passes the exact
`dEQP-VK.api.command_buffers.allocate_many_primary` case: 10,000 primary command
buffers created, one pass, zero skips/failures, complete QPA and no captured
fault markers. Native request elapsed time is 1,805 ms. Evidence:
`out/mesa-panvk-midgard/cts-many-primary-v37/`. This closes the previously blocked
phone allocation check, without establishing execution coverage for those
10,000 buffers or Vulkan conformance. The selected v37 graphics gate and eight
CDC reopen challenges also pass; [USB evidence](usb.md) records the controller
parser change that made this run possible.

The subsequent full command-buffer group does not complete. Its retained stdout
reports 18 passes and two unsupported maintenance cases, then stops at
`record_many_draws_primary_1`; no final CTS completion/QPA verdict was obtained.
These partial stdout results are not a passed group. Two native CPU snapshots
three seconds apart show CPU1 frozen at 16,548 context switches, 46,288 timer
ticks and 68,358 IPIs, while the other seven CPUs each advance 300 ticks. The
shell and USB remain responsive and native runtime sampling charges no new
time to `dango.manager`. This supports a CPU1/EL2 or kernel-path stall, rather
than another whole-device USB failure. No exact stopped instruction has been
captured. Evidence: `cts-command-buffers-v37/` and
`out/panvk-completion/{cpu-progress-after-cts-v37.log,native-liveness-after-cts-v37.log,usb-v37-after-command-buffers.log}`.

v38 adds fixed per-CPU vCPU/MMIO progress checkpoints, exposed read-only as
`MX_DIAG_VM_PROGRESS` and printed by `diag el2`. They distinguish cache/context
handoff, guest entry/exit, IRQ dispatch, MMIO emulation, Mali lock acquisition,
register access and shadow-table construction. A reader makes at most three
attempts and returns busy if publication is incomplete; it never waits for the
stalled CPU or takes its lock. Guest-entry detail is the last recorded guest PC,
not a live stopped-PC sample. Concurrent-reader/stopped-writer sanitizer checks,
the shell diagnostic checks and an ARM64/QEMU probe pass. The installed v38
image was used for the exact many-draws reproducer; the instrumentation itself
does not fix the stall.

v38's exact reproducer leaves CPU1 at guest-entry stage 3, sequence 1,554,932,
with no subsequent monitor return. The last entry PC is `0xffffffc08020394c`,
immediately after the HVC in Linux `channel_poll`. This is a last-entry marker,
not the instruction at which execution stopped. The matching guest Image hash
is `8edb771e1d5afd305fedce91b8b3b7141501922145a1730fdf9d12bc0e7d2626`;
its ELF is `out/panvk-completion/squash-kernel-build/vmlinux`. Before the
reproducer, CPU1 advances timer/checkpoint counters normally; afterward both
remain fixed across repeated samples. The exact case fails to complete even
without the earlier allocation tests.

The monitor replaced the host CNTP timer with guest CNTP state on entry, so a
guest with that timer disabled could suppress host preemption. A new QEMU
regression using the production monitor reproduces guest writes reaching the
physical timer. The candidate keeps hardware CNTP under host ownership and
emulates the guest's CNTP TVAL/CTL/CVAL registers. EL1PCEN controls these traps;
see the [Arm register reference](https://documentation-service.arm.com/static/6166bf63e4f35d248467c9c0).
The regression now traps a guest disable attempt and returns a physical IRQ
from a guest that masks its own IRQs and loops without exits. Host timer access
and SIMD state remain intact on return. Timer encodings, reserved control bits
and signed 32-bit TVAL deadlines have sanitizer coverage. Four host/QEMU test
methods pass, including preservation of all guest general registers on the
asynchronous IRQ return. A full Linux QEMU boot and 512 ordered channel round
trips also pass.

v39 was installed with backup and full partition readback verified. Its selected
phone graphics gate passes 51 supported pixel cases, two S8 skips, 25 Vulkan
animation frames and 295 GLES frames, with display restoration and no captured
GPU fault markers. The exact many-draws case now returns exit 137 through the
native RPC, and the Linux log reports an out-of-memory kill of `deqp-vk`.
The case is incomplete and failed; complete QPA retrieval does not mean the
killed test wrote a complete result. The reported 115 ms native wait excludes
the preceding start RPC and is not total case runtime. A subsequent USB
controller failure prevents a valid post-case CPU-counter capture, so continued
CPU1 timer progress after this reproduction has not been directly measured.
This demonstrates an observable OOM result in place of the earlier lost VM
reply, without establishing that every stall or USB failure is fixed. Evidence:
`out/mesa-panvk-midgard/{phone-host-preempt-v39,cts-many-draws-primary-v39}/`
and `out/panvk-completion/usb-v39-after-many-draws.log`.

On a fresh v39 boot, the same exact case passes: exit 0, one Pass, zero skips or
failures, a complete QPA session and no captured GPU fault markers. Host wall
time is 11.75 seconds. CPU1 timer ticks and vCPU checkpoint sequences advance
across two samples immediately after CTS and again after QPA retrieval; fresh
shell echo also succeeds. Twelve complete boot-log reads before this run pass.
This closes the missing post-case CPU observation for this reproduction. It
does not supersede the earlier OOM after the graphics workload or establish
unlimited guest-memory capacity. The retained USB log still contains a
payload-looking status-only word (`0x30303030`). Evidence:
`out/mesa-panvk-midgard/{cts-many-draws-immediate-cpu-v39,usb-v39-log-stress}/`
and `out/panvk-completion/usb-v39-immediate-cpu.log`.

After v41's selected graphics gate, the exact many-draws case returns exit 137
after 188.48 host seconds and leaves an incomplete QPA. The captured retained
tail has no OOM-kill marker, so this run's killing mechanism is unconfirmed.
The native 188 ms wait excludes time inside the start RPC and must not be used
as the total runtime. CPU1 timer ticks and vCPU checkpoints advance across two
immediate post-case samples and two post-QPA samples; fresh echo succeeds.
No captured GPU fault or USB controller exit occurs, although the retained log
still contains a payload-looking RX status word. The fresh-boot v39 pass and
this post-graphics failure establish different workload histories; they do not
establish a memory leak or prove a particular guest-memory failure mechanism.
Evidence: `out/mesa-panvk-midgard/cts-many-draws-post-graphics-v41/`.

The v20 package retains the 128 MiB guest and passes the 51 supported pixel
cases, 37 animation frames and 296 GLES frames. Its selected CTS robustness
compute group (`vec4_copy.r32_sint.*`) passes all 12 cases covering out-of-bounds
uniform reads, storage reads and storage writes at 1/3/4/32-byte ranges. The next
204-case compute group did not start because CDC stalled. Evidence remains under
`out/mesa-panvk-midgard/cts-robust-compute-v20/`. USB investigation and regression
fixes are recorded in [Native USB](usb.md).

A private compressed-rootfs prototype keeps the ordinary 128 MiB layout and
packs 52,435,959 runtime bytes into a 12,730,368-byte read-only SquashFS image.
Its purpose is to make immutable code/data reclaimable, reducing pressure during
allocation stress. It boots the dynamic renderer in a 128 MiB Cortex-A53 QEMU
VM, with roughly 100 MiB reported available before a GPU exists. This is a CPU
filesystem/loader check, not GPU execution. Later phone packages starting with
v29 boot the maintained compressed runtime. A separate allocation-only VM fixture contains the DRM shim and is
marked DO-NOT-FLASH. Prototype files live under `out/panvk-completion/`.
The maintained build now has an explicit `DANGO_PANFROST_ROOTFS=squashfs` option
with its own kernel fragment, policy and bootstrap. The fresh v27 renderer build
boots from it in CPU-only QEMU, with 100,300 KiB reported available before GPU
initialization. Its staged runtime contains no DRM shim. The ordinary appliance
policy remains unchanged. v37 validates the exact allocation stress on the
phone. Its installed CTS subset registers `api,compute,info,memory,query_pool`;
the later robustness-compute wildcard matches zero cases and is not a pass.

The proposed late-descriptor-update regression was rejected: core Vulkan 1.0
invalidates a bound command buffer on such an update without update-after-bind.
The invalid test and proposed GPU descriptor copier are not in the patch series.
The native reference runner now fails on validation errors as well as pixel errors.

Lavapipe is extracted locally under `out/panvk-completion/lavapipe/`; its package
signature was verified using the system Arch keyring. It is for host reference
checks only and must never enter the phone image. Use VK_DRIVER_FILES pointing
to its local icd.json and LP_NUM_THREADS=1 for host tests.

<a id="anne_vulkan_midgard"></a>
## Experimental PanVK Midgard port for ANE

<a id="anne_vulkan_midgard-experimental-panvk-midgard-port-for-ane"></a>

Implementation status, 23 October 2026: **first hardware frame rendered,
bring-up still incomplete**. There is a reproducible Mesa 24.3.4 v5 ICD, a real
Vulkan triangle client, and a native `diag gpu vulkan` transport. The physical
T830 now produces a verified triangle. It is not a complete translation of every
removed path and is not conformant Vulkan.

The later v12 run passes 38 supported pixel cases covering depth/stencil,
MSAA, blending, typed formats, storage images/atomics, GPU-expanded indexed
draws, indirect compute, resubmission and MRT LOAD. It then renders 38 animation
frames, restores the display and runs 295 GLES frames with no fault markers.
See [the active completion record](graphics.md#anne_vulkan_completion) for current
patches, evidence and remaining work. The historical runs below explain the
initial bring-up.

Six hardware runs have happened, each flashed with a backup and complete
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

<a id="anne_vulkan_midgard-reproduce-the-build"></a>
### Reproduce the build

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
the v5 tagged-framebuffer-pointer fix that produced the first hardware frame;
`0003` adds the color attachment LOAD path described below.
Applying all three to a pristine 24.3.4 extract reproduces the built tree byte for
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

<a id="anne_vulkan_midgard-implemented-paths-and-fixes"></a>
### Implemented paths and fixes

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

<a id="anne_vulkan_midgard-triangle-and-presentation"></a>
### Triangle and presentation

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

<a id="anne_vulkan_midgard-validation-completed"></a>
### Validation completed

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
- **Physical T830:** six flashed runs with backup and full readback each time.
  Run 4 rendered the static triangle with pixel probes verified
  (`checksum=355e1a23`), `display_restored=1`, and no GPU or MMU fault, followed
  by 296 GLES frames with the display restored. Run 5 animated for five seconds
  (37–38 frames), including repeated Vulkan/GLES transitions. Run 6b additionally
  passed RGBA8 LOAD preservation, then 35 animation frames and 295 GLES frames.
  No CTS result exists.

Logs and development source: `out/mesa-panvk-midgard/`; per-run device records
are in `phone-v1/` through `phone-v5/`. A fresh-source build was checked under
`out/panvk-clean/`; the host image is `out/panvk-host/triangle.ppm`.

<a id="anne_vulkan_midgard-coverage-of-the-removal-commit"></a>
### Coverage of the removal commit

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

<a id="anne_vulkan_midgard-work-still-required"></a>
### Work still required

Color LOAD/preload now has a single-layer, single-sample Midgard wallpaper
implementation for fixed-function blendable formats; its RGBA8 T830 pixel-preservation test passes.
Depth/stencil, MSAA, layered and shader-converted preloads remain rejected. Shader-based blending, general
render-target type conversion, storage-image descriptors, GPU indexed/indirect
preprocessing, layered rendering, broad copy/blit/resolve behavior, queries,
and wider shader/compiler correctness still need implementation and tests.
Some common PanVK code retains upstream limitations. No claim is made that
all unsupported Vulkan operations are comprehensively rejected yet.

<a id="anne_vulkan_midgard-fault-history-data_invalid-on-both-job-slots-resolved"></a>
### Fault history: DATA_INVALID on both job slots (resolved)

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

<a id="anne_vulkan_midgard-root-cause-and-fix"></a>
#### Root cause and fix

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

<a id="anne_vulkan_midgard-the-frames1-result-was-serial-starvation-not-a-driver-fault"></a>
#### The `frames=1` result was serial starvation, not a driver fault

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

The animation and repeated Vulkan/GLES transition gates passed on v5. Next are
the color LOAD preservation test, memory-bound and descriptor-heavy workloads,
and the general copy/blit/resolve and blending paths listed above. Vulkan CTS work is still required before treating this as general
Vulkan support.


<a id="anne_vulkan_midgard-color-load-continuation-v6b-hardware-verified-for-rgba8"></a>
### Color LOAD continuation (v6b, hardware verified for RGBA8)

`0003-panvk-v5-color-preload.patch` replaces the unconditional preload rejection
with a fragment shader and an injected Midgard tiler job. The shader samples the
stored attachment using screen-space coordinates and a v5 texture-pointer
table, then writes the tile buffer before the application's first tiler job.
It uses modern PanVK descriptor pools and internal shader caching, with the
maintained Gallium v5 preloader's job, varying and sampler layout. Injection
retains the polygon-list initialization dependency and is included in batch job
reset/tracing. Errors propagate into command-buffer recording. Clear batches
which require a preload also receive a tagged graphics thread-storage pointer.

This deliberately supports only fixed-function blendable color formats, one
layer and one sample. Unsupported Z/S, MSAA, layered, or shader-converted
preloads still fail explicitly. No Bifrost pre/post-frame descriptors are used.

The client now has `--record-load-only` and `--load-test`. The test draws the
static triangle, then uses LOAD in a second pass to draw a rotated triangle
inside a 64x64 scissor. It compares every pixel outside that patch against a
separately rendered baseline and requires pixels inside to change. This catches
both lost attachment contents and a skipped second draw. The native Vulkan
diagnostic performs this probe once before animation and reports
`panvk: T830 LOAD preservation verified` only after the comparisons pass.

Completed offline checks:

- ICD architectures 5, 6 and 7 build; the full patch series exactly reproduces
  all affected sources from the pinned 24.3.4 archive.
- T830 DRM shim compiles the preload shader and records both passes. Ordinary
  pipeline/recording and epoch-timestamp checks still pass; the shim executes
  no GPU work. The public Vulkan rejection probe also confirms that a 4x-MSAA
  LOAD fails with `VK_ERROR_FEATURE_NOT_PRESENT`, then resetting the same pool
  permits a clear-only recording to succeed.
- The LOAD pixel test passes on the host AMD driver with Khronos validation
  enabled and no validation errors (`checksum=0ed12d7f`). This validates the
  client, not the Midgard implementation.
- 18 renderer/native GLES/appliance tests pass. The combined guest, native
  system and signed phone package build and validate in separate directories.

New package: `out/anne-panvk-preload-v6b`, SHA-256
`29cdb0c1ef30851d1bb8989716791caf89fdf43ad5c322bd21f5273220d50b57`.
This supersedes v6 by also clearing the current-batch pointer after a failed
preload, preventing a failed batch from being closed/linked twice.
It includes the boot-regression fixes and the current workspace's native system.
The v5 package and guest artifacts remain intact. Test with the existing flash
backup/readback procedure and `panvk-gate.py --require-load`; the additional gate
requires the LOAD proof line and more than one animation frame, followed by
successful display restoration and GLES. Evidence lives in
`out/panvk-preload-work/`.

The v6b physical run passed this gate on 23 October 2026:

```text
panvk: T830 static triangle pixels verified checksum=355e1a23
panvk: T830 LOAD preservation verified checksum=6babfed3
GPU: Vulkan status=0 frames=35 checksum=ae9921d display_restored=1
GPU: Mali-T830 frames=295 ...; display restored
```

The full-image comparison retained every pixel outside the second draw's 64x64
scissor, while pixels inside changed. Both passes executed on the T830. The
Vulkan/GLES gate reports no fault markers; the phone reports all eight CPUs
entering EL2 with monitors ready. Installation record and the complete previous
v5 kernel backup are in
`out/anne-device-private/kernel-install-20261023T180251Z/`; the partition readback
matches the v6b SHA above. Hardware evidence is in
`out/mesa-panvk-midgard/phone-preload-v6b/phone-test.json` and its console log.
Two further Vulkan/GLES transition rounds passed on the same installed v6b:
38 Vulkan / 296 GLES frames, then 38 / 296 frames, with distinct animation
checksums and display restoration in every run. The final retained log contains
no fault markers. `repeat-test.json` and `repeat-console.log` in the same evidence
directory record these warm runs and the log collected after the final GLES run.

This verifies this single-layer, single-sample RGBA8 LOAD case. Z/S, MSAA and
layered preloads remain explicitly rejected; other color formats and Vulkan
conformance still need hardware coverage.

---

<a id="anne_vulkan_midgard-original-source-assessment"></a>
### Original source assessment

Earlier source audit, 2026-10-23, before the implementation below. Its
statements about no builds refer to that earlier assessment.

<a id="anne_vulkan_midgard-finding"></a>
### Finding

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

<a id="anne_vulkan_midgard-sources-actually-checked"></a>
### Sources actually checked

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

<a id="anne_vulkan_midgard-what-can-be-reused"></a>
### What can be reused

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

<a id="anne_vulkan_midgard-recommended-experiment-and-acceptance-boundary"></a>
### Recommended experiment and acceptance boundary

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

<a id="gpu_optimization_work"></a>
## GPU optimization checkpoints

<a id="gpu_optimization_work-gpu-optimization-work-in-progress"></a>

Historical GLES optimization work log: eight proposed improvements and a lighter
Linux appliance, retaining the five-second diagnostic and modular API layers.
Later Vulkan work is covered separately above.

Previous phone baseline: `out/anne-gpu-perf-light-v3`, SHA-256
`a78227e7c207a1cdfb273994e948aa06318db59562d5e6a4cfe929df8b806e93`.
Flashed/readback verified in
`out/anne-device-private/kernel-install-20261022T114811Z`.
Physical `diag gpu` cold/warm passed at 52,678 / 33,075 µs; checksum `36244b55`.
Warm run reuses its EGL context, shaders and VBO. USB and shell work after both.
Evidence: `out/anne-device-private/perf-light-v3-serial.log`.

The first lighter image failed in host Mali attachment before the Linux guest
started. It returned E_NOMEM because the clock work tried to map SCTRL a second
time. The page already exists in the Anne bootstrap identity map. Removed that
mapping; v3 boots and renders. The retained failure record is
`out/anne-device-private/perf-light-v2-failure/reconstructed.txt`.

Measured footprint versus the previous working native EGL image:

| Component | Before (bytes) | Lighter (bytes) |
| --- | ---: | ---: |
| Linux Image | 3,604,488 | 3,203,080 |
| Compressed initramfs | 7,805,965 | 7,244,517 |
| Stripped runtime ELF closure | 24,002,392 | 21,692,472 |

Linux now uses three-level 39-bit VAs instead of five-level 52-bit/LPA2, and a
16 KiB log buffer. Arm64 requires NR_CPUS >= 2; only one vCPU is exposed, as
before. PA_BITS=48 is the supported configuration choice. Mesa disables desktop
OpenGL and enables LTO, retaining GLES/Panfrost. Runtime starts at VmRSS 22,460
KiB; guest MemAvailable was 84,732 KiB of MemTotal 125,520 KiB before drawing.

Implementation progress:

1. Shared render target: physically passed in `out/anne-direct-fixed-v1`, SHA
   `44c16bf7a032af1a6938dd780f2edc12879bbbae7b1bc5650b31caf2474d70c6`.
   Cold/warm 10,565 / 5,123 us, CRC `36244b55`; reset/recreate 6,646 us with
   the same CRC. Logs: `direct-fixed-v1-serial.log` and
   `direct-fixed-v1-reset-serial.log` under the private evidence directory.
   Panfrost renders directly into the exported SharedGpu arena through EGLImage;
   no glReadPixels or RGBA conversion. That revision retained the native
   framebuffer CPU copy; direct scanout is described in the follow-up below.
   The target is created in the native context and released in that context.
   Host shadow translation authorizes only owned registered SharedGpu pages;
   GPU page tables remain restricted to owned ordinary guest RAM. Backing
   objects are pinned until GPU reset, and DMA_BUF_IOCTL_SYNC invalidates
   CPU cache lines before checksum/display reads. Hi6250 inherited scanout is
   physical linear D0, 1080x2280 ARGB8888, stride 4320. The later implementation
   uses two independently owned 1 MiB arenas rather than suballocating the
   boot-logo reservation, with explicit pinned capabilities and validated
   display switching. Existing
   `dango_shm.c` exports registered shared regions; Panfrost imports by SG DMA
   address. Never authorize ordinary CPU staging or arbitrary physical memory.
2. Persistent diagnostic context/shader/VBO: implemented and physically passed.
   `diag gpu reset` release/recreate physically passed with the shared target.
3. Overlap/fences: two-slot command storage and asynchronous command submission
   implemented; glFlush and capacity flush enqueue without waiting. Queries and
   glFinish drain queued work.
   GPU/display double buffering was subsequently added with synchronous GPU
   completion and display retirement. Explicit asynchronous GPU fences remain
   pending. The 49,152-byte native VBO exercise physically passed below.
4. Clock ownership: firmware DVFS mailbox 13, command 0x0003030a, MHz payload;
   only vendor OPPs <=480 MHz exposed. Observed OPP index is SCTRL+0x41c bits
   12..15. Runtime requests 480 for present and restores the prior idle OPP.
   Hardware ACK precedes SCBAKDATA update. Added a bounded 20 ms observation
   poll; direct-fixed-v1 logs confirm requested=480 observed=480, followed by
   requested=360 observed=360. Never equate ACK with measured frequency.
5. Shared uploads: implemented. CPU-only 32 KiB arena, two 16 KiB command slots;
   one descriptor/notification replaces 32-byte fragment messages. Native
   uploads use libmochi and guest copies into a private validated batch. Legacy
   inline fallback is capped at 1536 bytes and one in-flight batch when no
   upload arena exists.
6. Synchronous queries: per-frame setup/query traffic removed by retained
   resources; warm diagnostic sends one draw/present batch. Native virtual
   object IDs replace synchronous creates and are now physically tested;
   see the follow-up below.
7. Shadow MMU: bounded table pools reused only after the hardware flush barrier;
   complete guest mapping validation remains. Only changed tables are published.
   Regression verifies zero allocations/publications on identical rebuild,
   one publication for changed permission/removal, and cleanup on failure.
8. Persistent shader cache: implemented and physically passed across reboot.
   Mesa shader cache + pinned zlib enabled. EGL_ANDROID_blob_cache callbacks
   use a mutex-protected 16-entry dictionary (16 KiB per blob). Only the guest
   main thread checkpoints snapshots through a dedicated CPU-only 16 KiB arena.
   The manager uses libmochi FS to read/write the fixed `/data/mesa-blobs-v1`
   path; the guest receives no paths or FS handles. Snapshots have bounded
   lengths/version/checksum; corruption becomes a miss. Mesa keys include the
   driver build/GPU. Worker callbacks never touch SPSC channels or filesystem
   I/O. Checkpoint at command boundaries catches asynchronous updates on the
   next command as needed; no Linux block/filesystem distribution added.

60 tests passed across the graphics/appliance/channel/platform/flash/USB and
system-descriptor suites, including ASan/UBSan cache corruption/bounds and
native encoder/guest decoder probes, and the MMU probe. The fake-DRM shader
compiler check also passed after enabling the Mesa cache.
The fake backend/compiler tests are not hardware rendering evidence.

Vendor sources pinned by `out/anne-gpu-research/vendor-tree.json`:
`clk-hi3xxx.c`, `hisi-clk-mailbox.c`, `hisi_6250_{clock,gpu,ipc}.dtsi` and
`hisi_overlay_utils.c`. Downloaded vendor files/firmware remain private/ignored.

Cache-v1 boot failure: retained log `cache-v1-failure/reconstructed.txt` records
`init: failed service: dango.manager`, before the manager starts; no CPU/GPU fault.
The new FS client lacked `start_after: io.fs`, so init attempted its session
handshake before the provider was running. Cache-v2 adds that dependency.
Manifest validation now checks FS-provider ordering, including transitive paths;
all 13 descriptor tests pass, including the regression.

Current hardware package: `out/anne-direct-cache-v3`, SHA-256
`b876942931c7087806b17889fff1f61617608ba4114942261b1a1177cd02994e`.
Flash/readback verified in `kernel-install-20261022T155606Z`.
Cache-v2 saved a 1,120-byte snapshot; after reboot, v3 loaded it and recorded
five blob-cache callback hits, zero misses. Cold/warm renders 16,409 / 5,300 us,
CRC `36244b55`; no EL0 faults and USB/shell remained available.
Evidence: `direct-cache-v2-serial.log`, `direct-cache-v3-serial.log`.
The cache-enabled stripped runtime closure is 21,891,992 bytes, still below the
original 24,002,392-byte native EGL runtime. The worker thread raises virtual
address-space reservations; VmRSS before draw is ~22.6 MiB. Virtual peak is not
resident memory. No claim that cold render time uniformly improves.

Final cache-v3 context reset/recreate also passed: 12,875 us, CRC `36244b55`,
blob-cache hits rose from five to ten with zero misses. Post-test diag records
zero EL0 faults and USB remains enumerated. Evidence:
`direct-cache-v3-reset-serial.log`.

<a id="gpu_optimization_work-streaming-animation-latency-pass"></a>
### Streaming animation latency pass

Implemented after the 3D triangle animation was added:

- Keep a bounded 480 MHz request across the animation. Restore the original
  lower OPP on end/error, or after five seconds without another request. Higher
  firmware OPPs are left alone. A 300-frame host probe observes one boost and
  one restore, rather than 300 of each. Firmware thermal policy still applies.
- Upload immutable geometry once. New GLES `glVertexAttrib1f` encoding updates
  the shader angle without VBO reallocation or uploads each frame. Decoder
  validates the new payload; host tests check float preservation and malformed
  lengths. No native pointer crosses the stream.
- Streaming presentations retain GPU completion and DMA-buf cache maintenance,
  but defer the full image checksum and shader-cache persistence to explicit
  restore. Static diagnostics keep their checksum. Pbuffer/readback fallback
  still converts the pixel format, with frame logging/checksums deferred.
- Raise the diagnostic pacing ceiling to 60 fps. Print frame count, average and
  maximum synchronous submission time, and mean guest rendering time. These
  exclude pacing sleep and do not measure panel latency.
- Translate/authorize a guest page-table page once per shadow rebuild rather
  than once per PTE load. Keep atomic PTE reads, full leaf authorization, pinned
  SharedGpu backing, and all flush/root barriers. Tests cover table-access
  denial and leaf denial, changed permissions and existing table-pool reuse.

This does not establish an external GPU SMMU route, remove the framebuffer CPU
copy, or bypass MMU mediation. External-stage status remains as documented in
ANNE_GPU_IOMMU_AUDIT.md. USB serial was busy with another console when baseline
measurement was attempted; no hardware speedup is claimed from host tests.

<a id="gpu_optimization_work-batched-creation-upload-exercise-and-coherence-follow-up"></a>
### Batched creation, upload exercise and coherence follow-up

Native shader/program/buffer creates now allocate monotonic opaque names and
enqueue named-create commands. The decoder owns a contiguous 128-record map
of client name, Mesa name and object kind. Names are checked before resolving;
duplicate, cross-kind, exhausted and unknown names fail boundedly. Deleted
slots may be reused, while monotonic client names avoid stale-handle aliasing.
Context destruction clears the map. Legacy raw-name creates use a separate
mode and cannot be mixed with named creates. No host/guest memory ownership or
Mali leaf validation was relaxed. Tests verify creates issue no synchronous
submission, raw-name guessing is rejected in named mode, table capacity is
enforced, and deletion permits reuse. The map is guest-private CPU data, not
GPU-visible page tables or a native capability namespace.

The triangle also batches both shader compiles and checks program link status
once, retaining context/shader/VBO persistence. Added `diag gpu upload`: a
49,152-byte VBO exceeds both shared command slots, with the three vertices at
the end to require the final upload chunk. Normal fresh sessions retain the
small VBO. An upload exercise retains its large VBO until reset. The stress
buffer adds 48 KiB of native diagnostic BSS; it adds no appliance distribution
or Linux service. Setup time is reported separately from frame latency.

Coherence review found that Linux v6.12 invokes the exporter's
`begin_cpu_access` before its generic implicit-fence wait. The Dango exporter
now waits for the applicable reservation fences before cache invalidation,
with a ten-second interruptible timeout and error propagation. The existing
renderer `glFinish()` and DMA-buffer START/END pairing remain. This fixes the
exporter's ordering contract without claiming asynchronous display overlap.
The framebuffer row-copy helper already used AArch64 scalar load/store pairs;
no redundant copy optimization was added.

Validation: all 16 focused native GLES, shell, appliance and framebuffer tests
pass, including ASan/UBSan name-map/decoder and 49 KiB upload coverage. The
modified Linux driver compiled against the pinned v6.12 tree, and the native
phone build/package passed. Installed `out/anne-package-20261022T192500Z`, SHA
`727610241b43d7e1e8b4dbca3da90347179e16a91579c9f33dd7e568bbc74eef`, with full
partition readback verified in `kernel-install-20261022T192722Z`.

| Phone run | Frames / 5 s | Avg submit (us) | Avg render (us) | Setup (us) |
| --- | ---: | ---: | ---: | ---: |
| Cold | 300 | 3813 | 2292 | 35219 |
| Warm | 300 | 3908 | 2256 | 0 |
| 49 KiB upload | 300 | 3804 | 2272 | 9672 |
| Reset/recreate | 300 | 3735 | 2366 | 30828 |

Every run restored the display and returned to msh. The subsequent `diag el2`
completed and reports eight online CPUs entering EL2 with monitors ready.
Pre-update warm runs recorded 297 frames, average submissions 3827/3796 us
and average renders 2266/2261 us. These small samples establish functional
success and fewer setup round trips, not a controlled sustained frame-time
improvement or reduced panel latency. Private evidence is in
`out/anne-device-private/gpu-speedup/{baseline,updated}.{bin,jsonl}`.

That image still used one shared target and a native framebuffer copy. The
subsequent direct-scanout implementation uses two pinned, owned GPU arenas,
GPU completion/cache synchronization, and display-boundary retirement before
slot reuse. The initial physical run completed 151 frames and 152 transitions
in five seconds; the user confirmed animation and console restoration.
See [ANNE_DIRECT_SCANOUT.md](graphics.md#anne_direct_scanout) for the fixed-surface
scope, failure isolation, lifecycle retirement protocol, and later measurements.
Asynchronous GPU/display submission remains future work. External GPU IOMMU
work is set aside at the user's request; validated Mali shadow MMU stays active.
