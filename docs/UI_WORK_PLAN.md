# UI work plan: C1/C2 vertical slice

6 November 2026. Executable plan for the next slice of
[the UI plan](GRAPHICS_UI_PLAN.md), against the gates in
[the checkpoints](UI_CHECKPOINTS.md). Spec rules come from [QML_SPEC.md](QML_SPEC.md).

## Goal of this slice

Move C1 from "offscreen pixels" to "a QML screen drawn by the real 2D renderer,
with correct stacking and anchoring", and move C2 from "nothing" to "the exact
compositor pixel operations proven and measured". One slice serves both because
the renderer work is the same work: the checkpoint list says C1 and C2 may
overlap.

C1 needs a renderer that can draw a screen; C2 needs the operation matrix. Doing
the matrix first, as renderer features the screen actually uses, is cheaper than
proving it later against a renderer that already exists. The compositor,
surfaces and scanout come after both.

## What exists, precisely

Read from the tree, not assumed:

- `userland/libui/vulkan.cc` clears one target and draws one **fullscreen
  triangle per clipped rectangle** with a viewport/scissor trick and a fragment
  push constant. No vertex buffer, no image, no sampler, no descriptor set,
  no textured quad.
- `VulkanRenderer::text` decomposes each glyph into up to 128 solid rectangles.
  A 40-glyph line is thousands of draw calls before any shape work.
- `Scene::draw` paints in declaration order. `Scene::hit_test` walks indices in
  reverse. There is no `z`, so declaration order is the only stacking key.
- The compiler accepts only `anchors.fill: parent`, and rejects it alongside
  `x`/`y`/`width`/`height` with no alternative. A centred control cannot be
  expressed at all.
- Presentation is still fixed 512×512, `vkQueueWaitIdle`, invalidate, per-pixel
  convert, FNV checksum, guest `memcpy`, DMA_BUF sync.

## Sequence

Each step states its acceptance evidence and its stop rule. A step is done only
when its evidence exists in the tree; comments and intent do not count.

### S1. Glyph atlas and sampled-quad pipeline

Add a bounded R8_UNORM atlas covering ASCII 0x20–0x7e at 8×16 (95 glyphs,
128×96 px, 12288 bytes), a host-visible staging buffer, and a one-shot
`vkCmdCopyBufferToImage` during `initialize`. Transition to
`SHADER_READ_ONLY_OPTIMAL`, create one immutable sampler, one descriptor set
layout, descriptor set and a second graphics pipeline whose vertex stage reads a
static six-vertex unit quad and whose push constants carry the destination rect,
UV rect and premultiplied tint.

`text()` becomes: clip the run, emit one `sampled_quad` per covered glyph. Add
`sampled_quad` to the public interface so the compositor can draw images through
the same pipeline without a new one.

Rules held: premultiplied alpha with `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` (the
existing factors already assume premultiplied source), scissor to the
intersection, destination pixels as clip-space input so extreme offscreen client
coordinates never reach viewport bounds, descriptor written once and never
per frame, `kMaxDraws` still enforced.

Shaders are client-owned GLSL, compiled per build to validated Vulkan 1.0
modules and shipped as application resources, never as arrays baked into the
executable. Modules and pipelines are created once in `initialize`; the
diagnostic GPU's existing AmeFS blob cache persists the driver's compile, so a
first run pays and later runs do not.

**Evidence:** `glslangValidator` and `spirv-val` run on every module as part of
the build. Host reference images stay byte-identical for the CPU raster sink,
proving the Vulkan change is behaviour-preserving rather than
behaviour-changing. `draws()` for a text screen drops by a measurable factor;
record both counts in the checkpoint file.

**Stop rule:** do not add mipmaps, subpixel positioning, sRGB formats, or a
second texture binding to this pipeline. Sampling, one descriptor, one quad is
the whole scope.

### S2. Stacking and anchoring

Runtime: a `z` property compared only among siblings, a stable paint-order array
built at layout time, painting in that order, and reverse hit testing over it so
input matches pixels. Children stay above parents (Z3) because the order is
built parent-then-sorted-children.

Anchors: `anchors.centerIn` to the parent or an earlier declared sibling, plus
`anchors.margins`, both resolved in `arrange` against the parent's padded
content box. Compiler gains `z`, `anchors.centerIn`, `anchors.margins`, and the
A4 conflict errors with the offending property named in the diagnostic.

**Evidence:** `test_z_paint_order`, `test_z_hit_order`,
`test_anchors_center_and_margins`, `test_anchor_conflicts`,
`test_anchor_targets`; `counter-after.ppm` visibly carries the centred element.
Anchor rejection tests assert line *and* column.

**Stop rule:** no `left`/`right`/`top`/`bottom` lines, no cross-parent targets,
no `LayoutMirroring`. Add them when native resize needs them.

### S3. Screen uses both

Extend `Counter.qml` to a centred control column with a stacked overlay, so the
shipped example exercises anchors and `z` instead of only `anchors.fill`. Keep
`app_ui_demo`'s scripted event checks and its idle assertion.

**Evidence:** `app_ui_demo` builds and packages; the packaged-app QEMU
regression still launches it twice at 1/4 CPUs on EL1 and EL2.

### S4. Copy timeline, measured

Instrument the present path with counters and record them, so the readback
claim in the plan stops being a source observation: RPC batches, synchronous
waits, CPU bytes read, CPU bytes written, total frame wall time at 512×512.

**Evidence:** a host or QEMU run with the numbers in the checkpoint file,
labelled with exactly which boundary produced them. No handset claim.

## Explicitly not in this slice

Presentable allocation, image import, scanout geometry, input service routing,
surfaces, the compositor, text shaping and virtualized lists. Each has its own
gate and would otherwise sit half-built. The slice must not be widened to
compensate for them.

## Risk register

| Risk | Consequence | Mitigation |
| --- | --- | --- |
| PanVK Midgard rejects the sampler/descriptor path | Renderer initializes, first glyph draw is wrong or faults | Keep the rectangle pipeline untouched and fall back per-`text` call; S1 evidence is pixel-level, not "it did not crash" |
| Atlas upload ordering | Glyph reads before the copy completes | Same command buffer, `TRANSFER_WRITE` → `SHADER_READ` barrier before the first render pass |
| Paint order and hit order diverge | Click lands on the wrong control | One array feeds both; `test_z_hit_order` asserts the visible topmost item is the hit |
| Anchor cycle through a sibling | Layout never settles | Sibling targets must be earlier declarations; the compiler proves it |
| Saturation hides a layout bug | Element clamps instead of erroring | `LayoutError::None` still required; existing extent tests keep the clamp covered |

## Definition of done for the slice

- Every shader module passes `spirv-val` in the build; the four modules are package resources.
- The full `test_qml_ui.py` suite passes under ASan/UBSan.
- `app_ui_demo` builds, packages and still passes the packaged-app regression.
- `docs/UI_CHECKPOINTS.md` records C1 sub-items as done with pointers to that
  evidence, and C2's operation matrix is ticked to the extent actually proven.
- No claim exceeds the boundary that produced it: host, QEMU and handset stay
  separate.
