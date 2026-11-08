# QML rules Ame's compiler is measured against

6 November 2026. Companion to [QML compiler/runtime](QML_UI.md) and the
[UI plan](GRAPHICS_UI_PLAN.md).

Ame compiles a subset of QML. That subset must mean what QML means, or the
difference must be reported as a build error. This document is normative for
three separate things and keeps them apart:

1. **Qt rules.** Behaviour taken from the published Qt 6 QML specification,
   cited per rule.
2. **Supported subset.** The slice Ame currently accepts, with the acceptance
   test that proves it.
3. **Deviations.** Deliberate differences, each with a justification and the
   diagnostic that reports it.

The target is *QML*, not *Qt Quick*: no Qt libraries, no JavaScript engine, no
`QObject`/`QQmlEngine` identity, no Qt Quick scenegraph. A source file that
compiles here is expected to keep its meaning where the subset overlaps, and to
fail loudly everywhere else. Nothing in this document claims Qt Quick
compatibility, conformance, or that unported behaviour is absent from Qt.

## Where the Qt rules come from

| Area | Qt 6 reference |
| --- | --- |
| Object attributes, `property`, static values vs bindings, `readonly`, `default`, change handlers | <https://doc.qt.io/qt-6/qtqml-syntax-objectattributes.html> |
| Bindings and `Qt.binding` | <https://doc.qt.io/qt-6/qtqml-syntax-propertybinding.html> |
| Signal handler naming and parameters | <https://doc.qt.io/qt-6/qtqml-syntax-signals.html> |
| Anchors, anchor lines, sibling/parent restriction, `fill`, `centerIn`, margins, positioning conflicts | <https://doc.qt.io/qt-6/qtquick-positioning-anchors.html> |
| Item `z`, sibling stacking, flipping, `LayoutMirroring` | <https://doc.qt.io/qt-6/qml-qtquick-item.html#z-prop> |
| Positioners (`Row`, `Column`), `spacing`, invisible children | <https://doc.qt.io/qt-6/qtquick-positioning-layouts.html> |

## Qt rules in force

### Declarations

- **R1.** A property is declared as `property <type> <name>: <value>`. Typed
  declarations are preferred over `property var`, which gives up static
  checking and the optimisations a known type permits.
- **R2.** An initial value is either a static value or a binding expression.
  The two are different declarations, not a runtime detail.
- **R3.** `readonly` properties may hold a binding or a static value and cannot
  be assigned after construction.
- **R4.** A binding is re-evaluated when any property its expression reads
  changes. Assignment of a static value to a *writable* property replaces its
  existing binding; a later dependent change no longer re-runs it.
- **R5.** Rebinding a property that already has a static value from imperative
  code requires an explicit dynamic binding (`Qt.binding`); a plain assignment
  does not restore a binding.
- **R6.** Every property gets a change signal and therefore a change handler
  named `on<Property>Changed`, with the first letter of the property
  capitalised. A signal handler is named `on<Signal>` with the signal's first
  letter capitalised.
- **R7.** A `default property` captures unqualified child objects. Declaring one
  is part of the component's public contract, not a layout decision.

### Anchors

- **A1.** An item may anchor to its **parent** or to a **sibling** (an item with
  the same parent). It may not anchor to an arbitrary item elsewhere.
- **A2.** Anchor lines are `left`, `horizontalCenter`, `right`, `top`,
  `verticalCenter`, `bottom`, `baseline`, `fill` and `centerIn`.
- **A3.** `anchors.fill: target` is all four edges at once. It therefore leaves
  `width` and `height` undefined for that item: the size comes from the
  anchors, not from a stored extent.
- **A4.** Anchors and absolute positioning conflict. An item must not combine
  `left`/`right`/`horizontalCenter` anchoring with `x`, nor
  `top`/`bottom`/`verticalCenter` anchoring with `y`, nor `fill`/`centerIn` with
  `x`/`y`/`width`/`height`.
- **A5.** `anchors.margins` adds space outside every applied anchor. For
  `fill` it insets the item; for `centerIn` a positive margin makes the item
  larger than its own size on each side.

### Stacking and order

- **Z1.** `z` orders an item against its **siblings only**. The default is 0.
- **Z2.** Sibling items with equal `z` are stacked in declaration order; the
  later declaration is painted on top.
- **Z3.** A child is painted above its parent regardless of `z` values.

### Positioners

- **P1.** `Row` and `Column` place visible children in declaration order with
  `spacing` between them.
- **P2.** Invisible children occupy no space in a positioner.

## Supported subset

Each rule above has exactly one implementation site and one test. A rule
without both is not claimed.

| Rule | Implementation | Test |
| --- | --- | --- |
| R1, R2 | `scripts/ui/qmlc.py` `object`/`expression`; typed AST node | `test_typed_ir_and_dependency_order` |
| R3 | `readonly` accepted on root properties; handler write rejected | `test_source_diagnostics` |
| R4 | generated `set_*` clears `bound_*`; `update()` re-evaluates in dependency order | `test_handler_reads_fresh_bindings_and_assignment_removes_binding` |
| R6 | change handlers are not generated; `onClicked` follows the naming form | `test_source_diagnostics` |
| A1, A2 (fill, centerIn) | `Compiler.object` target resolution; `Scene` anchor fields | `test_anchors_center_and_margins`, `test_anchor_targets` |
| A3 | `Kind`-independent `fill` sizing in `Scene::measure` | `test_anchors_center_and_margins` |
| A4 | compile-time conflict check in `Compiler.object` | `test_anchor_conflicts` |
| A5 | `anchor_margins` applied on all four sides in `Scene::arrange` | `test_anchors_center_and_margins` |
| Z1–Z3 | `Scene::layout` builds a stable paint order; `draw`/`hit_test` use it | `test_z_paint_order`, `test_z_hit_order` |
| P1, P2 | `Scene::arrange` cursor and visibility filter | `test_native_runtime_and_generated_counter` |

Accepted source is the `import Ame.Ui 1.0` header, `Item`, `Rectangle`,
`Text`, `Row`, `Column`, `Button`, `MouseArea`, root typed properties, the
attribute set in [QML_UI.md](QML_UI.md), the pure expression grammar, and
`onClicked` assignment blocks.

## Deviations, each with its justification

Each row is a deliberate narrowing. All are build errors except where noted, so
no program silently gets different behaviour.

| Qt behaviour | Ame | Why |
| --- | --- | --- |
| `property var`, JavaScript, imperative functions in handlers | Rejected; typed properties and a fixed operator set only | The plan forbids unrestricted embedded code because it defeats dependency analysis and AOT typing. |
| `property alias`, `default property`, attached properties | Rejected | No QObject or scope-chain machinery yet; nothing in the current slice needs them. |
| `Qt.binding` | Rejected | Assignment already clears the binding (R4); an explicit re-binding API arrives with imperative native interfaces. |
| Anchor targets other than `parent`/a direct sibling; anchor lines beyond `fill`/`centerIn` | Rejected with the target or line named | Partial but sound: the two forms cover layout containers and centred chrome. Edge lists land with native resize/configure. |
| `width`/`height` alongside `fill`, `x`/`y` alongside any anchor | Rejected | A4 as a hard error rather than a silent override. |
| `anchors.centerIn` inside `Row`/`Column` | Placed on the parent's padded content box; the positioner still advances its cursor | Qt leaves this combination loosely specified. One deterministic rule beats an undefined one. |
| Arithmetic overflow | Saturates at `int32` limits; extents clamp to 1048576 | Freestanding, no float fallback, and bounded storage is a stated runtime rule. Qt's `int` is also 32-bit but its overflow is not a specified trapping rule. |
| Negative `width`/`height` | Means intrinsic content size | Matches the Qt convention for unset implicit geometry (`-1`). |
| `real`, `double`, `url`, `list`, `object`, enum, `Component`, states, transitions, `Repeater` | Rejected | Deferred by milestone; `Repeater`/keyed lists are C4 and will bring `list` typing with them. |
| `LayoutMirroring` and right-to-left order lists | Absent | No RTL locale in the slice; mirrored layouts are a later text/expansion concern. |
| `z` values outside the sibling set | Compared only within a parent | Z1; a global `z` would need a cross-parent order the painter does not have. |
| String storage | 255 UTF-8 bytes, ASCII rendering only | Fixed component storage; shaping and fallback are C4. |
| Property index of `x`/`y` for a `Text` baseline | No `baseline` anchoring | No font metrics object yet; the bootstrap font has a fixed 16 px cell. |

## Conformance procedure

1. A new supported construct needs its Qt citation, an implementation site, a
   positive test and a negative test for each rejected neighbour.
2. A construct that cannot be implemented faithfully is added to the deviation
   table in the same change as the diagnostic, never left to misbehave.
3. Tests assert diagnostics by source line and column, so a rejected construct
   cannot pass by being silently dropped.
