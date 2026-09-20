# Cleanup implementation tracker

Working baseline preserved in `.git/cleanup-baseline-20260920-154756/` (source archive, tracked diff, status). Existing archives and local agent settings are retained.

Implemented and initially exercised:

- Root-anchored build ignores, tracked toolchain visibility, CMake presets and build-type flags.
- Shared freestanding memory runtime, strict alignment for early code, contiguous kernel heap allocation.
- Bounded DTB parser and stdout-path/alias console selection.
- Raw-image and U-Boot disk boots reach system ready under QEMU, two CPUs.
- Headless boot runner records separate serial logs and JSON results using disk snapshots.
- Shared signed MXB v2 verifier with pinned Monocypher 4.0.3, signed object catalog, explicit release trust configuration.
- Removed kernel embedded/tar fallbacks; rejected signatures do not trigger fallback.
- Host ASan/UBSan parser/signature and capability tests (9 passed).
- Init receives named immutable BootInfo; shared structural JSON parsing; shared integrity routines.
- MMIO requested rights checks and honest unimplemented IRQ return statuses.
- Shared ring layout aliases and numeric syscall/capability aliases.
- Switch package address validation now uses generated board metadata and ELF.
- Switch RelWithDebInfo kernel and userland build and Hekate package staging pass;
  physical-device testing has not been performed.
- Retained capability lookups, reference-counted IPC/arena objects, and mapping ownership.
- Scheduler-acknowledged remote termination and generation-tagged, retained child-exit records.
- Page-table pruning and creation-failure cleanup exercised by the kernel lifecycle fixture.
- Kernel lifecycle regression on 1, 2, and 4 CPUs: 100 process restart cycles with stable
  page/object counts, simultaneous exits, capability exhaustion, and shared arenas surviving
  their creator. These are kernel process tests, not full init service-restart coverage.
- Readable bundle and executable validation stages, a small Ed25519 interface, and a common
  C++ formatting configuration. The wider readability and module separation work is unfinished.
- Incremental RelWithDebInfo build reports no work; direct boot reaches readiness after the
  parser refactor. A fresh full boot matrix and clean Debug/Release validation remain pending.

Still required before completion:

1. Finish build/launcher/packaging/docs integration and provenance recording; validate clean/no-op Debug/Release/Switch builds.
2. Finish the object-lifetime audit (including VM final ownership and failed page-table allocation)
   and separate scheduler queue, accounting, handoff, and lifecycle modules.
3. Consume the new exit records in normal supervision; implement declarative manifests/channels/
   dependencies, common first/restart launch, readiness, and dependency restarts/backoff.
4. Per-client SPSC discovery/FS sessions, timeout/peer-death handling.
5. Explicit read-only disk root + volatile /tmp, session-owned generation handles; FS/shell modules; example application and tests.
6. Full CPU/boot matrix, 100 lifecycle cycles, guest Linux/auth regression and final feature documentation.

This file is a progress record, not a completion claim.
