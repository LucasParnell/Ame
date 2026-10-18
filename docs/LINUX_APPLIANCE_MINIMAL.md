# Linux appliance — minimal configuration policy

Applies to every Dango guest kernel (`linux-artifacts/config/*.fragment`).

## Principle

The Linux guest exists **only** to run a hardware driver and a paravirtual channel back to Ame
(`docs/architecture.md §16`, "Linux as Driver Appliance"). It is not a general-purpose system. Every
kernel feature that is not required by (a) boot on arm64/QEMU `virt`, (b) the assigned-device flows,
or (c) the Dango channel is **off**. A fat guest is a bug: it enlarges the attack surface, slows
boot, and hides accidental dependencies.

Corollary: **prefer `=n`.** Accepting a subsystem "because defconfig enables it" is not allowed.

## Mechanism

Start from `defconfig`, then layer exactly one fragment with `scripts/kconfig/merge_config.sh`, then
`olddefconfig` — never hand-edit `.config`:

```
make LLVM=1 ARCH=arm64 O=$KOUT -C $DANGO_LINUX_SRC defconfig
$DANGO_LINUX_SRC/scripts/kconfig/merge_config.sh -m -O $KOUT $KOUT/.config <fragment>
make LLVM=1 ARCH=arm64 O=$KOUT -C $DANGO_LINUX_SRC olddefconfig
```

- One fragment per profile. `dango_hello_arm64.fragment` stays the minimal bring-up profile; the GPU
  appliance is a **separate** fragment so boot regressions bisect cleanly (spec §19).
- Helpers must be pulled in by Kconfig `select`/`depends`, not duplicated by hand (spec §10).

## Baseline deny-list (must be `=n` in every appliance)

`MODULES, BLOCK, NET, PCI †, USB, SCSI, ATA, SOUND, INPUT, WIRELESS, BT, CGROUPS, NAMESPACES, AUDIT,
SECURITY, BPF_SYSCALL, PERF_EVENTS, FTRACE, KPROBES, SWAP, HIBERNATION, SUSPEND, SMP, CPU_IDLE, VT`

† `PCI` is off in the minimal (hello) profile; the GPU profile turns on only `PCI`, `PCI_HOST_GENERIC`,
`VIRTIO`, `VIRTIO_PCI` (plus Kconfig-selected helpers), and nothing else.

Also: no filesystem beyond the initramfs needs, no net, no modules, single vCPU (`SMP=n`), no cpuidle.

## Known forced-on (empirically verified 2026-09-20, Linux v6.12 / arm64)

A `=n` in a fragment is only effective if nothing else `select`s the symbol.
These were observed to resist `=n`; the checker reports them as warnings:

| Symbol | Why it stays on | Fix |
|---|---|---|
| `SMP` | arm64 Kconfig: `config SMP def_bool y` — **architecture-forced**, not user-settable | none (arm64 is SMP-only) |
| `CGROUPS` | `SCHED_AUTOGROUP=y` (defconfig default) `select`s it | **fixed**: the hello fragment now sets `SCHED_AUTOGROUP=n` (+ `SCHED_MC`/`SCHED_SMT`/`CPU_FREQ=n`) |
| `CPU_IDLE` | `default y if ACPI`; forcing path not yet removed | investigate (drop ACPI, or find the selector) |
| `PERF_EVENTS` | `default y`; forcing path not yet removed | investigate |

Lesson: when a deny-list entry will not turn off, find its `select`/`def_bool`
parent rather than re-adding the `=n` line.

## Per-flow allow-lists

| Profile | Adds (beyond the arm64 boot baseline) |
|---|---|
| `dango_hello_arm64` | nothing — boot + PL011 console only |
| `dango_gpu_arm64` (virtio-gpu, spec §10, Phases E–F) | `PCI`, `PCI_HOST_GENERIC`, `VIRTIO`, `VIRTIO_PCI`, `DRM`, `DRM_VIRTIO_GPU` (+ helpers such as `DRM_KMS_HELPER`) |
| `dango_gpu_amd64`-equivalent (amdgpu, Phase Q, future) | the virtio-gpu set minus `DRM_VIRTIO_GPU`, plus `DRM_AMDGPU` and its firmware/FW_LOADER deps |

Nothing else is admissible without a written, empirically-grounded justification recorded here.

## Enforcement

The build calls `scripts/linux/check-minimal-config.py` on the resolved `.config`; a violation fails
the build. It asserts the deny-list is off and the flow's required set is on:

```
python3 scripts/linux/check-minimal-config.py --config $KOUT/.config --flow gpu
```

## Review checklist (for any fragment change)

1. Does the resolved `.config` pass `check-minimal-config.py` for the profile?
2. Was any `=y` added that is not in the profile's allow-list (directly or via `select`)?
3. Did the change enlarge `.text`/initramfs or boot time measurably? If so, why?
4. Is the helper set pulled in by Kconfig rather than written by hand?
5. Does the minimal `dango_hello_arm64` profile still boot (bisectable)?
