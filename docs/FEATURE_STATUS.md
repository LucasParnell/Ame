# Ame feature status

An honest, per-subsystem status matrix for the current tree. Every row cites the
code that implements the feature and the boot/test evidence that exercises it.
Nothing here is aspirational: where a claim could not be confirmed against the
code or a captured log it is marked **unverified**.

- **Tree**: branch `master`.
- **Validated toolchain** (from `scripts/verify.sh`): Homebrew Clang 23.1.1,
  CMake 4.4.0, QEMU 11.1.1.
- **Health gate**: `scripts/verify.sh` — configure → build → host regressions →
  provenance → direct boot → kernel lifecycle matrix → supervision restart.
  All checks green (see [Verification](#verification)).  Opt-in rows extend it for the
  appliance work: `AME_VERIFY_IOMMU`, `AME_VERIFY_ASSIGN`, `AME_VERIFY_GPU`, `AME_VERIFY_KMS`, `AME_VERIFY_CHANNEL`.
- **Boot evidence** below comes from the gate's QEMU runs (`scripts/test/boot.py`).
  The kernel and userland log over the *first* QEMU serial port; that is the
  `host.log` in the runner's output directory. The second serial port
  (`guest.log`) is unused by the native boot and is empty.

## Status legend

| Status | Meaning |
|--------|---------|
| **Implemented** | Code exists and is exercised by the default gate or a captured boot log. |
| **Partial** | Core path works, but a stated capability is missing or only exercised on a reduced path. |
| **Not implemented** | Stub or explicit `E_NOTIMPL`; no working path. |
| **Unverified** | Code may exist but no captured evidence in this tree exercises it. |

## Documentation drift warning

Some design documents predate the current code and describe the implementation as
less complete than it is. Do **not** treat these as status:

- `docs/scheduler.md` describes the V0 strict-priority scheduler with the fair
  queue and handoff features listed as *disabled*. `kernel/sched/` now implements
  the Ressha/Eki V2 fair scheduler and the handoff path. Trust the code.
- `docs/architecture.md` and `docs/boot-sequence.md` describe a `boot/` shim
  (`boot/entry/boot_main.cc`, `boot/dtb/fdt.cc`, `boot/mmu/*`) as the boot path.
  That directory is **not built** (`add_subdirectory(boot)` is commented out in the
  top-level `CMakeLists.txt`) and its sources are unimplemented placeholders. The
  live boot path is `kernel/arch/arm64/entry.S` → `kernel_main()`.
- `docs/v0-progress.md` acceptance markers (`hello world`, `IPC PING/PONG OK`) are
  historical; the current default boot ends at `init: all services ready`.

---

## 1. Anko kernel

### 1.1 Capabilities

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Capability allocation, lookup, dup, revoke, revoke-by-target | Implemented | `kernel/cap/cap_table.cc`: `cap::alloc`, `lookup`, `lookup_any`, `dup`, `revoke`, `revoke_target`, `reserve_slots`; entry generations (`next_generation`) | Slot table is per-process; `cap_table.cc:84` notes there is no global V0 registry |
| Reference-counted object lifetime + destructors | Implemented | `kernel/cap/object.cc`: `object::create/acquire/retain/release`, `live_count()` | — |
| Boot capability issuance for init | Implemented | `kernel/init/boot_caps.cc`: `issue_boot_caps` grants `manifest`, `boot.initrd`, `self`, `supervisor`, `nameservice`, `uart.mmio`, `vm.root`, `log.doorbell`, plus per-board MMIO/IRQ/arena (`fb.memory`, `xhci.mmio`, `xhci.irq`, `xhci.dma`, `virtio.blk.*`) | IRQ caps are issued but IRQ delivery is not wired (see 1.6) |
| Capability ABI (18 types, 25 rights) | Implemented | `shared/include/midix/abi/cap.h` (`MX_CAP_*`, `MX_RIGHT_*`) | — |
| Cap lifetime / revoke / exhaustion regression | Implemented | `tests/capability_probe.cc` + `tests/test_capabilities.py`; prints `capability lifetime, concurrent revoke and exhaustion passed` | Host-only (ASan/UBSan); not a full-boot path |

### 1.2 IPC — rings

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| SPSC shared ring create / share / map | Implemented | `kernel/ipc/ring.cc`: `ring_create`, `ring_share`, `ring_from_cap`; orders 4–10 (16–1024 slots) | Producer/consumer assignment implicit; `ring_share` sets `consumer_pid` on first share |
| Ring objects are refcounted and reclaim pages on teardown | Implemented | `kernel/ipc/ring.cc` `destroy_ring`; `object::create` wiring | — |
| Ring ABI | Implemented | `shared/include/midix/abi/ring.h` | — |
| Same-core + cross-core ring exchange | Implemented | Default boot: `demo.ping: ipc: hot ring ready`, `demo.ping: ipc: cross-core hot ring ready` | Exercised by `demo.ping`/`demo.pong` in the normal boot |

### 1.3 IPC — doorbells

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Doorbell create / share / notify / wait | Implemented | `kernel/ipc/doorbell.cc`: `doorbell_create`, `doorbell_share`, `notify_object`, `wait_doorbell`, `doorbell_counter` | Single waiter slot per doorbell (`db->waiter`); `wait_doorbell` returns `E_BUSY` if a second waiter arrives |
| Synchronous handoff + reply handoff | Implemented | `kernel/ipc/doorbell.cc`: `notify_handoff`, `reply_handoff`; schedule-side `kernel/sched/handoff.cc` (`handoff_take_next`, donation), `SWITCH_HANDOFF` in `runqueue.cc` | Same-CPU fast path only; falls back to a normal notify when eligibility checks fail |
| Cross-CPU wake via IPI + pending-wake queue | Implemented | `kernel/ipc/doorbell.cc` (`wake_local`/`wake_remote`), `kernel/sched/runqueue.cc` (`pending_push`, `drain_pending_wakes`), `kernel/smp.cc` | — |
| Doorbell sleep/wake + cross-core doorbell | Implemented | Default boot: `demo.ping: ipc: doorbell sleep ready`, `demo.ping: ipc: cross-core doorbell ready` | — |

### 1.4 Scheduler — Ressha / Eki

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Per-CPU station (`Eki`), per-band principal/thread fair queues | Implemented | `kernel/sched/runqueue.cc` (`Eki`, `enqueue_fair`, `pick_fair`, `pick_next`); `kernel/sched/fair_queue.cc`; `kernel/sched/sched_class.cc` | Pick order: native RT → latency → VM latency → fair → VM fair → batch → idle |
| vruntime fair scheduling with wake credit and preemption margin | Implemented | `kernel/sched/sched_class.cc` (`effective_key`, `compute_slice_ns`, `wakeup_preempt_margin_ns`), `kernel/sched/accounting.cc` | — |
| Principal budgets / throttle, `SchedContext` budgets | Implemented | `kernel/sched/accounting.cc` (`principal_throttled`, `refill_principal_budgets`), `kernel/sched/sched_context.cc` | — |
| Work stealing (fair classes only) | Implemented | `kernel/sched/steal.cc` (`maybe_send_steal_request`, `handle_pending_steal_requests`) | RT / pinned / VM latency exempt |
| Cross-CPU wake via lock-free MPSC + GICv2 SGI | Implemented | `kernel/sched/runqueue.cc` (`pending_push`, `drain_pending_wakes`), `kernel/smp.cc` | — |
| Timer preemption (~10 ms, PPI 30) | Implemented | `kernel/drivers/timer/generic_timer.cc`; `kernel/sched/runqueue.cc::tick` | Actual switch occurs on the SVC return path (`post_syscall`) |
| SMP bring-up | Implemented | `kernel/smp.cc`; boot: `kernel/smp: smp ready`; lifecycle fixture green at 1/2/4 CPUs | — |
| Scheduler init / dispatch | Implemented | Boot: `kernel/sched: ressha/eki v2 init`, `kernel/sched: eki[0] ready`, `kernel/sched: dispatch tid=… pid=…` | — |
| Dedicated scheduler conformance test | Not implemented | None found under `tests/` | Correctness is argued from the fair-queue code + lifecycle fixture, not a scheduler-specific test |

### 1.5 Memory

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Physical page allocator (bitmap), contiguous alloc, per-owner free | Implemented | `kernel/mm/phys.cc`: `phys_init`, `alloc_page`, `alloc_pages_contiguous_for`, `free_page_for`, `allocated_page_count` | Fixed bitmap cap (`kMaxBitmapBytes`) |
| Kernel heap + slab allocator | Implemented | `kernel/mm/phys.cc`: `heap_init`, `heap_alloc`, `slab_alloc`, `slab_free` | — |
| Per-process address space (TTBR0), EL0 permissions, device maps | Implemented | `kernel/mm/vm_space.cc`: `map_page_raw`, `user_page_attrs`, `user_device_attrs`, `vm_activate`; `kernel/sched/runqueue.cc::activate_thread_space` | — |
| Arena register / claim / unmap / release | Implemented | `kernel/mm/arena.cc`: `arena_register`, `arena_claim_va`, `arena_unmap_page`, `arena_release_va` | — |
| Image mapping / unwind on failure | Implemented | `kernel/init/image_loader.cc`: `load_plan`, `unwind_loaded`, `load_native_image` | Exercises partial map unwind (lifecycle fixture: `page-table allocation failure mid-map: unwound`) |
| Stability under churn | Implemented | Lifecycle fixture: `100 restart cycles: stable objects and pages`, `shared arena: survives creator and capability revocation` | Fixture is a kernel fixture, not full init restart coverage |

### 1.6 Dango VM host

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| VM root / VM / memory / vCPU / channel capability objects | Implemented | `kernel/vm/vm.cc`, `vm_memory.cc`, `vcpu.cc`, `vm_channel.cc`; cap types `MX_CAP_VM*` | Bounded: ≤4 VMs, ≤4 vCPU/VM, ≤32 memory objects, ≤16 channels |
| Stage-2 address space (create / map / unmap / destroy / activate / lookup), table prune | Implemented | `kernel/vm/stage2.cc`: `stage2_create`, `stage2_map_page`, `stage2_unmap_page`, `stage2_destroy`, `stage2_activate`, `stage2_lookup_page`, `prune_tables` | Guest PA window `0x40000000–0x48000000` (`kGuestPhysMin/Max`) |
| vCPU run loop with WFx / virtual-IRQ / MMIO-batch / unsupported-trap exits, software VGIC, EL2 monitor | Implemented | `kernel/vm/vcpu.cc` (`run_vcpu`, `log_unsupported_exit`, guest-console MMIO emulation); `kernel/arch/arm64/el2.S` (`el2_vectors`); `VcpuExitReason` in `kernel/include/anko/vm.h` | Runs non-VHE; EL1 state is saved/restored explicitly |
| VM memory objects (backing pages, map/unmap, write) | Implemented | `kernel/vm/vm_memory.cc`: `create_memory`, `map_memory`, `unmap_memory`, `write_memory` | — |
| Paravirt channels (host↔guest ring + doorbell endpoints) | Implemented | `kernel/vm/vm_channel*.cc`; built-in Linux `/dev/dango-channel0`, 512 ordered round trips via vIRQ/HVC (`AME_VERIFY_CHANNEL=1`). [Phase G delivery](../DELIVERY/gpu-appliance-phase-g.md) | Fixed boot-time channel; no dynamic arenas |
| VM/EL2 bring-up at boot | Implemented | Default boot: `dango.manager: vm.current_el=1`, `dango.manager: vm.el2_available=1`, `dango.manager: vm created`, `dango.manager: vcpu.id=1`, `dango.manager: channel ready`, `dango.manager: layout ready` | — |
| Guest Linux payload run | **Implemented (opt-in)** | Default build still logs `dango.manager: vcpu stub run skipped: no linux guest payload`; the GPU artifact set (`build/dango-linux-gpu`) is packaged when configured with `-DDANGO_LINUX_PROFILE=gpu -DDANGO_LINUX_ARTIFACT_DIR=…` and booted by the opt-in `gpu-appliance` / `kms-display` rows (Linux binds `virtio_gpu`) | Requires the guest artifacts built separately (Linux container on macOS: `scripts/linux/build-gpu-linux-docker.sh`); not part of the default gate |
| VM negative-authority probe | Implemented | Default boot: `dango.auth_probe: authority check passed` | Runs as a native probe, not a guest |
| Stage-2 allocation/prune regression | Implemented | `tests/stage2_probe.cc` + `tests/test_stage2.py` (`test_failed_allocations_and_sparse_unmap_reclaim_tables`) | Host-only |

### GPU appliance — Phases E and F

| Capability | Status | Evidence / limit |
|---|---|---|
| Synthetic PCI ECAM through stage-2 faults | Implemented for INTx bootstrap | `kernel/pci/ecam.cc`, `kernel/vm/vcpu.cc`, syscall 60; real AArch64 guest probe in `vm_device_selftest.cc`, 43 assignment checks. Fixed BARs; only mapped virtio MMIO capabilities; MSI-X table/PBA mappings rejected. |
| Config and mapping isolation | Implemented | ASan/UBSan ECAM + stage-2 probes; trapped ranges reject RAM/MMIO overlap; active-VM revoke removes Device leaves; BME requires an ACTIVE DMA domain. |
| Dango GPU appliance construction (assignment, BAR allocation, ECAM, INTx) | Implemented | `userland/dango/manager/main.cc`, `kernel/init/boot_caps.cc` (`gpu.function`/`gpu.irq`); boot: `assign: ACTIVE device=1`, `assign: bar4 mapped ipa=0x50000000`. |
| Linux `virtio_gpu` binds and creates a DRM device | Implemented | Opt-in `gpu-appliance` row: `dango: DRM card0 driver=virtio_gpu dumb_buffer=1`, `dango: Linux virtio ISR observed`; counters `faults=0 quarantines=0 smmu=1`. |
| Standalone Linux KMS modeset asserted from the host | Implemented | Opt-in `kms-display` row: guest raw-ioctl KMS (`linux-artifacts/kms_test/`), host `screendump` at `1024x768` showing mid-row R/G/B/W. See [Phase F delivery](../DELIVERY/gpu-appliance-phase-f.md). |

See [Phase E delivery](../DELIVERY/gpu-appliance-phase-e.md),
[Phase E.1 delivery](../DELIVERY/gpu-appliance-phase-e1.md),
[Phase F delivery](../DELIVERY/gpu-appliance-phase-f.md) and
[remaining phases](GPU_APPLIANCE_TODO.md).

### 1.7 Kernel drivers (in-kernel minimal set)

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| PL011 UART (QEMU) | Implemented | `kernel/drivers/uart/pl011.cc`; boot console output | Early path only; UART cap handed to `io.console` |
| Tegra 16550 UART (Switch) | Implemented (build only) | `kernel/drivers/uart/tegra_uart.cc`; Switch preset builds | No physical-device run |
| GICv2 | Implemented | `kernel/drivers/gic/gicv2.cc`; `gic-version=2` in `scripts/test/boot.py` | GICv3/GICv4 not implemented |
| Generic timer | Implemented | `kernel/drivers/timer/generic_timer.cc` | — |
| fw_cfg (QEMU, ramfb) | Implemented | `kernel/drivers/fwcfg.cc`; boot: `fwcfg: ramfb 1024x768` | — |
| Kernel-side PCI scan (temporary) | Partial | `kernel/drivers/pci_probe.cc`; boot: `pci_probe: xhci found bus=0 dev=2 bar0=0x10000000 … irq=0` | Documented temporary shortcut; replaced by `drv.pci` (not yet) |
| SDMMC1 (Switch boot log) | Implemented (build only) | `kernel/drivers/sdmmc/sdmmc.cc`; `kernel/init/fat_log.cc` (PIO only) | Switch only; no physical-device run |
| Hardware IRQ bind/ack | **Not implemented** | `kernel/syscall/dispatch.cc`: `SYS_IRQ_BIND_DOORBELL` and `SYS_IRQ_ACK` both `return err(E_NOTIMPL)` | All userspace drivers poll |

---

### 1.8 Panic screen

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Graphical panic screen (logo + headline + detail) | Implemented | `kernel/panic_screen.cc`, `kernel/include/anko/panic_screen.h`; painted from `kernel/panic.cc`; bound in `kernel/init/boot_caps.cc` | Drawn by the panicking core; other cores may still run and repaint (a panic is not yet a system-wide halt) |
| Logo is a generated asset, never hardcoded | Implemented | `kernel/assets/panic_art.txt` → `panic_art.h` via `scripts/mk-ascii-header.py`; `tests/test_panic_screen.py` asserts no source embeds the art | — |
| Font scaling: fit + re-sample to any framebuffer | Implemented | `panic_screen.cc` picks the largest integer glyph scale that fits and re-samples the art when scale 1 would overflow; host probe covers 1024x768/1280x720/1920x1080/640x480/400x300 | Integer scaling only (min 1) |
| Shared 8x16 console font | Implemented | `shared/include/midix/font/vga8x16.h` used by both `io.framebuffer` and the panic screen (generated by `scripts/fonts/psf2_to_header.py`) | — |
| End-to-end capture | Implemented | Opt-in `AME_VERIFY_PANIC=1` (`scripts/test/panic-screen.py`; dev-only `ANKO_PANIC_SCREEN_DEMO`) | Needs a dedicated dev build tree |

## 2. Mochi userland

Sources under `userland/`; each service is a freestanding ELF packed into the signed
`system.mxb` bundle. Boot order and capabilities come from `userland/manifest.v0`.

### 2.1 init / supervision

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Boot manifest parse + service spawn from verified MXB | Implemented | `userland/init/main.cc`; kernel `kernel/init/boot_archive.cc` (`init_archive`, `archive_get`); boot: `kernel/init: authenticated MXB bundle objects=17` | Runtime ELF/tar/embedded fallbacks removed; a rejected signature does not fall back |
| Per-service BootInfo (named entries, generation, cpu.count, sha256) | Implemented | `userland/init/main.cc` `boot_add`, `boot_init_for_service`; `shared/include/midix/abi/boot.h` | — |
| Readiness doorbells + dependency ordering + cycle detection | Implemented | `userland/init/main.cc` (`await_ready`, `dependencies_ready`); boot: `init: system ready`, `init: all services ready` | — |
| Supervision: exit records, dependency-aware restart, crash backoff | Implemented | `userland/init/main.cc`, shared policy `userland/init/supervision.h` (`restart_decision`, `dependent_closure`, `match_identity`) | Steady-state restart exercised by the fixture (see below), not by a long soak |
| Nameservice/FS session servers | Implemented | `userland/init/main.cc` (`SessionServer g_servers`, `refresh_exports`) | Fixed to two server roles (`io.fs`, `nameservice`) |
| Supervision-restart regression fixture | Implemented | `tests/kernel/restart.cc`; gate check `supervision-restart`, marker `supervision regression passed`; sub-assertions: `generation-tagged exits: matched to the right incarnation`, `generation reuse: identities remain unique per PID slot`, `dependency restart: dependents stopped and relaunched`, `restart policy: planned stops exempt, crashes back off` | Kernel fixture reusing the shared policy, not a full init service soak |

### 2.2 nameservice

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Resolve request → capability grant, one SPSC session per client | Implemented | `userland/nameservice/main.cc` (`MSG_NS_RESOLVE` handling, `mx_cap_grant`); `mochi::ServerSessions` in `userland/lib/include/mochi/sessions.h` | Has no dedicated serial "ready" line; liveness is inferred from a successful resolve |
| Resolve exercised end-to-end | Implemented | `userland/demo/ping/main.cc` `resolve_pong()` resolves `demo.pong`; boot reaches `demo.ping: ipc: hot ring ready` | `docs/v0-progress.md` still lists the fixed-table register/resolve flow as unchecked (stale) |

### 2.3 io.console

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Console boot handshake, UART mapping, log-ring drain | Implemented | `userland/console/main.cc`; boot: `io.console: ready` | Holds no framebuffer memory (see `docs/CONSOLE_TTY_SPLIT.md`) |
| Mirror to `tty.fb0` | Implemented | `userland/console/main.cc` (non-blocking write to tty endpoint) | Best-effort mirror |

### 2.4 io.framebuffer + tty.fb0

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Framebuffer owner (maps `"fb.memory"`, clears, serves blits) | Implemented | `userland/drivers/framebuffer/main.cc`; boot: `io.framebuffer: ready` | QEMU path uses ramfb (`1024x768`); Switch uses TegraDC0 (`1280x720`) |
| Text terminal rendering + log mirror + input line discipline | Implemented | `userland/drivers/tty/main.cc`; boot: `tty.fb0: shell terminal attached`, `tty.fb0: ready` | Input dropped until the `system ready` marker is seen (`scan_ready_marker`) |
| Cap isolation invariants | Implemented | `docs/CONSOLE_TTY_SPLIT.md`; `io.console` never maps fb memory, `tty.fb0` never reads kernel log rings | Enforced by grant policy in `userland/init/main.cc` |

### 2.5 io.fs

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| FS core (inode/extent/refcount, ring ABI, CoW+ordering invariants) | Implemented | `userland/fs/fs_core.cc`, `userland/fs/fs_handles.cc`; boot: `io.fs: ... durable /data empty` | Fixed-capacity V1; the journaled on-disk B+tree writer is not landed |
| Block backend via `drv.blk.virtio` | Implemented (when a virtio-blk device is present) | `userland/fs/fs_disk.cc` `io.fs: block endpoint ready`; read in `drv.blk.virtio: BLK read OK`, write+flush exercised by the durability check | Default QEMU boot has no virtio-blk disk → falls back |
| Durable `/data` volume: write-back through `BLK_WRITE` + `BLK_FLUSH` on `FS_SYNC` | Implemented | `userland/fs/fs_durable.cc`; boot: `io.fs: read-only root, volatile /tmp, durable /data replayed`; `durable-persistence` check | Only `/data` is durable; the read-only root's B+tree is never written |
| Memory backend fallback and read-only root + volatile `/tmp` | Implemented | Boot (no disk): `io.fs: read-only root, volatile /tmp, non-durable /data`; `FS_SYNC` returns `MX_ERR_NOT_IMPLEMENTED` | `/tmp` is always volatile; `/data` is non-durable only when no safe disk region is mounted |
| Disk-backed mount (`midix-fs-v1`) | Implemented (direct boot with a virtio-blk disk) | Durability check mounts a writable `midix-fs-v1` image (`read-only root, volatile /tmp, durable /data replayed`) | The U-Boot/AmeFS disk boot remains out of the gate |

### 2.6 msh (shell)

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Interactive shell (rings/caps/BootInfo), parser, builtins | Implemented | `userland/msh/main.cc`; boot: `msh: msh: terminal attached` | Not a full POSIX shell; documented as intentionally bash-like only |
| Declares readiness | Implemented | `mochi::boot::ready(boot)` in `userland/msh/main.cc` | Ready banner goes to the terminal ring, not serial |

### 2.7 Drivers as processes

| Service | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| `drv.pci` | **Not implemented (placeholder)** | `userland/drivers/pci/main.cc`; boot: `drv.pci: placeholder — M10 not yet implemented` | Kernel-side `pci_probe.cc` is the temporary stand-in |
| `drv.usb.xhci` | Implemented (polling) | `userland/drivers/usb/xhci/main.cc`; boot: `drv.usb.xhci: controller ready`, `drv.usb.xhci: keyboard ready` | Polls the event ring; IRQ routing/ack not wired |
| `drv.usb.hid` | Implemented (polling) | `userland/drivers/usb/hid/main.cc`; boot: `drv.usb.hid: ready` | Boot-keyboard reports only |
| `ui.input` | Implemented | `userland/drivers/input/main.cc`; boot: `ui.input: ready` | US boot-keyboard layout normalization only |
| `drv.blk.virtio` | Implemented (device-dependent) | `userland/drivers/blk/virtio/main.cc`; no-disk boot: `mmio bank has no virtio-blk`, `no-device mode`; disk boots: `virtio-blk device detected`, `BLK read OK` | `BLK_WRITE`/`BLK_FLUSH` are exercised by the durability check (the no-device path still runs in the default boot) |

---

## 3. Boot / platform

### 3.1 Kernel entry (live path)

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| AArch64 Image header + `_start`, EL2→EL1 drop, BSS clear, early stack | Implemented | `kernel/arch/arm64/entry.S`, `kernel/arch/arm64/el2.S` | — |
| DTB save, probe, physical init, GIC/timer, caps, IPC, VM, sched, SMP, init spawn | Implemented | `kernel/kernel_main.cc`; boot: `kernel/boot: anko booting`, `kernel/probe: boot_el=2 ram=0x40000000+0x20000000 console=0x9000000 …`, `kernel/init: init queued`, `kernel/boot: scheduler ready` | — |
| Legacy `boot/` shim | **Not implemented / unbuilt** | `CMakeLists.txt` (`add_subdirectory(boot)` commented); `boot/entry/boot_main.cc`, `boot/dtb/fdt.cc`, `boot/mmu/page_table.cc`, `boot/memory/phys_map.cc` are TODO placeholders | Not in the build graph; `docs/boot-sequence.md` describes it as if live |

### 3.2 qemu-virt

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Direct `-kernel` boot | Implemented | `scripts/test/boot.py --mode direct`; gate check `boot-direct` (`all services ready`) | — |
| Boot at 1 / 2 / 4 CPUs | Implemented | Direct boot green at 1, 2, 4 CPUs (see [Verification](#verification)); lifecycle fixture runs the full 1/2/4 matrix in the gate | — |
| Raw-boot + AmeFS disk image (U-Boot firmware required) | Partial | `scripts/build-ame-disk.sh`, `scripts/build-u-boot-qemu-arm64.sh`, `ame_disk` target; `boot_disk.cc` reads the bundle from AmeFS | **Not exercised by the default gate**: needs a locally built U-Boot firmware and the disk image |
| ramfb + PCIe/xhci + virtio-blk probe | Implemented | Boot: `fwcfg: ramfb 1024x768`, `pci_probe: xhci found …`, `kernel/probe: … pcie=1 vblk=1` | — |

### 3.3 switch-t210

| Feature | Status | Evidence | Limitations |
|---------|--------|----------|-------------|
| Board constants, DTS, Tegra UART / TegraDC0 framebuffer / XUSB no-reset, SD log | Implemented (build) | `boards/switch-t210/*`; `ame-switch.dts`; `hekate_ipl.ini` | — |
| Kernel + userland build + Hekate package staging | Implemented (staging) | `CMakePresets.json` preset `switch` (RelWithDebInfo); `scripts/build-hekate-package.sh`; CLEANUP_STATUS notes staging passes | **No physical-device testing**; package validation ≠ device acceptance |
| On-device boot | **Unverified** | None | Requires hardware |

---

## 4. Test matrix and gaps

### 4.1 What the gate covers (`scripts/verify.sh`)

| Check | Command / fixture | Result |
|-------|-------------------|--------|
| Configure (Debug, qemu-virt, dev key) | CMake preset args | green |
| Build | `anko mochi_system` | green |
| Host regressions | `ctest` — `tests/test_capabilities.py`, `tests/test_formats.py`, `tests/test_stage2.py` (ASan/UBSan host binaries) | green |
| Provenance | `generated/provenance.json` written and parsed | green |
| Direct boot (2 CPUs) | `boot.py --mode direct --cpus 2 --marker 'all services ready'` | green |
| Kernel lifecycle matrix | `lifecycle.mxb` at CPUs 1, 2, 4 (`lifecycle regression passed`) | green |
| Supervision restart | `restart.mxb` (`supervision regression passed`) | green |
| Durability persistence | `durable-persistence.py` — write `/data`, `FS_SYNC`, reboot, read back (`durability regression passed`) | green |
| SMMUv3 hostile DMA (opt-in) | `AME_VERIFY_IOMMU=1` — `iommu-dma` row (`iommu_dma.py`) | green |
| PCI assignment + IRQ virtualization (opt-in) | `AME_VERIFY_ASSIGN=1` — `assignment` row (`assignment.py`) | green |
| Linux virtio-gpu appliance (opt-in) | `AME_VERIFY_GPU=1` — `gpu-appliance` row (`gpu-appliance.py`) | green |
| Dango channel (opt-in) | `AME_VERIFY_CHANNEL=1` — `dango-channel` row (`channel.py`) | green |
| Standalone KMS display (opt-in) | `AME_VERIFY_KMS=1` — `kms-display` row (`kms-display.py`) | green |

### 4.2 Manually-confirmed extras (this pass)

| Check | Command | Result |
|-------|---------|--------|
| Direct boot, 1 CPU | `boot.py --mode direct --cpus 1 --marker 'all services ready'` | green |
| Direct boot, 4 CPUs | `boot.py --mode direct --cpus 4 --marker 'all services ready'` | green |

### 4.3 Gaps — explicitly not covered

| Gap | Why | How to exercise |
|-----|-----|-----------------|
| Disk-mode boot | Requires a locally built U-Boot firmware + `ame_disk` image; not part of the gate | `scripts/build-u-boot-qemu-arm64.sh`; `ame_disk`; `boot.py --mode disk --disk … --firmware …` |
| Journaled on-disk B+tree writer | The read-only root is never mutated; durability is delivered by the separate `/data` region | Land the journaled writer, then extend the durability check to root writes |
| Guest Linux / Dango guest boot (default gate) | Linux artifacts disabled by default (`DANGO_BUILD_LINUX_ARTIFACTS=OFF`), so the default boot still logs `vcpu stub run skipped: no linux guest payload` | Covered by the opt-in `AME_VERIFY_GPU` / `AME_VERIFY_KMS` rows (artifacts built via `scripts/linux/build-gpu-linux-docker.sh`) |
| Dango auth regression as a guest | `dango.auth_probe` runs as a native probe (`authority check passed`), not inside a guest | Enable guest path above |
| Switch hardware acceptance | No physical device | Flash and boot the staging package on a T210 console |
| Supervision as a long soak | The restart path is asserted by a bounded fixture | Extend the fixture / soak run |
| Hardware IRQ delivery | `SYS_IRQ_*` return `E_NOTIMPL`; drivers poll | Implement GIC delivery + teardown |

| Scheduler conformance suite | No dedicated test | Add scheduler-specific tests |

---

## 5. Explicitly unverified claims

Anything below has code but **no captured evidence in this tree**; treat as unproven:

- Switch on-device boot and any T210 peripheral beyond compile/link/stage.
- Disk-mode boot end-to-end and the `midix-fs-v1` on-disk mount path.
- Guest Linux boot through Dango in the **default** gate: the opt-in `gpu-appliance` / `kms-display` rows capture a green Linux `virtio_gpu` + KMS run, but the default boot still reports `no linux guest payload`.
- `drv.blk.virtio` against a real backing device: the no-device path runs in the default boot; `BLK_READ`/`BLK_WRITE`/`BLK_FLUSH` run against a virtio-blk disk only in the durability check.
- QEMU boot at CPUs other than 1/2/4 (e.g. 3), and any CPU count above 4 (`kMaxCpus` bounds the scheduler).

## 6. Verification

```
$ cd <worktree> && ./scripts/verify.sh
== toolchain
   Homebrew clang version 23.1.1
   cmake version 4.4.0
   QEMU emulator version 11.1.1
== check: configure            ok
== check: build                ok
== check: host-regressions     ok
== check: provenance
   -> vm-linux-hello-v0-21-gee6c160 Debug qemu-virt key=dev-… mxb=…
== check: boot-direct
   -> ready (1.56s)
== check: lifecycle-matrix
   cpus=1 -> ready (7.17s)
   cpus=2 -> ready (18.42s)
   cpus=4 -> ready (16.72s)
== check: supervision-restart
   -> ready (5.09s)
== verify: all checks passed
EXIT=0
```

Direct-boot CPU sweep (same build tree):

```
$ python3 scripts/test/boot.py --build out/verify --mode direct --cpus 1 --marker 'all services ready'
reason= ready  elapsed=1.67
$ python3 scripts/test/boot.py --build out/verify --mode direct --cpus 4 --marker 'all services ready'
reason= ready  elapsed=1.67
```

This document reflects the tree after Phase G (`DELIVERY/gpu-appliance-phase-g.md`); re-run
`scripts/verify.sh` after changes and update the matrix where behaviour moves.
