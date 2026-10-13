# Native boot diagnostics

The `huawei-anne` Anko target embeds the v6 probe's verified LED, counter and
LPM3 reset transport. It starts before BSS clearing and global constructors;
it requires the verified EL1/EL2, MMU-off, data-cache-off entry state.

Five records in the reserved 1 MiB ramoops area at `0x34800000` receive the
same trace. Their headers and new bytes are cleaned to memory at checkpoints,
newline boundaries and before reset. The area is reserved from allocation and
mapped in every process address space. Logging uses no heap and avoids the unverified UART. After MMU enable,
a per-CPU recursive guard serializes complete messages across cores; early
MMU-off logging uses no exclusive instructions. Each record is bounded; it stops appending when full.

Records start with `AMEANKO1`. `BOOT N name tick=...` identifies the last
checkpoint. `CTOR pc=...` identifies each constructor before it executes.
The checkpoints cover BSS, constructors, probing, memory allocation and MMU,
IOMMU, interrupts, timer, capabilities, IPC, scheduler, authenticated bundle,
init creation, scheduler entry, first timer IRQ and service readiness.

`!FAULT` captures the last checkpoint, ESR, fault PC, FAR and original stack
pointer on a private per-CPU fault stack. `!PANIC` records the panic reason. After a
three-second fault LED hold, `RESET nmi=0x4` precedes the verified LPM3 NMI
pulse. Nested faults bypass logging and attempt reset directly. A hardware hang
that raises no exception still needs the physical fastboot button sequence.

LED stage requests are red at entry, blue at `kernel_main`, green only at service
readiness, and white on fault. Holds last three seconds using the measured
1,920,000 Hz counter rate, also used by the native scheduler, timer and kernel
timestamps. The v6 phone test showed red, purple, purple, then reset: physical
LED output worked but selected colours were mixed. The current native LED
sequence fixes unselected output-control pairs, and the v3 eight-core capture
physically showed red, blue, green, reboot. The archived v6 probe is unchanged.

Build and package:

```sh
cmake --build --preset anne-native-dev --target anko
python3 scripts/anne/build-native-package.py --output out/anne-boot-trace-package
```

For a bounded phone capture, configure with
`cmake --preset anne-native-dev -DANKO_ANNE_DIAG_CAPTURE_SECONDS=10` before
building. This resets through LPM3 as soon as init reports service readiness,
on a userspace fault, or after ten seconds from scheduler entry when a timer interrupt or syscall
next checks the deadline. It records `CAPTURE reason=services.ready`,
`CAPTURE reason=el0.fault` or `CAPTURE reason=timeout`. Userspace exceptions
also leave a `!USERFAULT` line with process/thread IDs and fault registers;
those PCs belong to the userspace image, not the kernel ELF. Set the option
back to zero for normal operation.
This is a diagnostic run, not a ten-second limit on the production target.
A CPU stuck without interrupts or syscalls still needs physical recovery.

With `ANKO_ANNE_DIAG_RESET_AT_READY=OFF`, the configured interval measures
runtime **after service readiness**, including framebuffer readiness. Startup
retains a separate timeout of at least 30 seconds from scheduler entry. A slow
boot therefore records `timeout` rather than claiming `runtime.observed` before
services are ready. Watchdog retention uses the same runtime interval.

The package saves the exact `kernel.elf` and raw Image with hashes, alongside
the AVB container and stock restore image. Use that ELF to decode this run;
later builds can move instruction addresses.

After testing, restore the full stock `kernel` image, then on this phone use
`fastboot reboot bootloader` and allow recovery to start. This session verified
that route to installed TWRP; `fastboot reboot recovery` enters eRecovery.
The user reconfirmed this route during native USB work: use `reboot bootloader`,
not `reboot recovery`. Check the actual transport after the command and use the
installed-TWRP button route if recovery does not appear; a reboot acknowledgement
alone does not verify TWRP startup.
Do not issue overlapping USB writes or assume the shared recovery kernel can
boot while it still contains Anko.

Collect and decode after stock restoration:

```sh
python3 scripts/anne/collect-native-log.py \
  --package out/anne-boot-trace-package \
  --output out/anne-device-private/boot-trace-results
```

The collector verifies TWRP, the stock kernel checksum and the packaged ELF.
It copies available pstore records, saves their text and decoded fault symbols,
and prints only the diagnostic summary. `--offline` decodes previously saved
records. It does not flash, reboot or modify the phone.

Local validation uses `anne-trace-qemu-dev` with synthetic RAM-backed transport
addresses, away from the DTB and kernel. It checks full EL1 and EL2
boots through service readiness, faults before constructors and after MMU
enable, and retention in all five copies across address-space switches. The
synthetic build is a QEMU target and is rejected by the native package builder.

This validation found a real native bundle mapping bug: `.balign 16` left the
embedded MXB physical base unaligned for init's page mapping. The embedding
now aligns and pads the bundle to 4 KiB without changing its signed contents.
This fixes the local init failure; the subsequent phone capture also reached
init and service readiness.

The first full instrumented phone run showed red, purple, then a persistent
second purple stage. Those requests occur at entry, `kernel_main` and scheduler
entry. Stock restoration and TWRP boot succeeded, but neither pstore nor the
DFX backup retained an `AMEANKO1` record after the physical button reset.
That observation does not prove an exception or a service-readiness failure.
The bounded capture subsequently retained five identical 2916-byte records.
All checkpoints 1–36 were reached, including MMU enable, the first timer IRQ,
signed bundle authentication, init's first syscall, `io.console: ready` and
`init: all services ready`. There are no kernel faults, userspace faults or
panics. `CAPTURE reason=services.ready last=36` immediately precedes the NMI
reset. Stock restoration and TWRP startup are checksum-verified. This establishes
one-core native startup with the selected minimal services; it does not establish
longer-running stability, framebuffer or native USB operation.

The successful capture package is `out/anne-boot-trace-package-v2b/`, image
SHA-256 `104514e6eac19ae1a7a8f0030e24fdb27c87fc641e454be283aae7c2e5063bcf`.
Its matching ELF and private decoded records are retained alongside the run.
Kernel logs are also mirrored directly into the
trace after console ring attachment, so a stalled console service cannot hide
the scheduler's dispatch or userspace fault messages.

Eight-core native startup is now verified; see [SMP](SMP.md) and the
[framebuffer handoff](../../docs/HUAWEI_FRAMEBUFFER_HANDOFF.md) for the latest
LED correction, hardware evidence and recovery state.
