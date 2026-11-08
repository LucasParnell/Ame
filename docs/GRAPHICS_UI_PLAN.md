# Ame graphics and compiled UI plan

1 November 2026. Replaces the proposed compositor/UI roadmap in `ame_mochi_graphics_plan.md`.

6 November 2026 language update: UI source uses QML with a new Ame-native
compiler/runtime, as requested by the user. This supersedes the custom `.ui`
source-language proposal. The initial module is `Ame.Ui 1.0`; its supported
types and expression subset are explicit, and unsupported QML is diagnosed.
See [QML compiler/runtime](QML_UI.md). The functional UI framework remains the
final goal, with AmeFS application storage and libmochi startup/supervision.

## Direction

Build Ame's own compositor and its own QML compiler/runtime. Use the existing Vulkan frontend and Dango GPU appliance, improving either wherever the implementation needs it. Do not port Gamescope, Smithay, wlroots, or another compositor. Reuse specialist libraries for text and build tooling where they save substantial work.

The architecture should let compiled UI, hand-written native applications, games and video share the same surface system. A UI language must not become a requirement for submitting a window. Compositor policy belongs to Ame; Linux remains an implementation detail of the current GPU backend.

The goal is a usable compiled-language shell and application, followed by demonstrated improvements in latency, bandwidth and power. Neither a compiler nor zero-copy sharing automatically produces those improvements.

Implementation started 6 November 2026. Applications live on AmeFS and use
the existing libmochi application startup, sandbox, stdout and supervision
framework. Application packages may carry compiled UI metadata, shaders,
fonts and images alongside the MXI executable. Generated C++ remains part of
the executable; a package resource does not grant executable or service
authority. See [application packages](APP_PACKAGES.md) and the
[implementation checkpoints](UI_CHECKPOINTS.md).

## What the existing tree actually provides

The proposal underestimates existing work and overestimates how general it is:

- `userland/libvulkan/` already implements a freestanding Vulkan RPC frontend. `linux-artifacts/panfrost-renderer/vulkan_rpc.cc` executes requests through Mesa in the appliance. Extend this path rather than introduce another graphics API.
- `userland/libgraphics/transport.cc` already provides bounded batches, command slots, tickets and completion handling. Some public paths still synchronously exchange requests. Audit actual round trips instead of assuming the ring makes all rendering asynchronous.
- `userland/libvulkan/memory.cc` implements mapped memory through local staging and explicit upload/download. Large per-frame instance-buffer flushes can therefore be expensive even when Vulkan recording looks cheap.
- `ameVkPresentDiagnosticImage` is a fixed 512×512 diagnostic, not a window system. `vulkan_rpc_present.h` calls `vkQueueWaitIdle`, maps/invalidate-reads a linear image, and converts/copies its pixels into a CPU buffer while checksumming. This is the first presentation bottleneck to remove; it is not proof of Vulkan direct scanout.
- The GLES path already has a verified two-arena native DSS scanout diagnostic, cache synchronization and conservative retirement/restoration handling. It is limited to a particular inherited linear RGB32 pipeline. Use its ownership rules and board implementation; do not generalize its hardware claims to arbitrary Vulkan images or panel-sized targets.
- PanVK on this Midgard hardware is Ame's experimental port. Selected pixel and hardware tests are useful evidence, not full Vulkan coverage or conformance. The maintained patch series and its provenance must remain reproducible.

Repository evidence: [graphics status](graphics.md), [PanVK patch status](../third_party/mesa/panvk-midgard/README.md), [frontend presentation](../userland/libvulkan/present.cc), [guest presentation](../linux-artifacts/panfrost-renderer/vulkan_rpc_present.h), [mapped memory](../userland/libvulkan/memory.cc), [transport](../userland/libgraphics/transport.cc), [framebuffer service](../userland/drivers/framebuffer/main.cc).

These are source/document observations, not fresh handset measurements. Upstream's [Panfrost support table](https://docs.mesa3d.org/drivers/panfrost.html) does not establish support for Ame's custom Midgard PanVK branch.

## Assessment of the original proposal

| Proposal | Decision | Reason |
| --- | --- | --- |
| Own surface protocol, ownership, quotas and explicit completion | Keep | Essential correctness and isolation; cannot be optimized away. |
| Own Vulkan compositor | Keep; write directly | Textured layers, clipping and alpha are a manageable initial renderer. Porting a desktop compositor brings platform assumptions and maintenance work. |
| Transplant Gamescope renderer/backend code | Remove | Its architecture is not Ame's. Study techniques only; implement the small required renderer and presenter directly. |
| libliftoff/native plane solver | Defer | No general native plane API is established. Write a fullscreen eligibility check first; solve multiple-plane assignment only for verified hardware. |
| New display service immediately | Change | Extend/refactor the existing framebuffer/scanout owner first. A new name and process are not prerequisites for a sound presentation interface. |
| New universal graphics abstraction | Remove | Keep narrow image/sync/presentation contracts and the existing Vulkan interface. |
| Language compiler | Core deliverable | Build a real typed compiler early, alongside the runtime. It should generate native code and useful diagnostics. |
| Compiler after a complete desktop toolkit | Change | Develop one compiled screen vertically; let real screens drive language and runtime design. |
| General LLVM backend and vendor GPU machine-code compiler | Remove from initial scope | Emit C++ to the existing toolchain; compile a small shader library to SPIR-V on the host. Driver pipeline compilation remains separate. |
| Clay as required UI layout engine | Remove as default | Own compiler/runtime can begin with row, column, stack and explicit sizing. Wrapping an immediate declaration API and later replacing it risks doing layout twice. |
| HarfBuzz, FreeType, Unicode support | Keep, stage ports | Text correctness is specialist work; writing replacements is poor use of time. |
| Cross-process shared glyph atlas | Remove initially | Adds ownership, eviction, synchronization and isolation problems. Start per application with bounded caches. |
| vkvg, MSDF, VMA, HDR/ICC, blur | Optional later | Add only against a demonstrated feature or bottleneck; do not make them initial dependencies. |
| Native UI scene fusion | Defer | A second cross-process rendering protocol increases trusted complexity before its value is known. |
| Incremental rendering and no work when idle | Keep; stage complexity | First skip unchanged frames and cache text/resources. Add fine-grained layout and partial redraw after correctness. |
| Extensive performance instrumentation | Keep a focused subset | Frame timelines, RPC/copy counts, CPU/GPU time and memory are actionable. Do not block progress on unavailable bandwidth/power counters. |

Clay remains a credible fallback if layout grows beyond the intended scope. Its upstream documentation describes arena-backed, renderer-independent layout, but its declaration loop does not by itself provide this compiler's dependency-directed updates. See [Clay documentation](https://github.com/nicbarker/clay). This is an engineering choice, not a claim that hand-written layout always outperforms Clay.

## Architecture and boundaries

```text
.qml source -> host compiler -> generated C++ + metadata + assets
                                   |
                         application UI runtime
                  state / layout / text / hit testing
                                   |
                         app Vulkan 2D renderer
                                   |
Native Vulkan apps ----------------+--> native surface commits
                                            |
                                 Ame compositor and policy
                               scene / focus / scheduling
                                            |
                            Vulkan composition or direct scanout
                                            |
                              native display/scanout owner

Vulkan operations -> existing bounded RPC -> Dango GPU backend -> PanVK
```

The compositor is a native Mochi service. The current GPU implementation may execute its commands in Dango. The native display owner alone controls DSS and retirement. Start with compositor policy as a module in the same service; do not create extra IPC hops for each logical responsibility.

Keep three contracts independent:

1. **Surface contract:** client-owned content, configure/acknowledge, atomic commit, release, frame feedback and input delivery. Global stacking, focus and privileged overlays require shell authority.
2. **Graphics resource contract:** typed, owner-scoped image and completion tokens, backing references, format/extent/usage metadata and access rights. Never expose physical addresses, guest pointers or raw cross-device Vulkan handles as authority.
3. **Presentation contract:** supported targets, queueing, completion, restoration and retirement. Hardware-specific limitations are explicit capabilities.

Initial image sharing should target the GPU broker's actual ownership model. Two native clients do not automatically share a Vulkan device merely because both use the appliance. Establish broker-managed shareable allocations and valid per-device imports, or a deliberately broker-owned composition context. Do not pass one client's VkImage token to another device. Standard Vulkan external-memory extensions are one possible mechanism, not a prerequisite for an Ame-native allocation protocol.

Prefer a small public `libame-surface` API first. Standard Vulkan WSI can wrap that API later if application compatibility requires it. Do not implement an entire WSI stack just to display the shell.

## GPU work is part of this plan

The frontend, transport and Midgard driver are fresh code and may change. Preserve verified behavior, not accidental diagnostic constraints.

Before the compositor renderer grows, implement and measure the exact operations it needs: sampled images, uploads, descriptors, textured triangles, premultiplied alpha blending, scissor, image layouts/barriers, render-target preservation, queue submission/completion and target retirement.

Use that matrix to identify frontend codec gaps separately from driver gaps. Add focused pixel/lifecycle coverage for any new path. Keep driver changes as reproducible patches against the pinned Mesa source. No broad Vulkan-completeness or CTS campaign is required before a small compositor can work correctly; unsupported necessary features must be fixed or deliberately bypassed.

Priority changes:

1. Replace diagnostic map/readback/conversion/checksum presentation with a presentable allocation path connected to native scanout. Prove Vulkan can render into the backing with the required layout, channel order, cache visibility and lifetime. If it cannot yet, add import/allocation support or use a counted fallback copy. A GPU conversion/blit may be preferable to CPU readback, but must be measured.
2. Generalize dimensions and target allocation only within verified DSS constraints. Panel-size scanout is a separate gate from 512×512 success. Keep the console recovery path.
3. Add asynchronous submission/completion where needed. Start with ordered service tokens backed by the completion primitives the branch supports; do not require timeline semaphores. Keep GPU completion and display retirement distinct.
4. Batch recording and uploads; avoid synchronous queries in frame production. Inspect existing generated frontend behavior before changing transport. Immutable geometry, persistent buffers and small state updates come first.
5. Optimize staging only if traces justify it. Shared upload arenas need bounded immutable consumption/lifetime rules and cache maintenance. Zero-copy command data must not let a client mutate validated commands before execution.

A stalled GPU or display must not authorize early reuse. Timeouts retain/quarantine live backing until hardware quiescence is proven. A GPU restart invalidates affected resources and forces reconstruction; it is not transparent success.

## Own compositor: smallest useful implementation

Write a compact scene manager, renderer and presenter directly in Ame's C++ style. Use existing arena/ring infrastructure, with finite per-client quotas and generation-tagged IDs. Begin with one output, fixed refresh and a small set of formats.

Surface state includes backing image, source/destination rectangles, alpha, clip, transform, role and serial. Copy validated pending state into compositor-owned records and latch it atomically. Never traverse mutable client scene memory.

Start with full-output redraw on visual changes and a graphics render pass drawing ordered textured quads. Precreate a few pipelines and samplers; reuse buffers and descriptors after completion. Use premultiplied alpha with an explicit colour/alpha convention. Include known colour ramps and blending reference images in renderer validation. Do not accidentally blend encoded sRGB values while claiming linear-light correctness; if the first backend is restricted, document the restriction.

No unchanged frame submission. Hide fully occluded surfaces using simple opaque-rectangle cases first. Defer general region subtraction and partial target preservation: a full redraw can be cheaper than loads and fragmented scissors on a tile GPU. Benchmark rather than assume.

Direct scanout is an optional bypass when one visible image satisfies every display constraint. Fallback composition must always remain correct. Native overlays and plane allocation follow only once a real hardware API and workload justify them.

A client can declare opaque content within its owned bounds; a false hint must not affect security-sensitive content outside its authority. Validate geometry and use privileged roles for system overlays. Buffer references outlive client crashes until GPU/display use ends.

Initially retain at most a bounded pending candidate per surface plus the displayed image and in-flight references. Replacing an unlatched commit does not release a still-running producer's backing. Define dropped-frame feedback explicitly.

Drive presentation from available output feedback, sleep when idle, and choose the newest ready frame. Correctly retaining the previous frame is preferable to blocking every client behind an unfinished image. Add deadline prediction only after stage timing is trustworthy. Polling remains bounded until verified interrupt support replaces it.

## Ame-native QML compiler and runtime

The compiler is a first-class project from the first vertical slice. AOT compilation is useful for type checks, direct state access, static metadata and removing runtime parsing; dynamic text/layout/rendering still require runtime work.

```text
.qml -> parser with source spans -> typed AST -> component/dependency IR
    -> generated C++ and static tables -> existing Clang build -> native app
    -> asset manifest and fixed shader-library references
```

Initial language:

- Components, typed properties, local state, constants and explicit event handlers.
- Row, column and stack containers; fixed/content/fill sizing, padding, gap, alignment and min/max bounds.
- Text, image, rectangle, clipping and reusable style tokens.
- Conditional content and keyed repeated items with defined state lifetime.
- Pure typed binding expressions with a deliberately small expression grammar.
- Source-located diagnostics for unknown names, type errors, duplicate keys and detectable binding cycles.

Call native application logic through explicit typed interfaces. Avoid unrestricted embedded C++ expressions in bindings: they defeat dependency analysis. Event handlers can call registered application functions; state mutations must use generated setters/batches so invalidation is observable.

Generated code stores compact component state and static metadata. State writes mark dependency/domain bits; one event turn batches changes. Initially rerun layout for an affected component root, not a bespoke incremental constraint solver. Cache shaping and resource creation. Keep hit testing synchronized with the committed layout, and process animation only while active. Introduce finer layout invalidation only when measured.

Layout semantics must be specified before optimization: content measurement versus available space, percentage/fill behavior, overflow, clipping and cycle handling. Begin with deterministic rules and reject unsupported cyclic sizing rather than silently iterate indefinitely. Share the same layout functions between generated applications and host test tools.

Keep node identity, state identity and render-resource lifetime separate. Reordering a keyed list preserves item state; deleting an item retires resources safely. Virtualized lists belong in the first useful toolkit, implemented by limiting instantiated visible items plus overscan, not by generating thousands of hidden nodes.

Native C++ application code can also call the runtime. This gives a reference for testing compiler output and supports custom controls without adding a language feature for every case. The public UI abstraction should expose styles, layout and custom drawing, not Vulkan descriptors.

Do not build a custom LLVM backend, global optimizer, live editor, package manager or portable GPU ISA compiler initially. Emit readable C++ with source mappings. Build a small shader library on the host and create/cache pipelines through the driver with compatibility checks.

## Text, interaction and assets

Reuse HarfBuzz and FreeType with scoped freestanding ports and explicit allocator/platform hooks. HarfBuzz performs shaping; it does not provide the entire paragraph-layout/editing system. See the [HarfBuzz manual](https://harfbuzz.github.io/what-does-harfbuzz-do.html).

Provide line/word breaks, grapheme-aware caret movement, font fallback and bidi processing as explicit text-system responsibilities, using maintained Unicode libraries where practical. Choose and pin those libraries during the text milestone after checking their build requirements; do not invent Unicode algorithms to avoid dependencies.

Start with per-application grayscale glyph atlases, bounded by bytes and with in-flight references protecting eviction. Keep shaped-run caches keyed by font, size, text, script/direction and relevant features. Font subsets may package fixed shell labels; dynamic user text still requires fallback coverage.

Ship a usable text field with selection, clipboard and composition-state interfaces. Initial language coverage can be bounded and documented; do not claim complete international text support from shaping alone. Design semantic roles, stable control identity, labels and focus order early so accessibility does not later require a new widget model. Full IME engines and assistive technology integration can follow that contract.

Use build-time asset packing and versioned manifests. Asset/shader changes invalidate the appropriate caches. Bake simple icon assets where appropriate; defer general vector-path effects and MSDF until quality or scaling requires them.

## Implementation sequence and acceptance gates

| Milestone | Deliverable | Exit evidence |
| --- | --- | --- |
| 1. GPU/display foundation | Extend current probes with textured blending, timeline tracing, presentable allocation and separate render/display completion | Hardware pixels correct; actual copies identified; repeated presentation and console recovery; no fault markers or premature reuse |
| 2. Compiler vertical slice | Ame-native QML compiler, generated C++, minimal runtime and one interactive screen using the available diagnostic/presentation path | Build through existing toolchain; source-located error tests; state change updates screen; unchanged screen sleeps |
| 3. Native compositor | Own service and surface client API; two clients, Vulkan quad composition, focus/input routing and trusted shell role | Correct overlap, clipping, resize/configure, client death and delayed producer; bounded queues; independent per-client ownership |
| 4. Compiled shell and real app | Shell plus settings/demo app; text field, scrolling, keyed virtualized list, font/text libraries | Real input, coherent hit testing, resizing, font fallback subset and clean teardown; no per-frame resource creation in steady animation |
| 5. Measured optimizations | Eligible direct scanout, upload batching, opaque culling, optional damage, scheduler tuning | Before/after traces show gains without visual, latency or ownership regressions |
| 6. Expansion | Additional outputs/rotation, richer text/IME/accessibility integration and effects as needed | Each feature has an actual use case, backend coverage and a bounded resource contract |

Milestones 1 and 2 can overlap: the compiler/runtime can be exercised with host rendering and the existing diagnostic before generalized scanout is ready. Native multi-client resource isolation is a hard gate for milestone 3. Do not advertise multi-app graphics while the provisioned frontend is still effectively a single-client diagnostic.

## Measurements and stop rules

Record allocations, CPU submission/validation time, RPC batches and synchronous waits, upload bytes, CPU/GPU copy bytes, GPU elapsed time where available, queued-frame depth, actual present completion and frame misses. Report p50/p95/p99 over explicit workloads. Input-event-to-present timestamps measure software latency; claiming input-to-photon requires an external panel measurement.

For the 1080×2280 panel, one tightly packed RGBA8 image is 9,849,600 bytes (about 9.39 MiB). Two targets are about 18.79 MiB before app surfaces, depth, atlases and driver storage. One full-frame CPU read plus write at 60 Hz is about 1.18 GB/s of pixel traffic before cache effects and display reads. This is an accounting estimate, not measured DDR bandwidth. It explains why eliminating the current readback matters more than sophisticated widget compilation optimizations at this stage.

Required workloads: static shell; fullscreen animation; two overlapping windows; small caret updates; scrolling text/list; resize; rapid state changes; slow producer; client exit with frames outstanding; GPU/display failure. Compare full redraw against damage on the same target/backend before adopting it. Measure native and appliance boundaries separately.

Stop adding infrastructure when it cannot point to a required feature, a measured bottleneck or a correctness obligation. No universal performance winner can be selected without target traces. This plan keeps the compiler, renderer and compositor independently replaceable while making those measurements possible.

## First patch sets

1. Document the current presentation/copy timeline beside the existing probes; add the missing compositor pixel cases. Keep diagnostic checksums outside normal production frames.
2. Add a presentable-image allocation/registration experiment connecting Vulkan to the verified native target lifecycle. Add safe completion/retirement coverage before broadening target geometry.
3. Add the host compiler executable, language examples, typed IR and C++ generation for one stateful screen; compile it against a minimal runtime using the existing build.
4. Introduce the surface ABI/client and Ame compositor once image ownership across clients and composition is proven.

No external compositor port is part of these patch sets. GPU/frontend changes are authorized scope for future implementation of this plan; this document itself changes no graphics code or hardware state.
