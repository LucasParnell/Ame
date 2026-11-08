# Ame-native QML UI

The UI source language is QML, compiled into C++ for Ame's new native runtime.
The implementation uses the `Ame.Ui 1.0` module. It has no Qt dependency; its
current supported subset is described here so applications get explicit
diagnostics rather than an implied Qt Quick compatibility promise.

The example is [Counter.qml](../userland/app/ui-demo/Counter.qml). Build and
package it through the existing application build:

```sh
cmake --build out/qemu-dev --target app_ui_demo
```

`userland/ui-demo.amp` in the build directory contains the MXI executable and
`ui/Counter.json` metadata. Provision it with `mkamefs.py create --app
ui-demo.amp=PATH` as described in [application packages](APP_PACKAGES.md).
`exec /data/apps/ui-demo.amp` currently runs a scripted offscreen runtime check
and exits. It does not yet open a window or subscribe to real input.

The host compiler is `scripts/ui/qmlc.py SOURCE.qml --header OUTPUT.h
--metadata OUTPUT.json`. `ame_qml_component(TARGET SOURCE.qml)` in CMake
generates both artifacts, includes the generated header directory and links
the header-only `ame_ui` runtime interface. The generated constructor builds
the native object tree; no source parser runs in the app. Construct native
components from `_start` (use local static storage for larger components):
Ame's freestanding startup does not execute global constructors.

The filename must begin with an uppercase ASCII letter and supplies the class
name (`ame::qml::Counter` for `Counter.qml`). Metadata schema and runtime ABI
are version 1. Metadata includes object IDs, parents, source lines, property
types and binding dependencies in topological order. Generated C++ includes
source mappings and a runtime ABI check.

Current source support:

- `import Ame.Ui 1.0`, nested `Item`, `Rectangle`, `Text`, `Row`, `Column`,
  `Button` and `MouseArea` objects, and component-scoped unique `id` names.
- Root `property int`, `bool`, `string` and `color` declarations, optionally
  `readonly`, with typed literal defaults or pure binding expressions.
- Numeric `x`, `y`, `z`, `width`, `height`, `spacing`, `padding`; boolean
  `visible`, `enabled`, `clip`; Text/Button `text`; and `color` in `#RRGGBB` or
  `#AARRGGBB` notation. `anchors.fill` and `anchors.centerIn` accept `parent`
  or a previously declared sibling; `anchors.margins` insets a fill and grows a
  centred item outward. Anchors reject the conflicting absolute geometry
  (`QML_SPEC.md` A4). `z` orders siblings only, ties keep declaration order and
  children always paint above their parent.
- Pure expressions: property and explicitly bound object-property references,
  parentheses, integer `+`, `-`, `*`, comparisons, boolean operators, ternary
  selection, string concatenation/coercion and `String(value)`. No arbitrary
  JavaScript or native-code evaluation is permitted in bindings.
- `onClicked: { ... }` on Button/MouseArea, with typed root-property assignments
  (`=`, `+=`, `-=`, `++`, `--`). A later statement sees bindings updated by
  preceding assignments. Layout and drawing remain deferred until frame work.

Assigning a writable property removes its previous binding, as QML code
expects; readonly properties retain theirs. Generated setters make mutations
observable. `update()` evaluates the static dependency graph once after a
change; assigning an unchanged value does not invalidate visual content.
Native callers use setters, then `update()` or `prepare()` before reading
derived values. Unknown types/names, unsupported syntax, type errors, duplicate
IDs/bindings and detectable cycles produce source-located build errors.

Layout rules for this initial runtime:

- Explicit dimensions are pixel sizes; absent dimensions use intrinsic content
  size. A Text uses bootstrap 8x16 metrics; a Button includes its text content.
  Item/Rectangle/MouseArea intrinsic size bounds their positioned children.
- Row adds visible child widths plus spacing and takes maximum child height;
  Column adds visible child heights plus spacing and takes maximum width.
  Padding is added on both axes. Children retain source order. Invisible
  children occupy no layout space. Row/Column children may have local offsets.
- `anchors.fill` uses available parent content size; a content-sized parent
  with a filling child is a sizing cycle and rejects that layout. The previous
  committed input geometry is preserved. Unsupported implicit-geometry binding
  references are diagnosed during compilation rather than read as stale values.
- Viewport bounds always clip input/rendering; `clip: true` additionally clips
  descendants. Hit testing uses reverse paint order and committed geometry.
  Press/release activates only the captured target when released inside it.
  Cancellation drops capture. Disabled/hidden/clipped controls cannot take
  focus or activation after their layout is committed.

Storage is bounded: 256 static objects, 12 nesting levels, 64 expression
nesting levels and 255-byte string values per component value. Integer
arithmetic saturates at signed 32-bit limits and pixel extents at 1,048,576.
These limits bound component storage and parser/layout recursion; native stack
usage still needs validation with the target compiler and actual screen. Larger strings
formed at runtime truncate to the fixed byte capacity. Text storage accepts
UTF-8, but the current bootstrap renderer only draws ASCII; shaping, bidi,
fallback, grapheme handling and text editing remain C4 work.

Shaders are client-side GLSL in `userland/libui/shaders`, compiled to validated
Vulkan 1.0 modules per build and packaged as `ui/*.spv` resources rather than
embedded arrays. The renderer creates modules and pipelines once and the
diagnostic GPU's AmeFS shader cache persists the driver compile.

`Scene::layout()` commits geometry only after a successful complete layout.
Unchanged size/state does no new layout. `needs_frame()` indicates pending
visual changes. Rendering goes through a sink's rectangle/text methods;
applications call `presented()` only after the backend accepts the completed
frame. Backend failure leaves invalidation pending for retry. No graphics
handles appear in controls. The CPU reference sink uses opaque ARGB8 output
and source-over blending of encoded RGB channels, without claiming
linear-light correctness. It reuses the existing VGA font for bootstrap
validation and is separate from the planned Vulkan 2D renderer.

Still required for the functional framework: reusable component instances and
typed native interfaces; conditional/keyed dynamic content and virtualized
lists; richer sizing/alignment; text system/editing; animation and semantic
accessibility metadata; real input service routing, Vulkan rendering and
surface/presentation integration. These are tracked by
[the active checkpoints](UI_CHECKPOINTS.md).

Validation:

```sh
.venv/bin/python -m unittest discover -s tests -p test_qml_ui.py -v
.venv/bin/python -m unittest discover -s tests -p test_app_package_system.py -v
```

The host tests render before/after counter images and check runtime failures
under sanitizers. The QEMU regression runs the packaged generated app through
the production AmeFS/shell/Process path twice at 1/4 CPUs and EL1/EL2. Scripted
input inside that app proves runtime dispatch, not physical input delivery or
target-display behavior.
