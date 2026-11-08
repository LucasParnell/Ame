# UI implementation checkpoints

Started 6 November 2026.

Final goal (active): deliver a functional Ame UI framework following
[the UI plan](GRAPHICS_UI_PLAN.md): QML-based UI components, native UI runtime,
layout, text, input and observable state updates, Vulkan rendering, safe native
presentation, and isolated application surfaces managed by Ame's compositor.
A usable compiled shell and application must run from AmeFS using the existing
libmochi app framework and application packages.

Language update, 6 November 2026: the user selected QML for UI source. QML
replaces the proposed custom `.ui` language throughout these checkpoints and
the final goal. The user selected a new Ame-native compiler/runtime. The
initial QML module is `Ame.Ui 1.0`; accepting its supported syntax does not
establish Qt Quick compatibility. See [QML implementation](QML_UI.md).

The goal remains active across checkpoints. Packaging, a compiler prototype,
host tests or a rendering diagnostic alone do not complete it. Mark the final
goal complete only after the integrated shell/application meets the functional
acceptance criteria below and validation evidence is recorded.

The first work slice establishes application packaging. This is a prerequisite
for deploying the compiler's outputs, not completion of the compiler or the
graphics milestones in [the UI plan](GRAPHICS_UI_PLAN.md).

| Checkpoint | Deliverable and acceptance gate | Status |
| --- | --- | --- |
| C0: AmeFS application packages | Versioned bounded container with MXI and named UI resources; explicit offline AmeFS provisioning; launch through existing Process API; immutable child resource access after upload release; malformed-input tests and repeated shell launches | Complete: host sanitizers and QEMU at 1/4 CPUs, EL1/EL2 |
| C1: QML compiler/runtime vertical slice | Typed properties/bindings and handlers, component layout; one interactive QML screen built and packaged by CMake; useful diagnostics, state updates and no work when idle | In progress: compiler, native runtime, CPU reference pixels and AmeFS-packaged app verified; Vulkan screen rendering, display and input service integration pending. Sub-items below |
| C2: GPU/display foundation | Required textured/blended pixel probes; trace current copies/waits; presentable backing experiment; distinct GPU completion and display retirement; console recovery | Pending; can overlap C1 |
| C3: surfaces and compositor | Existing framebuffer owner extended with native surface API and compositor; two isolated app clients; clipping, focus, delayed producer and crash/retirement checks | Pending; requires resource isolation and C2 evidence |
| C4: shell and real app | Packaged shell and settings/demo app with text editing, scrolling and keyed virtualized lists; staged font/shaping support, input and clean teardown | Pending |
| C5: performance acceptance | Static shell, animation, overlap, caret, scroll and failure workloads; before/after timing, copies, memory and queue depth; direct scanout only where proven | Pending |

Final functional acceptance:

- Build QML components into native applications with source-located
  diagnostics and package their assets using the normal application build.
- Launch the shell and a real application from AmeFS through libmochi;
  render their UI and route real input through the compositor to controls.
- Demonstrate state-driven updates, coherent layout/hit testing, text editing,
  scrolling, keyed lists, resize/focus and no frame submission when unchanged.
- Demonstrate two independently owned application surfaces, correct overlap
  and clipping, bounded queues, safe delayed-producer handling and client exit
  with outstanding frames; distinguish GPU completion from display retirement.
- Record integration and target-display evidence, including console recovery
  and GPU/display failure handling. Record measured timings, copies and memory
  for the C5 workloads, with hardware and host/QEMU claims clearly separated.

Later expansion in the UI plan (additional outputs, richer IME/accessibility
integration and optional effects) follows this functional baseline. It does
not replace the baseline acceptance gates.

Record evidence when each gate passes. Host validation, QEMU validation and
handset validation are separate claims. Hardware flashing is not required to
complete C0. Explicit deployment/replacement, per-app writable filesystem
scopes and dependency resolution remain later app-framework work; packages
do not request privileges or install anything during boot.

Next C1 work: connect the compiled QML component to real input and presentation,
while developing the Vulkan 2D renderer and C2 presentation gates. Keep GPU
presentation experiments independent until ownership and retirement pass.

## Current slice

Planned in [UI_WORK_PLAN.md](UI_WORK_PLAN.md); language rules in
[QML_SPEC.md](QML_SPEC.md). This slice serves C1 and C2 together, because the
renderer operations C2 needs are the ones the screen needs. Later steps stay
listed even while unfinished; nothing is marked done without its evidence.

| Sub-item | Gate | Status |
| --- | --- | --- |
| S1.1 Atlas image, staging upload, sampler, descriptor set | Sampled path exists and is created once | Done: `VulkanRenderer::initialize_atlas`, R8_UNORM 128x96, one-shot copy then `SHADER_READ_ONLY_OPTIMAL`, descriptor written once | |
| S1.2 Sampled-quad pipeline with six-vertex unit quad, premultiplied tint, scissor | Glyphs drawn as textured quads | Done: `sampled_quad` + `text`; clip-space conversion stays in C++ so the shader holds no axis or extent policy | |
| S1.3 `sampled_quad` public for later compositor images | Interface present, no second pipeline | Done: takes normalized `Uv`; still one blit pipeline |
| S1.3b Modules created once, driver cache reused | Creation only in `initialize` | Done: no `vkCreateShaderModule` in the frame path | |
| S1.4 Shaders compiled per build and validated | `spirv-val` runs on every module | Done: GLSL in `userland/libui/shaders`, build-time `glslangValidator` + `spirv-val`, no embedded blobs | |
| S1.5 CPU raster reference images unchanged | `counter-before/after.ppm` byte-identical | Done: raster sink untouched; suite passes | |
| S1.6 Draw count for a text screen recorded before/after | Both counts in this file | Done: 305 -> 51 draws for the counter screen, 5.98x fewer | |
| S2.1 `z` property, sibling-only comparison, depth first | Paint and hit order tests | Done: `test_z_paint_order`, `test_z_is_depth_first_within_a_sibling` | |
| S2.2 Stable paint-order array at layout; paint and hit test both use it | Hit order matches paint order | Done: one `order_` array; `test_z_hit_order` | |
| S2.3 `anchors.centerIn` to parent or earlier sibling | Centring test | Done: `test_anchors_center_and_margins` | |
| S2.4 `anchors.margins` | Margin test | Done: inset for `fill`, outward growth for `centerIn` | |
| S2.5 A4 anchor conflicts rejected with the property named | Rejection tests with line and column | Done: `test_anchor_conflicts`, `test_anchor_targets` | |
| S3.1 Example screen uses centring and stacking | `app_ui_demo` builds and packages | Done: centred panel (z -1) and badge (z 5) over the column, `Counter.qml` |
| S3.2 Packaged-app regression still passes at 1/4 CPUs, EL1 and EL2 | QEMU log exists | Done: `out/app-package-smoke/package-cpu{1,4}-el{1,2}/host.log`, twice per config, also proving the four shader resources reach the app |
| S4.1 Present-path bytes, RPC batches, waits and frame time counted | Numbers recorded with their boundary | Pending |

Shader cache: the diagnostic GPU path already persists compiled shaders.
`shader_cache_install` registers bounded `EGL_ANDROID_blob_cache` callbacks on
the display, Mesa routes driver compiles through them, and the bytes land in
AmeFS `/data/mesa-blobs-v1` via `graphics_cache`. That backs GLES and PanVK
alike, so the renderer carries no cache of its own: it creates modules and
pipelines once per instance and the driver reuses the stored entry. The
renderer ships source-derived SPIR-V as package resources rather than embedded
blobs, so a shader edit changes only a resource, and the module stays
device-agnostic (clip-space in, no extent or axis policy).

C2 operation matrix — tick only what S1/S4 actually prove. Sampled images,
descriptors, textured triangles, premultiplied blending and scissor come from
S1; uploads from the atlas path; copy and wait accounting from S4. Image
layouts, render-target preservation, asynchronous completion, presentable
allocation and retirement remain open.

C1 evidence so far: `test_qml_ui.py` validates source-located diagnostics,
binding cycles/types and dependency order, binding override semantics,
sequential handler reads, and generated C++ under ASan/UBSan. Its native scene
tests cover Row/Column layout, clipping, focus/pointer capture, retained input
geometry after a rejected layout, bounded arithmetic and skipping unchanged
frames. Host reference images are in `out/ui/counter-before.ppm` and
`counter-after.ppm`. `app_ui_demo` builds the QML-generated native executable
and packages its versioned metadata as `ui-demo.amp`. The AmeFS package system
regression launches it twice at 1/4 CPUs and EL1/EL2; the app checks state,
bindings, raster pixels and idle behavior with scripted runtime events. These
are offscreen checks, not real input routing or Vulkan display validation;
C1 remains open and the final UI goal remains active.

C0 evidence: `test_app_package.py` passes seven host tests under ASan/UBSan,
including malformed tables, truncation, duplicate/unsafe names and bounds.
`test_app_package_system.py` launches a package with a multi-arena resource
twice in each of four QEMU configurations (1/4 CPUs, EL1/EL2), checks resource
bytes after upload release, rejects a truncated package, and verifies terminal
mappings and filesystem access survive. Logs are under
`out/app-package-smoke/package-cpu*-el*/`. The explicit `app_package_demo`
native target builds successfully. No handset validation is claimed.

The broader `test_app_system.py` regression currently stops at its benchmark's
trusted-worker spawn request: `kernel/syscall/dispatch.cc` rejects a sandboxed
parent creating trusted children. This occurs before its package checks.
The dedicated package regression exercises the same shell `cmd_exec` and
libmochi Process path without that benchmark policy dependency. Resolving the
benchmark's privilege requirements remains separate follow-up work.
