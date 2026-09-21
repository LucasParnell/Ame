# Phase E/F — completion pass (dedup, de-hardcode, guards)

Phase E (Linux `virtio-gpu-pci` probe) and Phase F (standalone Linux KMS) were already
functionally landed and green. This pass finishes them against the project bar: data-oriented,
no duplicated code (shared homes in `libk` / shared headers), no unjustified hardcoded values,
every fixed bug ships a guard for its class, clean and tidy, arch-conformant.

Base: `9f6e8a6`. Commits: `5918b77` (guest), `35b54e7` (dango/ABI), `d5b4bab` (kernel),
`788a13f` (tests).

## What changed, by rule

| Rule | Before | After |
|---|---|---|
| No duplication | 3 aarch64 svc layers, ring geometry ×3, DRM UAPI ×2, ECAM accessors ×4, `align_up` ×2, channel/GPU constants in manager **and** DTS, proof regex + markers ×3, load addresses ×3 | one home each: `midix/abi/linux_guest_{syscall,console}.h`, `linux_drm_uapi.h`, `dango_channel_guest.h` ring view + binding, `pci::cfg_read/cfg_write` + `ecam_offset`, `libk/integer.h`, `ef_constants.py` (proof + markers), `dango-load.env` |
| No hardcoding | `0x48000000/0x20000/48/6`, `0x3f000000/0x100000/0x50000000`, BAR4 `0x4000`, host VAs `0x30000000`, opcodes `0x4743000{1..4}`, `512`, `0x10000000`, `39`, `4u`, initramfs/DTB loads | single macro/`constexpr` per value, aliased and `static_assert`-tied; the guest DTS is templated from the header |
| Guards | bug classes runtime-only or absent | `static_assert`s for ABI sizes, ring geometry, BAR4==MMIO base, ECAM/table capacity, SPI base, IPA width; runtime guards for index-before-bounds, null owner, whole-BAR IPA alignment, error propagation, config-space bound, group function count |
| Data-oriented | per-TU hand-rolled tables/functions | ABI as data (`midix/abi/*`), ring/irq/PCI geometry as named `constexpr` tables |
| Tidy | dead constants, unused helpers, stale comments | dead macros/helpers removed, comments corrected, one naming scheme (`kReg*`) |

## Bugs fixed (each with a guard)

- **KMS pattern shear** — the self-test filled rows by `width`, not `dumb.pitch`; now strides by
  `dumb.pitch / (bpp/8)` behind a `pitch >= width*bpp && pitch % bpp == 0` guard.
- **`/proc/interrupts` false negative** — a full 4 KiB read was treated as failure; only a
  negative read fails now.
- **ECAM `config_read` bound** — accepted offsets up to 4096 while the model is
  `kCfgSpaceBytes` (256); could read into the adjacent synthetic function's space. Now bounded.
- **`device_attach` null owner** — dereferenced `owner_vm` on the null-owner proof path; guarded.
- **`collect_vm_ram` error collapse** — every authorise failure became `E_OVERFLOW`; now the real
  `SysError` propagates.
- **`expose_function` guard order** — indexed `a->fns[i]` before the bound check; check moved
  first.
- **Whole-BAR IPA alignment** — only page alignment was enforced in the kernel; a whole-aperture
  publish now also requires IPA aligned to the BAR size (was userland-only).
- **`enumerate_groups`** — `function_count` could exceed its array; bounded by an assert.
- **KMS capture resolution** — the host row asserted only four pixels; it now also asserts the
  capture is the expected `1024×768` (a 400×300 image used to pass).
- **Stale screendump** — the HMP capture could be satisfied by a previous run's file; it is now
  unlinked first and required to be newer than the request.
- **Harness/build** — `verify.sh` no longer hides a GPU build-tree failure behind an empty
  `--build`; the CMake artifact hash is `EXISTS`-guarded (a partial artifact set no longer dies
  with a cryptic configure error); the manifest checks initramfs/DTB load-address overlap.

## Deliberate decisions

- The channel test round count is `512` (no `U` suffix) so stringifying it for the acceptance
  marker yields `512`, matching the numeric value the harness derives — noted in the header.
- Guest-side `dango_ring_hdr`/`dango_ring_slot` remain a C mirror of `MxRingSlot`/`MxRingPage`
  because `abi/ring.h` is C++-only; size and tail-offset `static_assert`s tie them.
- The guest in-kernel driver (`dango_channel.c`) was left untouched (changing it needs a Linux
  rebuild); its ring offsets are the same bytes the shared C view asserts.

## Evidence (macOS arm64, Homebrew Clang 23.1.1 / CMake 4.4.0 / QEMU 11.1.1)

- `scripts/verify.sh` — **all checks passed** (configure/build/host-regressions/provenance/
  boot-direct/lifecycle 1-2-4/supervision-restart/durable-persistence).
- `gpu-appliance` (Phase E) — success, 10 checks, `faults=0 quarantines=0 routes=1 injected=1 smmu=1`.
- `kms-display` (Phase F) — success, `kms_pattern=true`, capture `1024×768`, mid-row R/G/B/W.
- `dango-channel` (Phase G) — success, 512 ordered round trips, `h2g=512 g2h=514`.
- `assignment` — 43 checks pass; `iommu-dma` — 22 checks pass.
