# Ame — handoff (session 2026-09-20→21)

Pick-up notes for continuing the Ame work in a fresh chat.

## Where things are

- **Canonical repo:** `/Users/lucasparnell/Documents/Code/Ame`
- **Agent working copy:** `~/.openclaw-autoclaw/agents/meow/workspace/Ame` (kept in sync; the agent's
  file tools are workspace-scoped, so it edits here and fast-forwards the canonical repo)
- **Both are clean and identical at `master = 01e7191`.**
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
DMA, `iommu-testdev`), D (PCI assignment + IRQ virtualization), **E (Linux `virtio-gpu` appliance)**.

Phase E end-to-end now works: Ame boots with the SMMU enforced, the Linux appliance runs under Dango
with the assigned `virtio-gpu-pci`, Linux **binds the DRM driver** (`/dev/dri/card0` → `virtio_gpu`),
the INTx route is installed/injected, with **no IOMMU faults/quarantines**.

**Remaining:** F (standalone KMS self-test + `screendump` capture), G (guest-visible Dango channel),
H (`dango-gpud` control plane), I (shared staging framebuffer), J (framebuffer/TTY migration),
K–M (lifecycle, dynamic arenas, zero-copy), N–S (Vulkan). **Deferred by decision: P/Q/R (physical
backend, amdgpu, RADV)** — build the plumbing backend-agnostic, no AMD-specific code yet.

## The gate (run this first, always)

```sh
cd ~/Documents/Code/Ame            # or the agent working copy
scripts/verify.sh                  # configure/build/host-regressions/provenance/boot-direct/
                                   # lifecycle-matrix 1/2/4/supervision-restart/durable-persistence
AME_VERIFY_IOMMU=1   scripts/verify.sh    # + iommu-dma        (SMMUv3 hostile-DMA)
AME_VERIFY_ASSIGN=1  scripts/verify.sh    # + assignment       (PCI assignment + IRQ)
AME_VERIFY_GPU=1     scripts/verify.sh    # + gpu-appliance    (Linux virtio-gpu appliance)
```

All four currently pass. The GPU row needs the guest artifacts built (below). Rows are opt-in so the
default gate stays fast and unchanged. `scripts/verify.sh` is the single source of truth; keep it green.

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
- **Guest minimality:** `scripts/linux/check-minimal-config.py --flow gpu` enforces the deny-list;
  `scripts/linux/check-profile.py` checks the profile. Policy: `docs/LINUX_APPLIANCE_MINIMAL.md`.
  `=n` only wins if nothing `select`s the symbol (e.g. `CGROUPS` is selected by `SCHED_AUTOGROUP`;
  arm64 `SMP` is `def_bool y`).

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
python3 scripts/test/gpu-appliance.py --build out/qemu-gpu     # E.5 proof
# or: AME_QEMU_IOMMU=1 python3 scripts/test/boot.py --build out/qemu-gpu --mode direct --cpus 2 --timeout 200 --marker 'all services ready'
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
| Guest DTS / profile / build | `linux-artifacts/dts/dango-linux-gpu.dts.in`, `linux-artifacts/config/dango_gpu_arm64.fragment`, `scripts/linux/build-gpu-linux.sh` |
| Tests | `scripts/test/{iommu_dma,assignment,gpu-appliance}.py`, `tests/*` |

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
  sched context in `SCHED_BAND_LATENCY` (VM bands need per-VM class setup that isn't applied).
- **Guest kernel console is quiet** (`loglevel=4`); use the agent's messages / the `gpu proof:` line
  for evidence, not the Linux console.
- **macOS:** BSD `readlink`/`sed`/`cp` lack GNU options (`merge_config.sh` needs GNU tools — the build
  script shims them); `/usr/bin/make` is too old.

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

**Phase F** (`docs/GPU_APPLIANCE_TODO.md`): add a small static appliance userspace KMS test (raw DRM
ioctls, no libdrm) to the initramfs — open `/dev/dri/card0`, create+map a dumb buffer, fill a pattern,
create an FB and program a CRTC — and capture the QEMU display via monitor `screendump` to a PPM so the
pattern is asserted from the host. That proves device assignment + MMIO + IRQ + DMA + Linux DRM +
`virtio_gpu` independently of the Midix GPU protocol, with Ame containing no virtio-gpu register logic.
