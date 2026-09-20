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
- Declarative boot manifest extended to channels, `start_after` dependencies (with cycle
  detection), readiness doorbells, and dependency-aware restart with backoff. Exercised by the
  normal boot reaching `init: all services ready`.
- Single verification gate `scripts/verify.sh`: configure → build → host regressions → direct
  boot → kernel lifecycle matrix, all green on the toolchain recorded below.
- Per-build provenance record emitted by the ordinary build: `generated/provenance.json`
  (git revision + dirty flag, build type, board, dev/release mode, signing key fingerprint,
  `system.mxb` sha256) with a `generated/provenance.h` companion. Best-effort when git is
  absent. See README for the field list.

### Session 2026-09-20 (development-readiness pass)

- **Fixed build breakage under LLVM 23.** `userland/fs/main.cc` failed with four `-Werror`
  unused-entity diagnostics (`unused-const-variable` ×2, `unused-function`,
  `unused-but-set-global`). The dead entities are marked `[[maybe_unused]]`; no behaviour change.
- **Fixed BootInfo slot-0 ordering bug in init.** `start_service_now()` called
  `refresh_exports()` (which grants peer process caps into the nameservice child) *before*
  `boot_init_for_service()` reserved the child's slot-0 BootInfo arena, so nameservice's BootInfo
  grant landed in a non-zero slot and init halted with `init: bootinfo slot failed`. BootInfo now
  claims slot 0 first, and `refresh_exports()` refuses to grant into a server before its BootInfo
  exists. Boot now completes to `init: all services ready`.
- Added `scripts/verify.sh`; recorded the validated toolchain (Homebrew Clang 23.1.1, CMake 4.4.0,
  QEMU 11.1.1).
- Added `scripts/provenance.py` and wired it into the top-level `CMakeLists.txt` so every build
  lands `generated/provenance.json`/`.h`. Confirmed the qemu-dev (Debug) tree reports
  `ninja: no work to do` on a second build with no source changes, and that qemu-release (Release)
  and switch (RelWithDebInfo) configure and build and emit correct provenance (release mode
  records a release key fingerprint distinct from the dev seed).

Still required before completion:

1. Finish launcher integration (the build/launcher/packaging/docs integration is largely in place:
   `ame_vfs`/`ame_disk` packaging targets, `scripts/verify.sh`, and the provenance record are
   documented in README; clean/no-op Debug/Release/Switch builds are validated). Remaining:
   unify the QEMU launch paths behind the documented entry points and drop any stale launcher docs.
2. Finish the object-lifetime audit (including VM final ownership and failed page-table allocation)
   and separate scheduler queue, accounting, handoff, and lifecycle modules.
3. Consume the new exit records in normal supervision (steady-state path) and verify dependency
   restarts/backoff under injected faults; add a regression fixture for the restart path.
4. Per-client SPSC discovery/FS sessions, timeout/peer-death handling.
5. Explicit read-only disk root + volatile /tmp, session-owned generation handles; FS/shell modules; example application and tests.
6. Full CPU/boot matrix, 100 lifecycle cycles, guest Linux/auth regression and final feature
   documentation. The per-subsystem status matrix — status, evidence, limitations and the
   test-matrix gaps (including what is explicitly unverified) — is now in
   [docs/FEATURE_STATUS.md](FEATURE_STATUS.md). Guest Linux/auth regression and disk-mode
   boot remain out of the default gate; see that document's gaps section.

Notes:

- The `init` service already implements channels/dependencies/readiness/dependency restarts; earlier
  revisions of this file listed them as pending. Tracker entries are now aligned with the code.
- `.git/cleanup-baseline-20260920-154756/` is referenced above but was not present in the
  2026-09-20 source archive; re-create it before further large edits.

This file is a progress record, not a completion claim.
