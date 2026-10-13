# Boot-time speed-up plan (for a fresh session)

Repo: `/Users/lucasparnell/Documents/Code/Ame` (branch `master`).
Read `docs/HANDOFF_2026-10-13-evening.md` for the native USB work context;
this project is separate: make the native kernel reach services-ready fast.
Do not touch the USB-attach track here.

## Goal

Cut kernel entry -> `init: all services ready` from ~13.5 s to < 2 s.
Current prediction: ~7.5 s after the first fix (done, see below); ~4.5 s
after the probe work; < 2.2 s after all Tier 2 items.

## Measured budget (stable across v8/v9/v10 builds; 1.92 MHz counter)

| Window | Time | Cause |
| --- | --- | --- |
| entry -> bss.begin | 3.003 s | `led_hold('r')` blocking LED dwell **(REMOVED)** |
| bss clear | 0.080 s | uncached clear |
| kernel_main -> console.ready | 3.004 s | `led_hold('b')` blocking dwell **(REMOVED)** |
| probe.begin -> probe.done | 3.08 s | unexplained; code is small (see Tier 1) |
| physical.done | 0.177 s | work |
| reservations.done | 0.759 s | DTB re-walks, uncached |
| heap.done | 0.030 s | |
| mmu.begin -> mmu.enable | 1.831 s | page-by-page 4 KiB page tables, uncached |
| iommu/gic/timer/caps/ipc/vm/scheduler | ~0.14 s | |
| smp.done | 0.117 s | |
| bundle.authenticated | 0.290 s | SHA over the bundle |
| init.spawn.done | 0.034 s | |
| services.ready | ~0.89 s | services started *serially* |
| **entry -> ready total** | **13.5 s** | |
| post-ready | +3 s | `led_hold('g')` **(REMOVED)** |
| reset path | +3 s | `led_hold('w')` **(REMOVED)** |

Verified facts — do not re-litigate:

- `CNTFRQ_EL0` reads 16384 (garbage) but the counter truly advances at
  1.92 MHz: measured against recovery CLOCK_MONOTONIC and confirmed by the
  3.000 s LED holds. Trust `counter_hz=1920000` from the record header.
- The SP805 watchdog is 60 s (load 0x1e0000 @ 32.768 kHz) and is fed by every
  boot checkpoint (`watchdog::poll`, >=1 s cadence) and by the bundle-auth
  progress hook. Nothing depended on the removed hold loops for feeding.

## Status

- **REMOVED (checkpoint 1):** the three blocking LED holds plus the reset
  hold. `boards/huawei-anne/minimal/timing.h` (`kLedHoldTicks = 0`) and
  `boards/huawei-anne/boot_diagnostics.cc` (`led_hold` no longer waits).
  LED colour transitions remain (set calls kept).
- **DONE (checkpoint 2):** two-phase service launch in
  `userland/init/main.cc` — `launch_prepared_services` prepares/resumes every
  dependency-ready service first, then awaits readiness, so independent
  services overlap across CPUs. Supervision keeps `start_service_now`
  (prepare + await; unchanged semantics). Expect the 0.89 s tail to shrink to
  ~0.3-0.4 s. Validated in QEMU (boot smoke + xHCI->HID keyboard chain).

**FIRST ACTION: build + flash + measure to confirm entry->ready ~7 s.**
If it is not ~7 s, re-derive with the stage table before any other work.

## Tier 1 — explain `probe` (~3.08 s)

`kernel/platform/probe.cc` + `fdt::walk`; the code between the checkpoints is
small, so this is either another fixed wait or an uncached DTB cost. Add 3-4
`ANKO_BOOT_CHECKPOINT`s (new enum entries in
`boards/huawei-anne/boot_diagnostics.h` + `kStageNames`) around: DTB
validation; alias/console walk; main walk; tail. One dev build answers it.

## Tier 2 — real work (~2.5 s)

- `mmu.begin -> mmu.enable` 1.83 s (`kernel/mm/vm_space.cc`): use 2 MiB block
  descriptors for the large linear ranges (kernel image, `kDebugRamBase`,
  DSS/logo ranges), keep 4 KiB level-3 mappings only where required (device
  windows / rights guard). Batch writes; single final TLBI.
- `reservations` 0.76 s: fold the DTB passes (`platform::probe` walk,
  `reserve_dtb_regions`, `discover_initrd`) into one traversal; skip
  unneeded subtrees early; avoid repeated string compares.
- `services.ready` 0.89 s: **DONE** — see checkpoint 2 above.
- Bundle auth 0.29 s: overlap with other phases or split the SHA across
  CPUs (SMP is up before it runs).

## Tier 3 — polish

- BSS clear 0.08 s (wider stores once caches can come on sooner).
- Re-check one capture run end-to-end after the reset-hold removal
  (reset pacing changed by 3 s; capture semantics unchanged).

## Measure loop

Retained traces only exist for builds with `ANKO_ANNE_DIAG_CAPTURE_SECONDS > 0`
(retention is committed BY the capture reset). For measurement runs, either
build with `-DANKO_ANNE_DIAG_CAPTURE_SECONDS=60`, or read the BOOT lines off
the phone screen directly. Parse stage deltas:

```python
import re, glob
rec = sorted(glob.glob('out/anne-device-private/usb-native-*/dfx-records/record-*.txt'))[-1]
seen = {}
for m in re.finditer(r'BOOT (\d+) ([\w.]+) tick=0x([0-9a-f]+)', open(rec).read()):
    sid, name, tick = int(m.group(1)), m.group(2), int(m.group(3), 16)
    if sid == 35 or sid in seen: continue
    seen[sid] = (name, tick)
prev = None
for sid in sorted(seen):
    name, tick = seen[sid]
    print(f"{sid:3d} {name:24s} delta={(tick - prev) / 1.92e6 if prev else 0:7.3f}s")
    prev = tick
```

Build / flash / restore / collect (short form; full details in the evening
handoff, "Tooling & procedures"):

- build:   `cmake --preset anne-usb-native-dev && cmake --build --preset
  anne-usb-native-dev --target anko mochi_system provenance -j 8`
- package: `python3 scripts/anne/build-native-package.py --build
  out/anne-usb-native-dev --output out/anne-usb-native-package-vNN`
- kit:     copy a recent kit dir, update `control.py` package path +
  `state.json` hashes
- flash:   phone in FASTBOOT; `cd <kit> && python3 control.py preflight &&
  python3 control.py flash && python3 control.py reboot`
- restore: `control.py restore && control.py bootloader_reboot` (then the
  on-phone button route to TWRP)
- collect: `python3 scripts/anne/collect-native-log.py --package ... --output
  ... --dfx-fallback` (in TWRP)
- capture/restore is pre-authorized: don't re-ask; verify target, hashes and
  transport.

## Guardrails

- Keep the LED set calls (colour transitions); only the dwell is gone.
- Every change must keep capture runs working (retention via reset).
- Verify no regressions: all services reach ready; reserved-region list
  identical; IOMMU list unchanged.
- The working tree may contain unrelated uncommitted work from other
  sessions — commit only the files you touch.
- git identity is auto-derived (cosmetic).
