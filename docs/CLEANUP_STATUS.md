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
- Release signing sorted: `scripts/make-release-key.sh` generates an Ed25519 PKCS8 keypair outside
  the repo; `qemu-release` configure + build + direct boot are validated with a generated key, and
  a release configure without `AME_SIGNING_KEY` still fails closed.
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
4. Per-client SPSC discovery/FS sessions, timeout/peer-death handling. (Done — see session
   note below; a fault-injection fixture for the stall path is still worth adding.)
5. Explicit read-only disk root + volatile /tmp, session-owned generation handles **done**;
   FS modules **done**; shell (msh) modules **done**; example application (app.hello) **done**.
   Durable user volume (`/data` + `FS_SYNC` write-back + `BLK_FLUSH`, with a two-boot
   persistence regression) **done**; still pending: automated tests for the example
   application, and the journaled on-disk B+tree writer (the reserved region is used as
   a separate durable store, not as the root's B+tree journal).
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

### Session 2026-09-20 (sessions + io.fs modules)

- **Per-client SPSC sessions (item 4).** io.fs and nameservice serve one SPSC
  request/reply ring pair and one doorbell pair per client; the service holds a
  `ServerSessions` directory and never a single global session. Each slot carries
  its own generation; the server maps/resets the slot rings only on the
  generation handshake and acknowledges before the client resumes.
- **Timeout + peer-death handling.** Added `SpscEndpoint::send_bounded` and a
  per-instance reply deadline (`mochi::kSessionReplyTimeoutNs`) used by
  `fs::Server`. A client that dies mid-request (its process cap is revoked, so
  `sys_cap_identify` fails) or stops draining its reply ring is dropped: the
  bounded send expires, the session is closed, and the other clients keep
  running. Only init can revive a dropped slot, by publishing a newer
  generation. The block-driver round trip (`fs::BlkClient::request`) is bounded
  the same way, so a dead drv.blk degrades io.fs to the memory backend instead
  of wedging it.
- **Session-owned generation handles (item 5).** `handle_state` now accepts a
  token only when its handle generation *and* the owning session generation
  match the client currently being served, so a stale handle can never be
  replayed across a client restart. Session teardown still releases every handle
  the exiting slot owned.
- **io.fs module split.** `userland/fs/main.cc` is split behind
  `fs/fs_internal.h` into `fs_ring` (boot wiring, per-client session loop,
  dispatch), `fs_core` (in-memory inode/extent/phys-ref core), `fs_handles`
  (handle table + handlers), `fs_snapshot`, and `fs_disk` (GPT/B+tree/block
  cache). Behaviour-preserving moves; `-Wall -Wextra -Werror` clean.
- **Mount semantics documented.** The disk root is read-only (disk-backed
  handles are always read-only and write/create/truncate flags are rejected
  before the B+tree) and `/tmp` is volatile (in-memory only, redirected away
  from the disk root, discarded on restart). See the header comment in
  `fs/fs_internal.h`.
- Verified: `scripts/verify.sh` green (boot ready, lifecycle 1/2/4, supervision
  restart, provenance); `cmake --preset switch && cmake --build --preset switch
  --target anko` green.

### Session 2026-09-20 (durable storage)

- **`sync` is now real, and the contract is honest.** `userland/fs/fs_durable.cc`
  serialises the `/data` subtree into the disk's reserved journal region with
  `BLK_WRITE` and commits it with one `BLK_FLUSH`. A file written and synced to
  `/data` survives a reboot. The read-only root is still never mutated, and `/tmp`
  is still volatile.
- **Crash-safe commit.** The region header is written `Dirty` → records → header
  `Committed` → flush; a torn sync leaves a `Dirty` header that the loader rejects.
  Records carry a record CRC and a folded data CRC, all validated before replay.
- **Non-durable mode is explicit.** With no usable reserved region, `/data` is an
  in-memory volume and `FS_SYNC` returns `MX_ERR_NOT_IMPLEMENTED`; the boot log
  says `non-durable /data` rather than implying persistence.
- **Block write path + reply hygiene.** `fs_disk` gained cache-coherent
  `disk_write_block`/`disk_flush`; `FS_CLOSE_REPLY`/`FS_SNAPSHOT_DROP_REPLY` are
  dedicated reply types so a close acknowledgement can never be read as a sync
  result. `init`'s manifest capacity rose from 16 to 20 services.
- **Persistence regression.** `scripts/test/durable-persistence.py` boots
  `durable.mxb` (the normal system + an `app.durable` probe) twice against one
  disposable copy of a writable midix-fs-v1 image, writes `/data/persist.txt`,
  syncs, reboots, and asserts the exact bytes. It is a `scripts/verify.sh` check
  (`durable-persistence`).
- Still open: the **journaled on-disk B+tree writer**. The durable region is a
  separate store, not the root's journal; mutating the read-only root through the
  B+tree remains unimplemented (and deliberately so).

### Session 2026-09-20 (GPU appliance E.1)

- Synthetic ECAM uses fixed records and bounded operations with no allocation in the fault path.
  Guest PCI enumeration supports sized reads, fixed BAR probing/restoration and mediated command
  writes; malformed accesses, config relocation and indirect PCI config access fail closed.
- Trap ranges prevent RAM/MMIO aliasing. Raw MSI-X table/PBA pages are denied. Host command stores
  use 16 bits to preserve adjacent write-one-to-clear status. Device revoke removes BAR leaves even
  after a VM has first run, under the existing VM call guard.
- Real AArch64 guest fault/resume proof added to the assignment gate (43 checks), alongside host
  sanitizer tests (16 host tests total). The final full gate with IOMMU and assignment rows passes;
  log: `out/validation/ecam-verify.log`.
- Fixed stale-log acceptance when reusing the boot harness output directory, with a regression.
- GPU DTB generation now uses the portable host file-size helper on macOS. Its ECAM is one bus
  (1 MiB); the MMIO window is outside the guest RAM and channel region.
- Linux GPU assignment/probe is still pending; see `GPU_APPLIANCE_TODO.md` and
  `DELIVERY/gpu-appliance-phase-e1.md`.

This file is a progress record, not a completion claim.
## Phase G — guest-visible channel

The generic Dango transport now maps two non-DMA rings into Linux, uses virtual
IRQ/HVC notifications, and exposes a bounded, root-only Linux character device.
The GPU Linux build adds its built-in driver through a read-only source overlay.
`AME_VERIFY_CHANNEL=1 scripts/verify.sh` checks 512 ordered round trips and clean
post-exchange IOMMU counters; sanitizer and lifecycle tests cover ARM races,
capability/VM isolation and mapped-channel reclamation. See
[Phase G delivery](../DELIVERY/gpu-appliance-phase-g.md).

### GPU executor cleanup (2026-09-21)

- Added a macOS Docker / arm64 Linux libc-rootfs build with an explicit ELF
  dependency closure and provenance; both plain and deterministic gzip initramfs
  packaging use the existing archive writer/checker.
- Replaced the unexecuted Vulkan per-range buffer cache with a preallocated arena
  buffer. Command snapshots prevent shared-record validation/execution races;
  shared decoding rejects truncated records, reserved fields and invalid opcodes.
  Overflow-safe operand bounds, command quotas, transfer dependencies and checked
  Vulkan return values cover the bugs found while enabling the real executor.
- Partial initialization unwinds every created object; invalid submits leave
  arena bytes unchanged. WSI now refuses unsupported work instead of claiming
  that bytes were copied. Real lavapipe transfer execution passes in the build
  container/chroot and a standalone Linux boot.
- Native N/O completion is still open: appliance RAM/load layout, session arena
  bridge and real graphics/scanout acceptance. See `GPU_APPLIANCE_REMAINING.md`.

### Appliance bring-up fixes (2026-09-22)

- **Entry-thread stack.** The initial user thread was mapped a single 4 KiB page;
  `msh` had already worked around overflowing it and the appliance manager then
  overflowed it too (EL0 fault, sp just past `USER_STACK_TOP - PAGE_SIZE`).
  `kernel/init/boot_caps.cc` now maps 16 KiB for the entry thread and starts later
  per-thread stacks below it.
- **Midix userland C++ runtime.** Mochi services link `-nostdlib`, so function-local
  statics with a non-constant initialiser had no `__cxa_guard_*`.
  `userland/lib/runtime/cxx_guard.cc` implements the Itanium guards over an atomic
  guard word, linked into every service as `mochi_runtime`.
- **Shared-arena window mapped.** The GPU DTS declares `dango-shm@4a000000` and the
  guest `dango_shm` driver reads it, but nothing mapped that IPA into stage 2, so
  the guest took a stage-2 translation fault while probing and the VM runner
  stopped before the agent ran. `map_channel` now maps the window read-only as one
  zeroed page repeated across it, owned by the VM.
- **C-compatibility of the ABI headers.** `abi/types.h`, `abi/status.h` and
  `protocols/gpu.h` compile as C and C++ (the guest `dango-gpud` build); a C++
  digit separator inside an `#if __cplusplus` block broke the GCC build and was
  replaced with plain hex.
- **Dead MSI route.** `add_or_update_msi_route` refuses by name (`E_NOTIMPL`,
  phase P–Q) when no host MSI line was reserved, instead of creating a route that
  can never be delivered.

### GPU continuation from `0da9833` (2026-09-22)

- Fixed block-split failure atomicity and exact-capacity accounting; whole-range
  RAM/MMIO rollback and revocation no longer allocate. Shared-arena and assignment
  headroom use a shared record-capacity check, with no unsigned underflow.
- Propagated block-map errors rather than retrying them as page maps. The host
  regressions cover 512 MiB capacity, failed splits, unchanged overlap rejection,
  and allocation-free active Device revocation at full record capacity.
- Repaired the native-frame guest service handoff after KMS/channel tests, and
  added shared Linux DRM `DIRTYFB` UAPI plus a checked flush for each present.
- Wired the TTY gate and changed its proof from the native framebuffer/staging
  marker to completed presentation and the assigned GPU's captured shell text.
- Fixed stale provenance after branch advances and made the gate check it against
  the current checkout. The larger libc appliance layout/native Vulkan bridge
  remains unfinished; see `GPU_APPLIANCE_GUEST_MEMORY.md`.
