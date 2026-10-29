# GPU, VM, EL2 and native app integration

Integrated on 29 October 2026 from the local checkpoint `0c3fbe9` and
`origin/feat/anne-el2-handoff` at `f630453`. The newer remote implementation
supplies the GPU, display, Linux appliance, EL2 and USB behavior. The local
checkpoint retains every original tracked change and nonignored untracked file.
An additional pre-integration snapshot is `0cc95af`.

The combined tree retains the native EGL/GLES/Vulkan frontend, T830 PanVK
patches, Mali shadow translation, shared render targets, native scanout and
firmware entry fixes alongside the app runtime, immutable executable snapshots,
scoped supervision, stdout, framed IPC, shared-buffer loans, filesystem changes
and power subsystem. Retired reference Vulkan broker/executor files follow the
remote deletions. The earlier `gpu-appliance` branch contains obsolete lavapipe
plumbing; its framebuffer fix already has an equivalent commit in master.

## Shared interfaces

- Remote platform operation numbers 14–19 remain unchanged. Local charger,
  CPU-frequency and eMMC operations move to 20–23. Power diagnostics move to
  request 7; VM progress retains request 6. Rebuild kernel and userland together;
  binaries built against the earlier local power numbers need rebuilding.
- Platform domains retain GPU at index 6 and append Charger at index 7.
- The service registry retains the local 64-entry capacity and allocates its
  actual size. This accommodates the combined service profiles.
- Syscall diagnostics retain the remote separate stack frame. The local
  producer-credit argument, bounded handoff authority, retained IRQ reference
  during revoke, and power diagnostics are carried into that implementation.
- Framebuffer handoff accounts for both pixel offset and actual row stride.
- Arena creation supports 256 pages (1 MiB). Unmap bookkeeping now covers all
  256 pages, releases the retained backing exactly once and permits reuse of
  the virtual window. Larger boot-issued DMA mappings retain their existing
  lifetime behavior.
- Guest CNTP emulation and EL2 cache maintenance remain the remote paths.
  An EL1-only manager publishes native service readiness before parking, so
  an unavailable VM cannot prevent native apps and the desktop from starting.
  CPU-only commands/cache memory stays separate from explicitly authorized,
  pinned SharedGpu targets. Display retirement and quarantine retain ownership
  of any buffer hardware may still read.

## App and graphics use

The IPC benchmark remains the reference for `mochi::app::start()`,
`app::Process`, suspended wiring and `wait_all()`. The loan benchmark remains a
separate workload because returning a shared buffer measures a different
contract from a copied echo. Apps remain outside the normal boot bundle.

Graphics clients can link `libEGL` or `libVulkan` and pass
`mochi::app::start().boot` to `ame::graphics::connect()`. Provision the graphics
ring/doorbell endpoint and command arena before resuming the app, using explicit
capability grants. The current Anne diagnostic profile provisions the shell;
sharing that single connection between concurrent app clients is not supported.
The app runtime does not introduce ambient GPU authority. See `APP_API.md` and
the native API sections of `graphics.md`.

## Arena resizing search

No dynamic resizing implementation was found in the original working tree,
its 65 untracked files, ignored files under the source directories, fetched
branches, Codex tree snapshots, the adjacent `Ame-trim` checkout, or 19
recoverable commits and 1,398 unreachable blobs. The earlier local architecture
document describes growable arenas as a design; the actual allocator is bounded
and the kernel header explicitly marks growth unimplemented. This integration
fixes large-arena cleanup but does not claim to recover or implement resizing.

Any future resize operation must preserve existing CPU mappings and app
pointers, enforce quota authority, and coordinate sharing. GPU/display backing
cannot move or be freed while stage-2 mappings, GPU work or scanout pin it.
Shared loan pools also require stable geometry while descriptors are in flight.

## Validation

The integration was checked with native Anne GPU-profile and QEMU release
builds, repeated full-system benchmark launches and terminal restart/input
checks at one and four CPUs, the kernel lifecycle fixture, and host regressions
for memory, IPC, power, graphics, USB, stage 2 and EL2 timer/SIMD isolation.
Detailed local build/test captures are under `out/integration-*`.

The terminal fixture now copies complete serial lines to the debug UART so
unrelated kernel diagnostics cannot appear in the middle of benchmark JSON.
The production stalled-USB fallback remains in place. Host fixtures also
isolate privileged cache operations and Linux-only CTS process spawning.

No handset was flashed during integration. GPU hardware behavior and packaged
Linux/Mesa artifacts still need validation on the target device after rebuilding
the matching artifact set.
