# Eight-core Ressha bring-up

The captured ANE-LX1 DTB lists eight CPUs at affinities `0,1,2,3,100,101,102,103`
in two clusters; the second cluster is listed out of numeric order. Recovery
also reports CPUs `0-7` online and possible. Native bring-up uses the DTB's SMC
conduit and `CPU_ON` function `0xc4000003`.

The native target now supports eight host CPUs. The shared kernel/userland limit
also sizes boot log rings, per-core stacks and EL2 state. Other boards retain
their four-core limit. The topology parser honours disabled nodes, one- or
two-cell affinities, the PSCI conduit and boot-core identity. Logical CPU zero
is the executing boot CPU; remaining identities are sorted by full affinity.

Before starting CPUs, the boot core cleans the kernel image, bootstrap data and
identity table to memory. Secondary assembly identifies the physical core from
that table, selects its own stack, masks interrupts and invalidates stale
instruction-cache entries. It enables the shared kernel mappings, installs
vectors, initializes its GIC interface and timer, then publishes online status.
Secondaries wait at a release gate while the primary finishes startup.

Each online core owns a Ressha/Eki station. An atomic online mask prevents
scheduling to failed CPU slots; count and mask are passed to init and services.
GIC wake SGIs use the interface's banked target mask rather than assuming a
logical ID equals a hardware target bit. Queue publication precedes SGI delivery.
Global userspace timestamps still use the boot CPU's 100 Hz tick count; per-core
ticks and scheduler/IPI statistics are retained separately.

Native tracing serializes records across cores only after normal-memory
attributes are installed. Each core has its own fault stack and recursion guard.
Kernel messages and debug-output syscalls write whole messages under the trace
guard, so concurrent CPU startup cannot interleave checkpoint lines.

The green LED request occurs at `BOOT 36 services.ready`, after the existing init
readiness message. Colour changes clear the previous channel enables and restore normal output
control before configuring the next channel. The stock recovery driver selected
only green, and the user confirmed the physical LED was green. The first native
eight-core capture reached readiness but appeared purple; its unselected output
control pairs had been forced to 3. The corrected sequence clears those pairs.
The final v3 native capture physically showed red, blue, green, reboot;
its test and recovery evidence are tracked in the framebuffer handoff.

The register sequence follows the matching public EMUI 8 Huawei
[LED driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/lineage-20/drivers/leds/hisi_leds.c)
and [LED definitions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/lineage-20/include/linux/hisi/hisi_leds.h),
using register locations and currents from the actual captured DTB. The vendor
[PSCI CPU operations](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/lineage-17.1/arch/arm64/kernel/psci.c)
also use the full physical affinity to start each CPU.

## Validation workload

The isolated `huawei-anne-smp` profile adds `test.anne_smp` to nameservice and
console. It creates eight suspended helper threads, pins one to every core with
a Ressha scheduling context, and releases them together. An arrival barrier
checks shared-memory visibility across cores; each worker verifies its actual
CPU and computes for 200 ms before notifying completion. The main thread blocks
on a doorbell, then reports readiness only after all eight have completed.
Workers use the latency band because init polls readiness there: fair-band
workers on CPU0 would be starved while init is runnable in a higher band.

The trace records `SMP_WORK cpu=N observed=N rounds=...`, followed by
`SMP_WORK complete mask=ff workers=8`. At readiness it records every core's
dispatch/switch count, timer accounting, tick count and handled IPI count. The
initial idle-to-thread dispatch now counts as a context switch, so a core that
runs its first worker no longer appears unused.

```sh
cmake --preset anne-smp-native-dev
cmake --build --preset anne-smp-native-dev --target anko provenance
python3 scripts/anne/build-native-package.py --build out/anne-smp-native-dev \
  --output out/anne-smp-capture-package
```

This preset deliberately resets at readiness, faults or the ten-second runtime
capture deadline. The normal `anne-native-dev` profile keeps the two production
services and leaves the green LED selected after readiness, with capture reset
disabled. Package the exact build and use its saved ELF for fault decoding.

Local tests cover eight-core EL1 and EL2 startup, worker execution, per-core
timers and real SGI handling, green-stage ordering, faults before constructors
and after MMU enable, capture reset, and retention of all five record copies.
Parser tests cover both clusters and shuffled DT order, booting from the second
cluster, disabled CPUs and invalid conduits. Four-core lifecycle and supervision
matrices also pass with the shared changes.

## Verified phone capture

`out/anne-smp-capture-package-v2` ran natively at EL1. All seven secondary
`CPU_ON` calls returned zero; all eight CPUs came online (`mask=ff`). Each
pinned worker ran on its assigned core and completed, with positive switch,
preemption, timer-tick and handled-IPI counts on every core. Init reached service
readiness, and five records survived the intentional LPM3 reset. No kernel fault,
userspace fault or panic was recorded. Stock restoration and TWRP startup were
checksum-verified. This proves bounded eight-core startup and the test workload;
it does not establish long-term stability or load balancing under all workloads.

The exact package image SHA-256 is
`22c047bdb2f5a495513ac61685c0581950ad373bdcde5e928e4462f8434ab332`.
Decoded records are under
`out/anne-device-private/smp-capture-v1-test/results/`. The physical LED sequence
was red, purple, purple, reboot; the revised output-control sequence was built
after this capture. See [framebuffer handoff](../../docs/HUAWEI_FRAMEBUFFER_HANDOFF.md)
for the latest device state and follow-up artifacts.
