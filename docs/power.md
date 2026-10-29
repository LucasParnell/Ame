# Power subsystem

`kernel/power` owns system transitions, CPU idle residency, battery/charger
telemetry, thermal sampling, and frequency-domain policy. `anko/power.h` is its
kernel interface; `midix/abi/power.h` is the fixed-size userland snapshot.
SMP supplies the immutable PSCI topology before releasing secondary CPUs.
Native power-hold/reset and gauge helpers moved from board headers into
`kernel/power/anne`. Platform clock/reset gates remain with capability-owned
platform controls; USB boost and I2C ownership remain in userland.

Scheduler WFI entry/exit update one 64-byte `libk::IdleAccount` per CPU with IRQs
masked. The wake protocol still checks queues with IRQs masked and enables IRQs
after WFI. Snapshot readers retry at most four times. Busy runtime accounting
continues to belong to the scheduler. No firmware, I2C, global lock, allocation,
or governor work runs at context switches. Residency includes a live WFI
interval and excludes the interrupt handler after wake.

`diag power` queries `MX_DIAG_POWER`. The existing status bar uses the same
one-second battery cache through its unchanged health ABI. The profiler mailbox
accepts operation 3 and returns the same binary `MxPowerSnapshot` in the load
mailbox (`len=0` on failure). This is on demand and adds no polling wakeups.

The Hi6523 owner publishes STATUS0/CHG_REG0 once a second using the exclusive
`charger.observe` platform capability. These describe input buck-good,
charge-enable, charge-done, source output and charger thermal limiting. Enabled
charging is not a measurement of charging current. Failed reads invalidate the
sample; process teardown also invalidates it. Samples expire after three
seconds. Charger settings and battery percentages are not inferred from voltage.

Hi6250 thermal sampling uses the vendor EL3 read-temperature call 0xc5009900,
indices 0/1/2 for the two clusters and GPU. Discovery requires the captured DT
calibration (ADC 116..922, -40..125 Celsius). Samples are read at boot, through
the charger owner's existing one-second maintenance loop, and on diagnostics.
No scheduler or IRQ path issues sensor SMCs. Invalid readings expire after two
seconds and force the affected CPU domain to its minimum OPP. The governor uses
the hotter of the cluster and GPU readings: cooling begins in the DT's 60–80 C
passive region and reaches the minimum OPP at 80 C, with 1 C recovery hysteresis.
Manual thermal ceilings are also enforced. Battery NTC temperature remains
unavailable; Ame does not enable charging without a verified charging policy.

Battery current is now read from the signed Hi6555v100 CURRENT_0/1 register pair
with bounded high/low/high retries. It uses the vendor 10 mOhm conversion and DT's
nominal gain; Android NV current calibration is not loaded. The ABI explicitly
marks this nominal calibration. Positive current means discharge, negative means
charge. Voltage and current share the existing one-second sensor cache.

The Hi6523 service preserves the prior charge-enable bit across OTG sessions,
restoring it only after boost has been disabled. A failed boost-disable write
keeps the source state uncertain and retries shutdown rather than reporting it
off. A faulted source no longer gets its watchdog renewed. It never invents a
charging-enable state that firmware did not supply.

Frequency domains are bounded tables grouped by CPU clock phandle, mapped to
logical CPUs using the discovered MPIDRs. This first parser supports ascending
legacy `operating-points` pairs, which the captured Anne DT and its xfreq driver
use alongside OPP-v2. Malformed/truncated DT data never publishes a partial
result. Unsupported tables produce no domain. Anne's SCBAKDATA4 indices are
reported as firmware observations only when the xfreq node matches the protocol;
invalid indices are unavailable. These are firmware-reported OPPs, not measured
clock rates.

The governor runs from CPU0's existing 100 Hz timer maintenance and uses the
busiest online core's idle delta, with 25% headroom. Requests rise immediately
under load; downward changes wait 80 ms after the preceding burst. The boot CPU's
active accounting interval initializes the same policy before secondaries are
released, so the initial boot boost follows CPU activity rather than a separate
boot-frequency override. Offline domains are not requested.

Anne registers a real LPM3 backend after verifying the DT's IPC page, unlock key,
mailbox 13 source/destination, and cluster commands. Each poll performs a bounded
number of MMIO accesses. Transactions use auto-ACK, unmask only LPM3, and never
wait or spin for firmware. Completion requires both ACK and the requested OPP
index in SCBAKDATA4; an ACK alone does not mark a clock transition applied. Busy
or foreign channels are not seized. A 50 ms timeout or access failure latches a
fault without force-releasing a live remote transaction. LPM3 owns voltage and
clock sequencing. The IPC page is mapped Device-only in every kernel address
space and is not granted to userland.

`diag power` exposes requested, applied and observed rates, transition/failure
counts and status independently. `MX_PLATFORM_CPU_FREQUENCY` remains restricted
to init's system capability. Unsupported platforms retain read-only telemetry.
`diag power load [100..5000 milliseconds]` exercises a CPU burst and reports peak
observed rates. `cmake --build <build> --target power_load_probe` builds a separate
eight-core native workload at `userland/test.power_load.mxi`; upload it through
`scripts/anne/tools/app-stream.py` after `container start`. It never enters the normal
system bundle or writes phone storage.

Primary references:

- [Clock mailbox transport](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/clk/hisi/hisi-clk-mailbox.c).
- [HiIPCV230 transactions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/mailbox/hisi_mailbox/hisi_mailbox_dev.c).
- [Hi6250 xfreq implementation](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/clk/hisi/clk-hi3xxx.c).
- [Hi6523 register map](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/ap/platform/hi6250/soc_schargerV300_interface.h).
- [Hi6250 thermal service/conversion](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/thermal/hisi/hisi_tsens.c).
- Gauge and reset provenance: `boards/huawei-anne/STATUS_BAR.md` and `POWER.md`.

Downloaded references are retained in `out/power-research`; the captured DT is
`out/anne-device-private/recovery-inspection/observations/firmware.dts`.

## Validation, 18 October 2026

Validation was run after implementation. ASan/UBSan probes passed for concurrent
idle publication, runtime load recommendations, OPP rounding, thermal ceilings,
backend failures, sample expiry/invalidation, voltage/register access and thermal
ADC conversion. Scheduler, framebuffer/status, libk, CPU topology and descriptor
regressions passed. QEMU's real shutdown/reboot paths passed. The Anne USB release
kernel and its signed userland bundle built successfully.

`tests.test_power_subsystem` additionally checks the captured private Anne DT
when present: eight CPUs, both frequency domains and all nine OPPs, with a big
cluster core mapped to logical CPU zero. Other checkouts skip this private test.

The currently running phone image returned ADC 499/499/493 for cluster0,
cluster1/GPU through the existing CDC `diag el3-nn` command, confirming the vendor
read-temperature service responds on this firmware. These correspond to about
38.4/38.4/37.2 C with the DT transfer function. This initial live firmware smoke check preceded deployment. Reference
transcripts/source hashes are in `out/power-research`.

### Native deployment

At the user's request, `anne-power-v1` was flashed to the kernel partition and
booted successfully. Package: `out/anne-power-package-v1`. Image SHA-256:
`864e19ca1bcf9e113d7c34ee76327c8e9015a8f2e91f7bbd76a5e9efb048f1de`.
The package includes the verified stock restore image. Fastboot acknowledged
both the write and reboot; the retained log confirms `RUN tag=anne-power-v1`
and all services ready at the 0.260-second log timestamp. Entry-to-ready was
384.098 ms; the periodic log clock starts later and omits early kernel work. CDC returned to msh with eight CPUs.

Two `diag power` samples verified live thermal updates, increasing per-CPU
idle residency/entry counters, and valid interval loads on both clusters.
Battery voltage was 3782 mV. Charger input was present with charge-enable,
charge-done, boost source and thermal-limit bits clear. This does not establish
active battery charging. Cluster temperatures were approximately 37.8–38.0 C
and GPU 36.6 C. Firmware OPP observations were 1306000/1402000 kHz; idle governor
recommendations fell to 480000/1402000 kHz while control remained unavailable,
in v1. Hardware DVFS is enabled and verified in v2; battery temperature remains unavailable.

Flash state, acknowledgements and CDC diagnostics are retained privately under
`out/anne-device-private/power-v1`.

### Scheduler-driven DVFS deployment (v2)

`anne-power-v2` is flashed and running. The package is
`out/anne-power-package-v2`; image SHA-256:
`d5b056d1c55ab87c38667c4e6a906a8741bef20a34a4b7785c7f7492b9237080`.
All 24 targeted tests passed after implementation, including ASan/UBSan mailbox
state-machine, ownership, ACK/readback, timeout, governor load/cooling, current
conversion and OTG charge-state tests, scheduler/USB/framebuffer/descriptor
regressions, and real QEMU power transitions.

The native eight-worker workload reached 1709000 kHz and 2362000 kHz on the two
clusters, exited zero, and recorded zero DVFS failures. Afterwards, observed and
applied frequencies returned to 480000/1402000 kHz at idle. Boot logs show the
first little-cluster request before bundle authentication and its confirmation
at the 0.110-second kernel log timestamp. Cluster/GPU temperatures continue to
update automatically without shell polling. Gauge current changed between zero
and -937 uA in the observations; these near-zero values do not establish active
charging, and the charge-enable bit remained clear.

Three kernel-entry-to-all-services-ready measurements were
371.694, 391.615, 351.656 ms; median
371.694 ms. The single recorded v1 baseline was
384.098 ms, so the median is about 3.2% lower, but the 40 ms run-to-run range is
larger than that difference. This is not a statistically established whole-boot
speedup. In the first paired observations, bundle loading/authentication fell
from 53.381 to 40.753 ms and init parsing from 21.554 to 16.497 ms, consistent
with increased CPU frequency. The longest io.fs readiness wait stayed near
84 ms and the overall service-launch interval still dominated boot.

The phone remains at the normal msh prompt after the tests. Reboot measurements,
clock/sensor diagnostics, fastboot acknowledgements and workload results are in
`out/anne-device-private/power-v2`.
