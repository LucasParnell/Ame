# Ame — handoff (session 2026-09-21)

Pick-up notes for continuing the Ame work in a fresh chat.

## Where things are

- **Canonical repo:** `/Users/lucasparnell/Documents/Code/Ame`
- **Agent working copy:** `~/.openclaw-autoclaw/agents/meow/workspace/Ame` (kept in sync; the agent's
  file tools are workspace-scoped, so it edits here and fast-forwards the canonical repo)
- Reference docs in-tree: `docs/GPU_APPLIANCE_TODO.md` (per-phase plan + status — **start here**),
  `docs/LINUX_APPLIANCE_MINIMAL.md` (guest minimal-config policy), `docs/FEATURE_STATUS.md`,
  `docs/CLEANUP_STATUS.md`, `DELIVERY/*` (per-phase delivery notes), `Architecture_3.md`,
  `docs/FS_SPEC.md`.

## What Ame is

A from-scratch AArch64 OS: **Anko** microkernel (capabilities, shared-memory IPC rings, doorbells,
arenas, preemptive scheduler with bands/budget, GICv2, stage-2 VM support) + **Mochi** userland
(init/supervision, nameservice, io.console, io.framebuffer/tty.fb0, io.fs, msh, drivers) + **Dango**
(Linux VM appliance host, EL2). Renamed from Midix; `midix`/`mx` names remain internally.
Reference target: QEMU `virt`; board `switch-t210` builds only (no device).

## Current state (phases)

**Done and verified:** A (PCI/IOMMU inventory), B (generic IOMMU core), C (SMMUv3 backend + hostile
DMA, `iommu-testdev`), D (PCI assignment + IRQ virtualization), **E (Linux `virtio-gpu` appliance)**,
**F (standalone Linux KMS self-test + host `screendump`)**.

Phase E end-to-end works: Ame boots with the SMMU enforced, Dango constructs the assignment
(function + INTx authority, BAR allocation from the PCIe window, synthetic ECAM, stage-2 **Device**
BAR mapping), the Linux appliance runs under Dango with the assigned `virtio-gpu-pci`, Linux **binds
the DRM driver** (`/dev/dri/card0` → `virtio_gpu`), the INTx route is installed/injected, with **no
IOMMU faults/quarantines**.

Phase F works: the guest appliance userspace (`linux-artifacts/kms_test/kms_test.c`, raw DRM ioctls,
no libdrm, linked into the init binary) opens `/dev/dri/card0`, allocates a dumb buffer, fills a
four-colour bar pattern, creates an FB and programs the CRTC (legacy `SETCRTC`); the host captures the
assigned console with a QEMU monitor `screendump` (`scripts/test/kms-display.py`) and asserts the
pattern. Proves assignment + MMIO + IRQ + DMA + Linux DRM + `virtio_gpu` with no virtio-gpu register
logic in Ame.

**Panic screen:** the kernel paints a graphical panic screen — a QR code carrying the panic report
(top-left, with the key data under it), the project logo (smaller, in the remaining space) and the
`KERNEL PANIC` title/reason left-aligned along the bottom. The QR is produced by an in-kernel
byte-mode Model-2 encoder (`kernel/qr.cc`) whose spec tables come from
`kernel/assets/qr_tables.json`; the report record, the full serial dump and the halt semantics live
in `kernel/panic_report.cc` / `kernel/panic.cc`. Captured and decoded end-to-end by
`AME_VERIFY_PANIC=1`. See `DELIVERY/panic-qr-report.md`.

**Phase G is complete:** non-DMA ring mappings, vendor HVC NOTIFY/ARM, virtual IRQ 48,
`/dev/dango-channel0`, and 512 ordered Linux ↔ Mochi round trips. See
`DELIVERY/gpu-appliance-phase-g.md`. Rebuild the GPU kernel for its built-in channel driver.

**GPU continuation:** H–O code has since landed; use
`docs/GPU_APPLIANCE_REMAINING.md` for the current unfinished work. The libc/lavapipe
rootfs now builds with `scripts/linux/build-vkd.sh` (Docker on macOS), and
`AME_VERIFY_VULKAN=1 scripts/verify.sh` runs the real transfer executor under
standalone Linux. Native N/O still needs the session bridge, appliance memory/load
layout changes and graphics/WSI acceptance. This rootfs is in `build/dango-vkd/`,
separate from the existing `build/dango-linux-gpu/` artifacts.
**Deferred by decision: P/Q/R (physical backend, amdgpu, RADV)** — build the plumbing
backend-agnostic, no AMD-specific code yet.

## The gate (run this first, always)

```sh
cd ~/Documents/Code/Ame            # or the agent working copy
scripts/verify.sh                  # configure/build/host-regressions/provenance/boot-direct/
                                   # lifecycle-matrix 1/2/4/supervision-restart/durable-persistence
AME_VERIFY_IOMMU=1   scripts/verify.sh    # + iommu-dma        (SMMUv3 hostile-DMA)
AME_VERIFY_ASSIGN=1  scripts/verify.sh    # + assignment       (PCI assignment + IRQ)
AME_VERIFY_GPU=1     scripts/verify.sh    # + gpu-appliance    (Linux virtio-gpu appliance)
AME_VERIFY_CHANNEL=1 scripts/verify.sh    # + dango-channel (ordered ring traffic)
AME_VERIFY_KMS=1     scripts/verify.sh    # + kms-display      (KMS modeset + screendump)
AME_VERIFY_PANIC=1   scripts/verify.sh    # + panic-screen     (logo/headline capture)
```

All rows currently pass. The GPU/KMS rows need the guest artifacts built (below). Rows are opt-in so
the default gate stays fast and unchanged. `scripts/verify.sh` is the single source of truth; keep it
green.

## Environment (macOS)

- **Toolchain:** Homebrew LLVM 23 (`/opt/homebrew/opt/llvm/bin`), CMake 4.4, QEMU 11.1.1, brew
  `make` 4.4 (Apple's `/usr/bin/make` 3.81 will NOT build Linux), brew `bison`/`flex`/`dtc`,
  GNU coreutils (`greadlink`, `gsed`, `gmake`, …) — needed because macOS ships BSD userland.
- **Linux guest can't build natively on macOS** (kernel host tools need `<elf.h>`). Use the container:
  - `colima start --cpu 6 --memory 8 --disk 40` (Docker via Colima; no Docker Desktop). Re-run after reboot.
  - `scripts/linux/build-gpu-linux-docker.sh` builds the full GPU artifact set in `ubuntu:24.04`
    (aarch64) into **`build/dango-linux-gpu/`** (`Image`, `dango-agent`, `initramfs.cpio`,
    `dango-linux.dtb`, `manifest.toml`). Kernel dir `build/dango-linux-gpu-kout` persists → rebuilds are
    incremental; delete it if a killed build corrupts `.cmd` files.
- **Iterating on the guest userspace needs no container.** The agent cross-compiles natively on macOS
  (clang + lld), so after editing `linux-artifacts/agent/` or `linux-artifacts/kms_test/` you can rebuild
  just the agent/initramfs/DTB/manifest and repackage the MXB:
  ```sh
  export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
  export DANGO_LINUX_OUT="$PWD/build/dango-linux-gpu" DANGO_LINUX_PROFILE=gpu
  scripts/linux/build-agent.sh && scripts/linux/build-initramfs.sh
  DANGO_DTS_TEMPLATE="$PWD/linux-artifacts/dts/dango-linux-gpu.dts.in" \
    DANGO_DTB_NAME=dango-linux.dtb scripts/linux/build-dtb.sh
  scripts/linux/build-manifest.sh        # sizes must match the artifacts (check-profile enforces)
  cmake --build out/qemu-gpu --target mochi_system
  ```
- **Guest minimality:** `scripts/linux/check-minimal-config.py --flow gpu` enforces the deny-list;
  `scripts/linux/check-profile.py` checks the profile + manifest sizes. Policy:
  `docs/LINUX_APPLIANCE_MINIMAL.md`. `=n` only wins if nothing `select`s the symbol (e.g. `CGROUPS` is
  selected by `SCHED_AUTOGROUP`; arm64 `SMP` is `def_bool y`). The Phase F KMS test needs **no new
  kernel config** (it uses the DRM/KMS path the GPU fragment already enables; `DRM_FBDEV_EMULATION=n`).

## Build / run commands

```sh
# default (no Linux payload)
cmake --preset qemu-dev && cmake --build --preset qemu-dev --target anko mochi_system
python3 scripts/test/boot.py --build out/qemu-dev --mode direct --cpus 2 --marker 'all services ready'

# the GPU appliance
rm -rf out/qemu-gpu
cmake -S . -B out/qemu-gpu -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake \
      -DCMAKE_BUILD_TYPE=Debug -DANKO_BOARD=qemu-virt -DANKO_BUILD_DEV=ON \
      -DDANGO_BUILD_LINUX_ARTIFACTS=OFF -DDANGO_LINUX_PROFILE=gpu \
      -DDANGO_LINUX_ARTIFACT_DIR="$PWD/build/dango-linux-gpu"
cmake --build out/qemu-gpu --target anko mochi_system
python3 scripts/test/gpu-appliance.py --build out/qemu-gpu     # Phase E proof
python3 scripts/test/kms-display.py  --build out/qemu-gpu     # Phase F proof (screendump)
```

## Key files for the GPU path

| Concern | File |
|---|---|
| Secure-attach / BAR assignment / stage-2 headroom guard | `kernel/vm/vm_device.cc` |
| IRQ routing to the vGIC | `kernel/vm/vm_device_irq.cc` |
| SMMUv3 backend, fault→quarantine | `kernel/iommu/{smmuv3,faults,core}.cc` |
| Synthetic ECAM (stage-2 trap), config mediation | `kernel/pci/ecam.cc`, `kernel/pci/assignment.cc`, `kernel/pci/msi.cc` |
| BAR/window inventory | `kernel/pci/enumerate.cc`, `anko/pci.h` |
| Boot authority (`gpu.function`/`gpu.irq`, INTx id) | `kernel/init/boot_caps.cc` |
| `gpu proof:` counters | `kernel/vm/vcpu.cc` |
| Appliance sequence + budgeted runner | `userland/dango/manager/main.cc` |
| Guest DRM probe / assertions | `linux-artifacts/agent/dango-agent.c` |
| Guest KMS self-test (raw DRM ioctls) | `linux-artifacts/kms_test/kms_test.c` |
| Guest DTS / profile / build | `linux-artifacts/dts/dango-linux-gpu.dts.in`, `linux-artifacts/config/dango_gpu_arm64.fragment`, `scripts/linux/build-gpu-linux.sh`, `scripts/linux/build-manifest.sh` |
| Tests | `scripts/test/{iommu_dma,assignment,gpu-appliance,kms-display}.py`, `tests/*` |
| Log de-interleaver (guest vs Ame console) | `scripts/test/logfmt.py` |
| Panic screen renderer | `kernel/panic_screen.cc`, `kernel/include/anko/panic_screen.h` |
| Panic report record + renderers | `kernel/panic_report.{h,cc}` |
| QR encoder | `kernel/qr.{h,cc}` |
| QR spec data (asset + generator) | `kernel/assets/qr_tables.json`, `scripts/gen-qr-tables.py` |
| Panic screen logo (data asset) | `kernel/assets/panic_art.txt` + `scripts/mk-ascii-header.py` |
| Shared 8x16 console font | `shared/include/midix/font/vga8x16.h` |
| Panic/QR tests | `tests/test_qr.py`, `tests/qr_decode.py`, `tests/test_panic_screen.py`, `tests/panic_screen_probe.cc`, `scripts/test/panic-screen.py` |

## Pitfalls already hit (don't rediscover these)

- **BAR aliasing.** The BAR allocator must skip regions the inventory already reports as assigned;
  starting at the window base aliased the native xHCI (`0x10000000`) and broke its reset.
- **GIC interrupt specifiers** are `<type number flags>` and the SPI INTID is **32 + number**
  (QEMU virt PCI INTx pin A slot 0 → number 3). Using the raw number is rejected by `irq_bindable`.
- **Stage-2 table sizing.** Guest RAM is mapped page-granular; 128 MiB alone is 32768 entries
  (`kMaxStage2Mappings` is 40960). `device_map_bar` returns `E_NOTFOUND` for `bar.base == 0` and
  `E_BUSY`/`E_OVERFLOW` for the table being full — check the headroom guard in `vm_device.cc`.
- **MSI-X BAR must not be mapped.** `bar_range_safe` rejects mapping a BAR containing the MSI-X
  table — map the virtio modern BAR (BAR4), not BAR1.
- **BAR guest IPA must be aligned to the BAR size** (asserted at compile time in the manager).
- **Runner starvation.** The manager's polling loop must not stay runnable ahead of the budgeted vCPU;
  status reporting now lives in the runner and the manager sleeps. The runner uses a budget/period
  sched context in `SCHED_BAND_VM_FAIR`, pinned to CPU1 when available. A single-host-CPU
  boot can still starve behind native polling services.
- **Guest kernel console is quiet** (`loglevel=4`); use the agent's messages / the `gpu proof:` line
  for evidence, not the Linux console.
- **Guest and Ame share the host console serial**, and the guest driver writes byte-by-byte, so Ame log
  entries land *inside* guest lines and split markers (`dango: DRM card[ … ] [I] …\n0 driver=…`).
  De-interleave before matching (`scripts/test/logfmt.py`, used by `boot.py` + both GPU tests). This was
  the root cause of an intermittent `gpu-appliance` gate failure.
- **Second `GETCONNECTOR` call must clear `count_props` / `count_encoders`.** The kernel copies into
  every pointer whose matching count is non-zero; leaving the first call's counts set with NULL pointers
  returns `-EFAULT` and the mode fetch silently yields no modes.
- **A headless QEMU console needs `edid=on,xres=,yres=`** for `virtio-gpu` to expose modes; and pass
  `id=` so `screendump` targets the assigned device rather than `ramfb`.
- **macOS:** BSD `readlink`/`sed`/`cp` lack GNU options (`merge_config.sh` needs GNU tools — the build
  script shims them); `/usr/bin/make` is too old.
- **Host C++ probes must not add `-I lib/include`.** That freestanding `cstdint` shadows the system
  one and breaks libc++ headers; compile probes with `-I kernel/include -I shared/include` only.
- **To capture the panic screen** build with `-DANKO_PANIC_SCREEN_DEMO=ON` (dev only): the kernel
  panics right after the framebuffer is bound, before any userland can repaint, and
  `scripts/test/panic-screen.py` captures it via the QEMU monitor.
- **Panic text must bypass the log ring.** `log::write()` at Info level goes to the per-CPU ring, which
  nothing drains during early boot, so a panic dump silently vanishes. Use `log::panic_dump()`.
- **Don't verify a QR encoder by matrix equality.** Reference encoders differ in legal padding (segno
  emits an extra zero byte when the message is already byte-aligned); `tests/qr_decode.py` decodes the
  symbol instead, which is both stricter and encoder-agnostic.
- **The staged Linux artifacts are stamped by content hash.** Ninja judges freshness by mtime, so an
  artifact dir refreshed with older timestamps (`rsync -a`) used to be skipped and `system.mxb` kept a
  stale payload. `userland/CMakeLists.txt` now depends on a content-hash stamp; if you copy artifacts
  around, keep that stamp logic intact.

## Conventions / quality bar

- Capability-first kernel; ABI in `shared/include/midix/abi/` with `static_assert` drift checks.
- 64-byte ring slots, ≤48-byte inline payloads, bulk data out-of-band.
- Fixed-capacity, data-oriented structures; no hot-path allocation; mask untrusted indices at entry.
- **Any bug found gets a guard/assert where it costs nothing at runtime** (that is how the xhci BAR
  aliasing was caught — `init` now names the service that misses its readiness deadline).
- No untidy/buggy/insecure/placeholder code; any `E_NOTIMPL` must name its phase and ship a failing
  test. `docs/LINUX_APPLIANCE_MINIMAL.md`, `docs/GPU_APPLIANCE_TODO.md` are the spec-adjacent policies.
- Commit in coherent steps; keep `scripts/verify.sh` green.

## Immediate next step

**Phase H** (`docs/GPU_APPLIANCE_TODO.md`): implement `dango-gpud` control-plane
requests (`GPU_HELLO`, `GPU_ENUM_OUTPUTS`, `GPU_GET_MODES`, `GPU_ERROR`) over the
Phase G channel, with a Mochi probe enumerating the virtio-gpu output. Replace
the startup transport test with the service protocol as that phase lands.

Phase G's Linux driver is built in through a shallow source overlay; the original
Linux sources remain read-only. Native userspace iteration also rebuilds
`linux-artifacts/channel_test/channel_test.c`. The aperture is outside appliance
RAM at `0x48000000` and never enters the GPU DMA domain. See the Phase G delivery
for the exact ABI and the existing single-host-CPU VM-runner starvation limit.
