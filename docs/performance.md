# Performance measurement and recorded results

These are recorded checkpoints, not a fresh benchmark of the current tree. Keep host, QEMU TCG and physical-phone evidence separate. Workload, compiler, placement and measurement boundaries must match before comparing numbers. Current scheduling and transport behavior is in [architecture](architecture.md), [scheduler](scheduler.md) and [IPC](ipc.md).

- [IPC benchmark](#ipc_benchmark)
- [Cortex-A53 IPC results — verified checkpoint](#ipc_floor_results)
- [IPC baseline — 2026-10-18](#ipc_baseline)
- [Anne runtime CPU measurements](#runtime_performance)
- [Release boot performance](#boot_performance)
- [Framebuffer rendering and acceleration](#framebuffer_performance)
- [Boot, runtime and library refactor](#performance_refactor)

<a id="ipc_benchmark"></a>
## IPC benchmark

<a id="ipc_benchmark-ipc-benchmark"></a>

This measures application-visible communication between two processes in separate
address spaces. The same C++ workload runs on Ame rings, Linux Unix-domain
`SOCK_SEQPACKET`, and a Linux pipe pair. It measures copied messages with blocking
waits by default. It is suitable for comparing that service contract across OSes
on a matching machine, and for measuring regressions within Ame.

The default QEMU model is now Cortex-A53, matching the primary P20 Lite ISA.
Use `--cpu cortex-a57` only when reproducing the historical emulator setup.
See [the current IPC paths](ipc.md#ipc_floor) for ASIDs, packed framing and combined
waits; the saved baseline documents describe the earlier implementation.

[The initial results](performance.md#ipc_baseline) include raw timings and configuration. Ame
was measured under QEMU TCG; the Linux references ran on the host. Those results
are separate baselines and must not be used to rank the operating systems.

<a id="ipc_benchmark-run"></a>
### Run

Requirements: Python 3, Clang C++23, and Linux for the reference adapters. Ame also
needs the normal AArch64 build tools, CMake, Ninja, QEMU, and the Python signing
dependency from `requirements-build.txt`. In the prepared workspace:

```sh
source /workspace/.ame-tools/activate.sh
python3 scripts/test/ipc-bench.py ame --output out/ipc-results/before
python3 scripts/test/ipc-bench.py linux --transport seqpacket --output out/ipc-results/linux-unix
python3 scripts/test/ipc-bench.py linux --transport pipe --output out/ipc-results/linux-pipe
```

Output directories must be new. Each contains `results.json` with every raw
sample, `summary.csv`, build logs and serial/process logs. New runner captures
also preserve the Linux binary or Ame kernel and per-placement signed bundles.
The Ame runner uses a separate `out/ipc-bench` build and an isolated signed
`ipc-bench.mxb`; `system.mxb` remains the normal system bundle.

Defaults: three independent process pairs per case, five aggregate rounds of
1,024 operations, 256 warmup operations, 4,096 individually timed RTT samples
per RPC case, and a streaming ACK window of 32. Ame starts a new QEMU boot for
each repetition **and** placement: six boots, three observations per case.
Its affinity API applies only to suspended threads, so the bootstrap pins both
workers before resume. Linux pins its forked server before each case.

For a focused optimization experiment:

```sh
python3 scripts/test/ipc-bench.py ame --placement different_cpu \
  --payloads 0,48 --iterations 8192 --rounds 7 --runs 7 \
  --output out/ipc-results/focused-before
# Change the implementation, keeping the workload and machine fixed.
python3 scripts/test/ipc-bench.py ame --placement different_cpu \
  --payloads 0,48 --iterations 8192 --rounds 7 --runs 7 \
  --output out/ipc-results/focused-after
python3 scripts/test/ipc-bench.py compare \
  out/ipc-results/focused-before/results.json \
  out/ipc-results/focused-after/results.json
```

Use `--include-spin` for additional **different-CPU** Ame active-polling cases.
They are separately labeled and consume CPU while waiting. They are not a
blocking-IPC optimization by themselves; this suite does not measure energy or
CPU utilization. Linux adapters skip spin cases. Single-CPU Ame runs skip
unavailable different-CPU cases. Use `--cpus 1`, `2`, or `4` to exercise those
configurations; workers use at most CPUs 0 and 1.

The shorter configuration below is a correctness smoke check, not a baseline:

```sh
python3 scripts/test/ipc-bench.py ame --runs 1 --iterations 33 --warmup 5 \
  --samples 32 --rounds 3 --window 16 --include-spin
python3 -m unittest discover -s tests -p 'test_ipc_benchmark*.py' -v
```

33 operations with a window of 16 exercises the final partial streaming window.
The native integration test runs both transports under ASan/UBSan, covers every
payload and both placements, and validates all records. Integrity tests reject
missing/duplicated samples, false affinity labels, invalid clocks/counts,
configuration drift, corrupt summaries, and incompatible comparisons.

<a id="ipc_benchmark-workload-contract-v1"></a>
### Workload contract v1

| Property | Contract |
| --- | --- |
| Peers | Two processes, one worker each; no threads sharing an address space |
| RPC | One outstanding request; server validates it and echoes the same payload; client validates reply |
| Streaming | Sender delivers a window of messages; server validates all and returns one empty completion ACK; includes a partial final window |
| Payloads | 0, 8, 48, 256, 4096 bytes, selectable with `--payloads` |
| Validation | Every sequence number and deterministic payload byte; generation and validation remain inside timed work on every OS |
| Placement | `same_cpu` and `different_cpu`; exclusive worker affinity verified before timing |
| Allocation | Setup and all buffers allocated before timing; buffers reused after warmup |
| Aggregate operation | One full request/reply for RPC; one delivered logical message for streaming |
| Goodput | Payload bytes only: two payloads per RPC or one per stream message; excludes metadata and ACK bytes |
| Waiting | Blocking receive; optional separately labeled Ame spin cases |

`different_cpu` means different logical CPU IDs, not necessarily different
physical cores. Linux automatically prefers a non-sibling CPU and records the
physical-package/core/sibling topology. Override `--client-cpu` and `--server-cpu`
when required. QEMU vCPUs do not demonstrate physical-core cache behavior.

RPC latency is **round-trip** latency, never RTT divided by two. Streaming
throughput includes completion ACKs, flow control, copying, validation, and
scheduling; ACKs do not inflate the logical operation count. This is a small
fixed-load communication suite, not a saturation/concurrency scalability test.
There is no zero-copy, capability-transfer, page-transfer, or contention workload.
Published lmbench `lat_pipe`/`lat_unix` or microkernel IPC-cycle figures use
different contracts; rerun this workload on the reference OS instead of directly
comparing their published numbers with these results.

<a id="ipc_benchmark-timing-and-statistics"></a>
### Timing and statistics

The aggregate path reads a monotonic nanosecond clock immediately before and
after the batch. Warmup, affinity, setup, counter snapshots, formatting and output
are outside this interval. It reports elapsed/operations and the reciprocal rate.
Individually timed RPCs run in a separate phase with two explicit clock syscalls
per RPC. They produce raw RTT samples and nearest-rank p50/p90/p95/p99/p99.9.
Clock reads are included, never subtracted. The two phases answer different
questions; aggregate per-operation time need not equal instrumented RTT p50.

Linux explicitly uses `syscall(SYS_clock_gettime, CLOCK_MONOTONIC_RAW)`, avoiding
a vDSO-vs-syscall instrumentation mismatch with Ame's `mx_clock_ns`. This is a
contract for this experiment, not a claim about each OS's fastest public clock.
1,024 back-to-back clock deltas per process pair report measurement-floor noise.
A delta is not an exact amount to subtract from RTT. QEMU guest nanoseconds are
not physical CPU cycles. CPU frequency, timer implementation and emulation
behavior remain material when assessing results.

For each case, take the median aggregate round within each independent process
pair, then the median of those independent run medians. A seeded 2,000-resample
bootstrap gives a 95% interval over **independent run medians**, not thousands of
correlated messages. Three runs give a coarse interval; use 7–15 runs for small
changes. Retain individual run medians, per-run RTT quantiles, and pooled RTTs.
p99.9 is withheld below 10,000 pooled samples. Quantiles describe the observed
workload; the suite does not provide a confidence interval for tail quantiles.

Reports flag fewer than three runs, smoke-sized workloads, and cases with run
CV above 10%. Increase `--iterations`, `--rounds`, and `--runs` for noisy cases,
and investigate host interference. Keep these parameters identical in both
measurements. The runner does not control host services, frequency, turbo,
thermal state, IRQ affinity, NUMA placement, or cloud scheduling. Measure on a
quiet machine, run experiments sequentially, and interleave repeated before/after
collections when testing a small effect. Overlapping bootstrap intervals are a
warning about uncertainty, not a formal significance test.

<a id="ipc_benchmark-transport-and-diagnostic-details"></a>
### Transport and diagnostic details

Ame uses the production Mochi SPSC ring and doorbell library, with a 128-slot
framed ring and a 48-byte inline payload. Larger messages use copied packed
records, with a single sequence/type/length header and wrap-safe payload copies.
A 4096-byte message requires 65 cursor units and fits in one publication.
Blocking data and producer-credit waits compare retained ring cursors in the
kernel. Notification can share that entry, and eligible same-core RPCs use
bounded request/reply handoff. The optional spin adapter uses CPU relax.
`--reference-framing` retains the old 64-slot fragment-at-a-time adapter as an
explicit implementation control; it is not the production default.
It does not execute WFI while waiting in userland. Blocked workers let the kernel
use its normal idle/WFI behavior. The bootstrap parent parks and normal userland
services do not run, isolating communication and scheduler behavior.

Linux uses a connected Unix `SOCK_SEQPACKET` pair or two blocking pipes. Both
carry a 16-byte sequence/size/opcode header followed by the payload. Seqpacket
uses one send and receive per record; pipe receive reads the header and body
separately and handles short reads/writes. These are different OS mechanisms
implementing the same logical exchange. Their framing/syscall choices are
explicit so results are not mistaken for a pure kernel-entry comparison.

Raw rounds contain context switches, IPC syscalls, doorbell sleeps,
notifications, yields, remote/local wakes, IPIs, and timer preemptions where
available. Summaries normalize their median deltas per logical operation.
Ame counters cover the isolated system; Linux `getrusage(RUSAGE_SELF)` counters
cover the client only. Unsupported values are null, never zero. Snapshots sit
just outside timing, so deltas are diagnostic approximations rather than exact
trace events for the timed boundary. Ame's context-switch counter records
changes of selected thread; sleeping then resuming the same thread can report
zero, even with doorbell sleeps. Do not equate its values with Linux switch
accounting or treat zero switches as zero scheduler work.

These counters help distinguish inline wakeup overhead, fragmentation/copying,
backpressure yields, and remote wake/IPI costs. Preserve timing **and** counters
when optimizing; reducing a count alone does not establish a speedup. The
existing `benchmark-ipc.py` runner and its IPC/ring fixtures remain host
microbenchmarks for component diagnosis. They do not replace this OS workload.

<a id="ipc_benchmark-comparing-operating-systems-and-preserving-baselines"></a>
### Comparing operating systems and preserving baselines

For a cross-OS comparison, use the same physical machine/ISA with the same
online CPU count and worker/core placement, or identically configured emulated
machines using the same emulator/host. Match clocks, payloads, validation,
iteration counts, ACK window, compiler optimization, and wait policy. Record
kernel/build mode, CPU/NUMA topology, timer settings, power policy, and system
load. Ame's supplied runner uses an instrumented development kernel with
`Release` compiler settings and an `-O2` workload. The development switches retain
the benchmark target, signing setup and diagnostic counters; this is an optimized
development kernel, not the production release signing configuration. Select
`--build-type RelWithDebInfo` only to reproduce the initial pre-refactor collection.

The runner records OS/host/ISA/CPU details, online CPU count, affinity/topology,
compiler/version, QEMU configuration, cgroup quota, clock source, and hashes of
the workload, adapter, and binaries. Every executable embeds the workload-header
SHA256. The parser checks it against the current runner, catching stale builds.
Results and comparisons use versioned JSON schemas. Changing workload semantics
requires a new contract version; do not pool results from different hashes.
Changes to transport libraries or kernels are the implementation under test;
retain their source/binary identity with the result.

```sh
python3 scripts/test/ipc-bench.py linux --machine-id arm-board-01 --output out/linux-board
python3 scripts/test/ipc-bench.py compare --cross-os \
  out/ame-board/results.json out/linux-board/results.json
```

`compare` rejects different workload hashes/configurations, native-vs-emulated
runs, different ISA/CPU model/online CPU count, and different case sets. Cross-OS
comparisons also require an explicit matching `--machine-id`. This label is an
operator assertion: it cannot verify hardware or power settings. Regression
comparisons additionally check host identity, affinity/topology, transport and
recorded emulator configuration. Review metadata even when the guard passes.
The result reports percentage changes, speedups and interval overlap per case;
it does not issue an unsupported overall OS ranking.

For physical Ame hardware, build the dedicated bundle with the appropriate
board configuration and `-DAME_IPC_BENCH_SERVER_CPU=0` or `1`, and collect serial
logs from three or more independent boots per placement. `ipc_bench` is a dev
build target; the common harness hash is embedded by CMake. Physical execution
of this bundle has not been validated here. Import complete logs using:

```sh
python3 scripts/test/ipc-bench.py import captures/*.log --os Ame \
  --execution-kind native --machine-id arm-board-01 --isa aarch64 \
  --cpu-model 'exact matching CPU model label' --cpus 8 --output out/ame-board
```

Here `--cpus` describes the online CPU configuration, not just the two workers.
The import command marks metadata as operator-supplied and preserves the logs.
Use it for other OS adapters too. Port `benchmarks/ipc/linux.cc`'s small backend
interface to the other OS while retaining `workload.h`, allocation/validation,
clock and placement semantics. Missing facilities should produce explicit skips
or null diagnostics, not silently different work.

<a id="ipc_floor_results"></a>
## Cortex-A53 IPC results — verified checkpoint

<a id="ipc_floor_results-cortex-a53-ipc-results-verified-checkpoint"></a>

These are matched QEMU TCG observations, not physical P20 Lite latency or proof of the hardware floor. Original means the supplied IPC-refactor ZIP, rebuilt with the same Release compiler settings and workload. The final implementation includes a bounded 64-check poll only for cross-core blocking waits; same-core waits poll zero times. Energy and CPU utilization are not measured.

<a id="ipc_floor_results-configuration"></a>
### Configuration

- Cortex-A53, two vCPUs, 512 MiB, EL2, QEMU QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1).
- Clang 19.1.7; Release kernel, unchanged `-O2` portable workload.
- Five independent boots per placement; 512 warmup operations, five rounds of 2,048 operations, 4,096 separate RTT samples, stream window 32.
- All 30 cases completed, including separately labeled unlimited polling cases. Payload and sequence validation stayed inside the timed workload.
- Builds/tests did not run concurrently with accepted timed measurements. Configuration comparison found no compiler/build-mode differences.

RPC figures below are microseconds per complete round trip. Streaming figures are microseconds per logical delivered message, including completion ACK overhead. They are medians of independent run medians, not RTT divided by two. Clock instrumentation is not subtracted.

<a id="ipc_floor_results-full-matrix"></a>
### Full matrix

| Case | Original µs | Checkpoint µs | Speedup | Aggregate 95% intervals overlap |
| --- | ---: | ---: | ---: | :---: |
| rpc/different_cpu/blocking/0 | 92.293 | 4.119 | 22.41× | no |
| rpc/different_cpu/blocking/256 | 94.039 | 82.562 | 1.14× | no |
| rpc/different_cpu/blocking/4096 | 350.074 | 163.169 | 2.15× | yes |
| rpc/different_cpu/blocking/48 | 55.561 | 5.833 | 9.53× | yes |
| rpc/different_cpu/blocking/8 | 55.151 | 4.110 | 13.42× | no |
| rpc/different_cpu/spin/0 | 1.705 | 1.526 | 1.12× | yes |
| rpc/different_cpu/spin/256 | 11.806 | 6.772 | 1.74× | no |
| rpc/different_cpu/spin/4096 | 121.077 | 80.321 | 1.51× | yes |
| rpc/different_cpu/spin/48 | 2.872 | 2.727 | 1.05× | yes |
| rpc/different_cpu/spin/8 | 1.793 | 1.779 | 1.01× | yes |
| rpc/same_cpu/blocking/0 | 138.238 | 60.216 | 2.30× | no |
| rpc/same_cpu/blocking/256 | 179.508 | 66.627 | 2.69× | no |
| rpc/same_cpu/blocking/4096 | 1020.867 | 124.955 | 8.17× | no |
| rpc/same_cpu/blocking/48 | 158.044 | 61.573 | 2.57× | no |
| rpc/same_cpu/blocking/8 | 159.508 | 60.774 | 2.62× | no |
| stream/different_cpu/blocking/0 | 1.352 | 1.092 | 1.24× | yes |
| stream/different_cpu/blocking/256 | 4.799 | 5.071 | 0.95× | yes |
| stream/different_cpu/blocking/4096 | 67.669 | 38.254 | 1.77× | yes |
| stream/different_cpu/blocking/48 | 1.819 | 1.761 | 1.03× | yes |
| stream/different_cpu/blocking/8 | 1.478 | 1.215 | 1.22× | yes |
| stream/different_cpu/spin/0 | 0.393 | 0.480 | 0.82× | yes |
| stream/different_cpu/spin/256 | 4.091 | 1.940 | 2.11× | yes |
| stream/different_cpu/spin/4096 | 42.900 | 29.871 | 1.44× | yes |
| stream/different_cpu/spin/48 | 1.101 | 0.786 | 1.40× | yes |
| stream/different_cpu/spin/8 | 0.598 | 0.604 | 0.99× | yes |
| stream/same_cpu/blocking/0 | 10.464 | 2.310 | 4.53× | no |
| stream/same_cpu/blocking/256 | 35.418 | 8.369 | 4.23× | no |
| stream/same_cpu/blocking/4096 | 515.318 | 121.189 | 4.25× | no |
| stream/same_cpu/blocking/48 | 12.531 | 3.092 | 4.05× | no |
| stream/same_cpu/blocking/8 | 12.422 | 2.675 | 4.64× | no |

<a id="ipc_floor_results-remaining-slower-estimates"></a>
### Remaining slower estimates

Three median estimates are slower. Their coarse five-run uncertainty intervals overlap; this does not prove equivalence or a regression. They are being investigated separately rather than omitted:

- `stream/different_cpu/blocking/256`: +5.65% (4.7993 → 5.0706 µs).
- `stream/different_cpu/spin/0`: +22.21% (0.3930 → 0.4803 µs).
- `stream/different_cpu/spin/8`: +1.06% (0.5977 → 0.6041 µs).

Several cases exceed 10% run CV; cross-core results are particularly noisy. Pooled RTT samples are correlated observations, not independent repetitions. The raw records retain independent-run distributions, p99/p99.9, counters and all warnings.

<a id="ipc_floor_results-structural-results"></a>
### Structural results

- Same-core 4 KiB RPC `context_switches` per operation: 12.5234 → 2.0000.
- Same-core 4 KiB RPC `ipc_syscalls` per operation: 19.6172 → 2.0000.
- Same-core 4 KiB RPC `yields` per operation: 4.2290 → 0.0000.

Normal address-space activation contains no TLBI. Production ASID activation/reuse passed on A53 at EL1 and EL2. Packed 4 KiB records use 65 cursor units instead of 86 fragments and fit in one publication. Inline frame handlers are separated from cold packed/fragmented code. See [implementation details](ipc.md#ipc_floor).

<a id="ipc_floor_results-correctness"></a>
### Correctness

- Full suite: 279 tests, 39 expected skips, one initial error caused by missing shell execute bits in the supplied ZIP. Shell execute permissions were restored; the entire affected 25-test Huawei/guest-timer group then passed.
- New end-to-end A53 integrity test passed all 30 cases, partial windows, both placements, polling/blocking and synchronous shutdown.
- Final framing, real blocking data/credit race, end-to-end IPC and Mochi-ring group: six tests passed. IPC rights/lifetimes and 2,000 registration races passed; legacy/current benchmark fixtures both compile.
- Eight-core A53 normal boot, EL1/EL2 SIMD matrix, 100-restart lifecycle and the Release Huawei native build passed.
- The exact measured kernel subsequently passed four-core lifecycle and SIMD workloads without its SHA256 changing.

Host scheduler probes used Clang 19 with CX16 enabled on x86-64 for native 128-bit budget atomics. Cross-builds used the ordinary ARMv8.0 toolchain. Optional skipped tests include missing external emulation/oracle dependencies.

<a id="ipc_floor_results-artifacts-and-provenance"></a>
### Artifacts and provenance

- [Original matched A53 data](../benchmarks/ipc/baselines/2026-10-18/ame-original-a53.json)
- [Checkpoint A53 data](../benchmarks/ipc/baselines/2026-10-18/ame-floor-a53.json)
- [Guarded comparison](../benchmarks/ipc/baselines/2026-10-18/ame-floor-a53-comparison.json)
- The delivered ZIP includes accepted serial/build logs and exact benchmark binaries under `performance-artifacts/`.
- Failed shutdown captures and diagnostic intermediate candidates are excluded from the accepted collection.
- Measured kernel SHA256: `6e7aaa2f9f318aa36952543571cd8fe0df3be791df128d831c5d07496ad191e0`.

The adapter, ring library, framing library and measured kernel hashes were checked against the delivered source/binary files. The older saved A57 collections are historical context, not the matched baseline used here.

<a id="ipc_baseline"></a>
## IPC baseline — 2026-10-18

<a id="ipc_baseline-ipc-baseline-2026-10-18"></a>

These are reproducible starting observations, not a cross-OS ranking. Ame ran in AArch64 QEMU TCG; Linux ran directly on the x86-64 host. The comparison guard rejects that pairing. Physical AArch64 measurements remain necessary for an OS comparison.

See [the benchmark guide](performance.md#ipc_benchmark) for commands, workload semantics, timing, limitations and comparison rules. Full raw per-round timings, RTT samples, counters and provenance are retained in the linked JSON files.

All three collections use the same compiled workload hash and default parameters: 0/8/48/256/4096-byte payloads, blocking waits, 256 warmup operations, 5 × 1,024 aggregate operations, 4,096 separate RTT samples per RPC case, stream window 32, and 3 independent process pairs per case. Ame uses six boots to obtain three pairs for each placement. Tests and other benchmarks were not run concurrently with these baseline collections.

The tables show the median of independent run medians. p99 is the separately instrumented pooled RTT, not the p99 of aggregate batches. Stream throughput counts logical messages and their completion ACK cost; MiB/s counts only delivered payload bytes. Zero-byte messages have zero payload goodput.

<a id="ipc_baseline-ame-qemu-tcg"></a>
### Ame / QEMU TCG

[Raw results](../benchmarks/ipc/baselines/2026-10-18/ame-qemu.json). aarch64; cortex-a57; 2 online logical CPUs. Compiler: Debian clang version 19.1.7 (3+b1).

QEMU cortex-a57, 2 vCPUs, 512 MiB, EL2 entry, isolated signed bundle, development kernel RelWithDebInfo, benchmark `-O2`. Guest time is not physical CPU cycles.

| Placement | Payload B | RPC µs/round trip | RTT p99 µs | Stream ns/message | Stream MiB/s | RPC run CV |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| same_cpu | 0 | 96.68 | 205.66 | 6779.8 | 0.00 | 4.1% |
| same_cpu | 8 | 82.72 | 226.51 | 6787.9 | 1.12 | 3.4% |
| same_cpu | 48 | 90.69 | 181.52 | 5380.3 | 8.51 | 7.5% |
| same_cpu | 256 | 160.73 | 708.24 | 29391.4 | 8.31 | 8.1% |
| same_cpu | 4096 | 1539.29 | 3854.14 | 456129.2 | 8.56 | 1.7% |
| different_cpu | 0 | 19.57 | 143.95 | 783.8 | 0.00 | 19.7% |
| different_cpu | 8 | 26.53 | 177.14 | 774.2 | 9.86 | 30.6% |
| different_cpu | 48 | 29.67 | 137.73 | 1101.3 | 41.57 | 15.2% |
| different_cpu | 256 | 52.89 | 178.91 | 3299.3 | 74.00 | 14.0% |
| different_cpu | 4096 | 119.20 | 318.61 | 40762.1 | 95.83 | 23.8% |

Back-to-back clock delta p50: 1376 ns (not subtracted).

Measurement note: Some run medians vary by more than 10%; inspect interference before attributing changes.

<a id="ipc_baseline-linux-unix-sock_seqpacket"></a>
### Linux / Unix SOCK_SEQPACKET

[Raw results](../benchmarks/ipc/baselines/2026-10-18/linux-unix.json). x86_64; AMD EPYC 9V74 80-Core Processor; 3 online logical CPUs. Compiler: Debian clang version 19.1.7 (3+b1).

Workers pinned to logical CPUs 0 and 1 for different-CPU cases. Host cgroup quota: `200000 100000`. Host background services and cloud scheduling are uncontrolled.

| Placement | Payload B | RPC µs/round trip | RTT p99 µs | Stream ns/message | Stream MiB/s | RPC run CV |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| same_cpu | 0 | 3.11 | 6.60 | 566.5 | 0.00 | 5.8% |
| same_cpu | 8 | 3.05 | 5.12 | 582.8 | 13.09 | 4.4% |
| same_cpu | 48 | 3.12 | 15.40 | 631.3 | 72.51 | 4.6% |
| same_cpu | 256 | 3.64 | 6.53 | 761.8 | 320.49 | 8.6% |
| same_cpu | 4096 | 9.87 | 38.08 | 4526.0 | 863.07 | 5.1% |
| different_cpu | 0 | 16.28 | 74.79 | 921.9 | 0.00 | 3.3% |
| different_cpu | 8 | 15.95 | 58.98 | 852.8 | 8.95 | 10.9% |
| different_cpu | 48 | 16.37 | 52.04 | 865.2 | 52.91 | 3.5% |
| different_cpu | 256 | 17.58 | 66.51 | 1140.1 | 214.14 | 6.4% |
| different_cpu | 4096 | 27.12 | 69.94 | 3747.9 | 1042.26 | 16.9% |

Back-to-back clock delta p50: 90 ns (not subtracted).

Measurement note: Some run medians vary by more than 10%; inspect interference before attributing changes.

<a id="ipc_baseline-linux-pipe-pair"></a>
### Linux / pipe pair

[Raw results](../benchmarks/ipc/baselines/2026-10-18/linux-pipe.json). x86_64; AMD EPYC 9V74 80-Core Processor; 3 online logical CPUs. Compiler: Debian clang version 19.1.7 (3+b1).

Workers pinned to logical CPUs 0 and 1 for different-CPU cases. Host cgroup quota: `200000 100000`. Host background services and cloud scheduling are uncontrolled.

| Placement | Payload B | RPC µs/round trip | RTT p99 µs | Stream ns/message | Stream MiB/s | RPC run CV |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| same_cpu | 0 | 2.23 | 3.84 | 348.3 | 0.00 | 13.6% |
| same_cpu | 8 | 2.53 | 4.61 | 444.8 | 17.15 | 3.3% |
| same_cpu | 48 | 2.96 | 6.21 | 603.2 | 75.89 | 9.8% |
| same_cpu | 256 | 2.93 | 6.19 | 694.1 | 351.72 | 6.4% |
| same_cpu | 4096 | 9.42 | 55.75 | 5249.7 | 744.08 | 4.2% |
| different_cpu | 0 | 17.67 | 53.66 | 726.2 | 0.00 | 2.6% |
| different_cpu | 8 | 18.28 | 41.39 | 899.3 | 8.48 | 0.9% |
| different_cpu | 48 | 18.35 | 80.76 | 934.8 | 48.97 | 0.8% |
| different_cpu | 256 | 18.77 | 62.67 | 1629.6 | 149.82 | 9.2% |
| different_cpu | 4096 | 26.64 | 78.71 | 4586.4 | 851.70 | 15.3% |

Back-to-back clock delta p50: 90 ns (not subtracted).

Measurement note: Some run medians vary by more than 10%; inspect interference before attributing changes.

<a id="ipc_baseline-what-to-investigate-next"></a>
### What to investigate next

The Ame same-CPU 4 KiB RPC takes about 1.54 ms and shows median diagnostic deltas of 34.52 selected-thread switches, 5.53 yields and 18.09 doorbell sleeps per operation. A logical message needs 86 ring slots, while the ring holds 64. Fragmentation and producer backpressure are a useful first target: inspect wake coalescing, producer-space notification and large-message transfer strategies. These counters identify work to investigate; they do not by themselves prove which change will help.

Ame inline same-CPU 48-byte RPC is about 90.69 µs with two selected-thread switches per operation, giving a separate wakeup/scheduler path to track. Different-CPU blocking cases have zero selected-thread changes when the same worker resumes after idle; the nonzero sleep/wake counters show why zero is not equivalent to no scheduler cost.

Several cases have more than 10% run CV; same-CPU 48-byte streaming reaches roughly 47% in this exploratory Ame collection. Use longer batches, more independent repetitions and a quieter or physical machine before claiming small gains. Three repetitions are a starting baseline, not strong evidence for small improvements.

<a id="ipc_baseline-validation"></a>
### Validation

- 12 benchmark integrity/integration tests passed; both Linux transports ran under ASan/UBSan.
- Full default 20-case matrices completed with three observations per case for Ame and each Linux adapter.
- One-vCPU Ame smoke: four selected cases passed, eight unavailable cases skipped.
- Different-vCPU Ame smoke: 20 blocking/spin cases passed with all payload sizes, partial final windows and ring backpressure.
- Ame baseline self-comparison returned exactly 1.0× for all 20 cases. Native Linux vs Ame TCG was rejected.
- The recorded Ame kernel and both placement bundles rebuilt to their recorded SHA256 values.
- The normal `anko mochi_system` build and two-vCPU normal-service boot passed; boot-runner tests passed.

The short smoke runs establish correctness only. Their timings are not used in these baseline tables.

<a id="runtime_performance"></a>
## Anne runtime CPU measurements

<a id="runtime_performance-anne-runtime-cpu-measurements"></a>

Measured on the handset on 2026-10-15 with the Mac connected over native CDC,
using `anne-runtime-profile-v1`. Three five-second windows covered 15.008 seconds
with eight CPUs online and no requested application workload. No process was
excluded because of exit, restart, or counter regression.

Percentages below describe **one CPU's time**, calculated from raw nanosecond
deltas across all three windows. Eight fully occupied CPUs would total 800%.

| Process | Average CPU | Observation |
| --- | ---: | --- |
| `nameservice` | 23.393% | Scans client sessions, then yields when no request arrived. |
| `init` | 23.390% | Checks control requests and supervision, then yields when idle. |
| `io.framebuffer` | 0.792% | Bursty: 0.0014%, 0.0014%, then 2.374%; cause not isolated. |
| `drv.i2c.dw` | 0.0783% | Largest consistent driver cost in these quiet windows. |
| `drv.typec.fusb301` | 0.0179% | Type-C service. |
| `io.usb.role` | 0.00954% | Automatic USB role policy. |
| `drv.power.hi6523` | 0.00524% | Charger and source supervision. |
| `drv.usb.dwc2` | 0.00146% | USB controller with the Mac connected. |

Attributed process CPU totals 47.690% of one CPU, or 5.961% of the eight-CPU
capacity. `init` and `nameservice` account for 98.096% of that attributed work.
Their idle paths call `sys_yield()` and remain runnable: yielding is not waiting
for an event. Replacing these scans with correctly notified waits is the first
runtime optimization target. It must preserve supervision, session generation
changes, request delivery, and lost-wakeup protection. No speedup from that change
has been measured yet.

<a id="runtime_performance-collection-and-limits"></a>
### Collection and limits

Run `diag runtime 5000` in msh. The command reads existing process runtime
counters before and after a quiet interval, then prints results. It adds no
accounting writes to the scheduler hot path. Report output happens after the
second snapshot; earlier terminal output and asynchronous rendering can still
affect later windows, so the framebuffer burst is not established as a recurring
idle cost.

The kernel copies at most eight 64-byte rows under the process lifecycle lock.
The shell retains two fixed snapshots outside its small stack, computes deltas
in separate numeric columns, and sorts a compact index array. PID generations
prevent attributing a replacement process to an earlier instance. A full snapshot
uses several bounded reads; the reported skew was tens of microseconds here.

These are scheduler-accounted process times, including kernel work while that
process is current. Idle time and interrupt work with no current process are not
represented. This is a service ranking, not a complete CPU or function profile.
Keyboard, graphics, IPC throughput, and other active workloads require separate
measurements.

Private captures and raw deltas are in
`out/anne-device-private/runtime-profile-v1/runtime-{1,2,3}.{txt,json}` and
`runtime-summary.json`. The retained boot log confirmed the profiling image,
all eight CPUs, automatic USB switching, and msh readiness at 0.23 seconds on
this boot. That single boot is not a repeatability claim.

Validation: bounded kernel snapshot probe and shell reporting probe passed;
native Release build passed; an eight-CPU synthetic Anne Release boot passed;
all three handset samples and the retained boot-log check completed.

<a id="boot_performance"></a>
## Release boot performance

<a id="boot_performance-release-boot-performance"></a>

ANE-LX1 hardware measurement, 2026-10-15. Latest measured image run tag:
`anne-usb-auto-v5`, SHA-256:
`1a4fa94b211699c25809e27f542927f85b6b1e892e95c32d04b2838353594826`.
The image is installed on the phone. Signed package and verified stock restore:
`out/anne-usb-auto-package-v5`. One measured boot reached msh in 0.30 s
(kernel to services ready: 308.521 ms). The user confirmed stable keyboard
typing and normal Mac reconnection without the Mac charging from the phone. The v8 IPC image initially measured 0.29 s; a later user-reported
0.36 s v8 boot could not be captured after the handset entered fastboot.

| Image | msh boot banner | DT probe checkpoints |
| --- | ---: | ---: |
| Previous release, `anne-cdc-hw-v1` | 1.46 s | 736.67 ms |
| Early instruction cache, `anne-boot-speed-v1` | 0.89 s | 345.75 ms |
| Compact DT transform, `anne-boot-speed-v2`, boot 1 | 0.81 s | 257.419 ms |
| Same image, boot 2 | 0.81 s | 257.435 ms |
| Same image, boot 3 | 0.81 s | 257.418 ms |
| Single-pass cached DT transform, `anne-boot-speed-v3`, boot 1 | 0.53 s | 4.555 ms |
| Same image, boot 2 | 0.53 s | 4.546 ms |
| Same image, boot 3 | 0.53 s | 4.548 ms |

The probe average fell from 257.424 ms to 4.550 ms: 56.58 times faster,
or a 98.23% reduction. These are actual phone checkpoint intervals, including
the temporary mapping's setup and teardown, with the same Release configuration.

The banner uses the unchanged `DBG_BOOT_CS` counter, from `kernel_main` to msh
startup, at centisecond precision. It excludes firmware, decompression, and BSS
clearing before `kernel_main`. Checkpoint intervals use the phone's 1.92 MHz
counter. Kernel-main to all-services-ready averaged 538.365 ms for v3 and
374.266 ms for v4. These measurements establish an improvement, not a minimum possible
boot time.

The next optimization, v4, batches permanent identity-map construction:

| Image | Page-table construction | msh boot banner |
| --- | ---: | ---: |
| v3, boot 1 | 92.833 ms | 0.53 s |
| v3, boot 2 | 92.823 ms | 0.53 s |
| v3, boot 3 | 92.807 ms | 0.53 s |
| v4, boot 1 | 1.037 ms | 0.37 s |
| v4, boot 2 | 1.037 ms | 0.37 s |
| v4, boot 3 | 1.036 ms | 0.37 s |

The page-table interval averages 92.821 ms before and 1.036667 ms after:
89.54 times faster, a 98.88% reduction. Service startup also fell from an
average 189.294 ms to 120.135 ms because each process creates an address space.
The hardware-discovery probe remains about 4.56 ms.

<a id="boot_performance-changes-and-ownership"></a>
### Changes and ownership

The phone entry path invalidates the instruction cache and enables it before
BSS clearing and DT probing. During hardware discovery, a temporary identity
mapping enables data caching for the kernel and immutable DTB. It is removed
before allocator initialization; the existing page-table setup still performs
the permanent MMU transition. The handoff checks still reject MMU/data-cache-on
entry. The [AArch64 boot protocol](https://docs.kernel.org/arch/arm64/booting.html)
allows instruction caching with the MMU off and requires no stale kernel-image
instruction entries.

Hardware discovery is a bounded DTB-to-`HwInfo` transformation:

- A contiguous array of 64-byte aligned records holds active node metadata.
  It has the same maximum depth as the structural walker, with no heap allocation.
- `reg` and `ranges` use bounded read-only views into the DTB; views never escape
  into the published hardware description.
- One path buffer and per-depth lengths replace copied full paths per node.
  An overflowing path cannot match a truncated console name.
- Each compatibility list is traversed once, against a fixed table that preserves
  selection priority.
- Hardware, `/chosen`, aliases, and UART candidates are collected in one walk.
  Fixed-size candidate/path storage handles the common case; overflow performs
  exact streaming selection rather than truncating or silently losing a UART.
- A walk-local cache validates and classifies property names by their interned
  strings-block offsets. Full tags make hash collisions harmless. Node names
  are checked and measured in one scan. No cache survives a different input blob.
- Candidate hardware data stays local until the entire walk succeeds, including
  the phone's RAM limit. Malformed input leaves the caller's output unchanged.
- Disabled ancestors remain disabled, and a memory-controller node cannot be
  mistaken for a root memory bank.

DTB input is immutable and discovery runs on the sole boot CPU before SMP.
No cacheable alias or new concurrent writer is introduced. The existing SMP
startup cleans the kernel, including `g_hw`, to PoC and executes `DSB SY` before
CPU_ON; its release/acquire handshake publishes startup state to secondary CPUs.
CPU/PSCI and reservation walkers also use bounded local records over the same
read-only input.

The temporary tables use complete 2 MiB blocks plus 4 KiB boundary pages. Only
the exact kernel/DTB page ranges become Normal WBWA, inner-shareable memory;
the DTB is read-only and execute-never, and peripherals remain Device-nGnRnE.
Invalid, overlapping, or unsupported extents retain the uncached boot path.
Table walks and software accesses have matching cache/shareability attributes.
Before enabling caching, stale lines are invalidated by VA. A stackless assembly
routine cleans dirty kernel/stack/output lines to PoC and drains writes with
`DSB SY` before restoring the original MMU/cache registers. It then invalidates
the clean DTB through physical access, without creating another VA alias.

An initial hardware boot exposed a teardown permission bug: `DC IVAC` requires
write permission even at EL1, so it must not target the read-only DTB while
translation is active. Moving this invalidation after the MMU-off transition
fixed the phone boot while retaining read-only cached parsing. QEMU does not
enforce that permission check; the CPU test also inspects the linked restore
routine's ordering. See the [Arm architectural correction, D23577](https://documentation-service.arm.com/static/67ab274f6dbc975ccea916d7).

<a id="boot_performance-permanent-identity-map-construction"></a>
### Permanent identity-map construction

The DOP boundary is a compact array of 24-byte `{begin, end, attributes}` runs.
Cold code resolves RAM/MMIO/framebuffer/noncached policy at attribute boundaries,
sorts the resulting runs, rejects overlaps, and merges adjacent equivalent runs.
The plan is cached across process creation and invalidated when the registered
noncached ranges change. The VM lock serializes plan creation and invalidation.

The emitter consumes runs in slices bounded by one L3 table. It walks upper
levels once per slice and streams contiguous 64-bit PTE stores, rather than
repeating the full lookup and attribute policy for each 4 KiB page. A complete
new leaf needs no preliminary zeroing; partial leaves clear only their uncovered
edges. Other tables are cleared with 64-bit stores instead of byte stores.
Existing leaves reject a duplicate batch before writing it.

This removes repeated work but retains 4 KiB leaves and private identity tables
per address space. A fully present 512 MiB bootstrap RAM mapping still needs
131,072 PTEs, 256 L3 tables, and 1 MiB of leaf-table storage. Future block mapping
needs a safe split path for noncached retagging, including page-table access while
breaking a block. Shared kernel TTBR1 mappings, ASIDs with SMP invalidation, and
restricting EL1 execution to kernel text remain separate work. The current kernel
identity map is supplied by TTBR0; the source comments now reflect that fact.

<a id="boot_performance-reservation-batch-v5-first-phone-boot"></a>
### Reservation batch: v5 first phone boot

v5 replaces the second full uncached DT traversal with a bounded array of
16-byte `{base, size}` records, kept separate from hot `HwInfo` data. The existing
cached hardware-discovery walk also collects `/reserved-memory` tuples; the short
header reservation list is decoded in the same cache scope. After `phys_init()`,
allocator setup streams the records directly. No extra cache transition, heap
allocation, node graph, or per-node property copy is introduced. The existing
temporary-cache teardown publishes the batch to PoC before MMU-off consumption.

A source pointer marks a batch complete only after both sections validate and
console selection succeeds. Capacity overflow, malformed input, or a different
DTB pointer selects the original streaming consumer. The collector preserves the
existing reservation policy: header entries first, two address/two size cells,
disabled and nested reserved nodes, the 16-level consumer bound, partial trailing
tuple handling, zero filtering, duplicates, and source order. Console replay
cannot collect a reservation twice. The phone's 28 ranges match an independent
decode of the original consumer's addresses, sizes, and ordering exactly.

The signed Release package is `out/anne-boot-speed-package-v5`, run tag
`anne-boot-speed-v5`, native image SHA-256
`5bd60f0d2385760246f64a1f87ca29a6609308969b28df0cdb38056b13cfc1c3`.
Its raw kernel hash matches the final Release build. Following the user's flash
request, v5 was installed and verified at the USB shell. The first phone boot
measured reservations at **0.469 ms**, versus the v4 three-boot average of
75.105 ms (160.14 times faster for this first sample). Hardware discovery was
4.718 ms and page-table construction 1.041 ms. msh reported **0.29 s**, and
kernel-main to all-services-ready was 299.170 ms. This is one boot, not a v5
average. All 28 reservation records were applied; all eight CPUs reported MMU,
instruction cache, and data cache enabled, without a fault. The sink-only marker
and complete diagnostic report were present. The phone was left running v5.

For v5, 37 focused tests passed. The production transform runs with ASan/UBSan
over read-only input, including full register arrays, disabled ancestors, header
entries, malformed structures/headers, exact capacity, overflow, and console
replay. Full Debug QEMU boots passed at EL1, EL2, and eight-core EL1. Two additional
full boots used injected reservation DTBs: a complete two-record batch and a
129-record overflow, both reaching all-services-ready with the full reservation
count. Compiler stack-usage reports keep discovery plus the walker/callbacks below
the existing 16 KiB boot stack; the reservation array has static storage.

<a id="boot_performance-scheduler-dop-v6-ready-phone-measurement-pending"></a>
### Scheduler DOP: v6 ready, phone measurement pending

The hot principal record is now exactly **64 bytes, aligned to 64 bytes**, down
from the original 3,264-byte record containing six thread heaps. Selection clocks,
generation, bounded queue counts, membership, and heap indexes fit one line.
The active service/thread clocks retain their full 64-bit precision. Unused
per-band clock/watermark bookkeeping and a duplicated local runtime total were
removed; authoritative thread, process, and global principal runtime remain.
Fixed-capacity heaps occupy a parallel array. Both arrays are CPU-owned and
indexed by principal ID; generation reuse resets the hot state. Compile-time assertions
enforce the layout, and the linked Release scheduler storage is 64-byte aligned.

Heap size/band fields sit near their root pointers. Dispatch combines principal
metadata reads under one lock, skips empty bands, and avoids clock conversion for
unbudgeted eligibility. Accounting reuses its locked weight snapshot. The switch
path retains the running thread's cached lifetime record instead of searching
the global object registry; a same-thread syscall return also avoids copying
the saved trap frame. Deferred wakes still publish only after a full frame save,
and temporary syscall address-space changes still restore the caller's mapping.

The total station shrank from 212,928 to 207,808 bytes, saving 40 KiB across eight
CPUs. Queue capacity remains 64 threads per principal/band. Locks still protect
global budgets and lifetime references; cross-CPU wakes and fairness policy retain
their existing rules. See [the scheduler notes](scheduler.md).

The final code passed 39 focused tests, including ASan/UBSan scheduler and
capability probes, 30,000 heap operations, 10,000 cross-band dispatches, exact
queue capacity/overflow, weighted fairness, budget rollover, principal reuse,
remote frame publication, and retirement. Seven QEMU checks passed: full Debug
boots at EL1, EL2, and eight-core EL1; eight-core EL1/EL2 workloads checking pinned
workers, preemption, budgets, and diagnostics; and four-core process lifecycle
and supervision regressions. Synthetic Anne builds use the same private EL3
compile fixture described above; no EL3 diagnostic command is exercised.

The signed Release image is `out/anne-boot-speed-package-v6`, run tag
`anne-boot-speed-v6`, native image SHA-256
`efdbd4237cedcc553f1a24d4a312e4d309e14687b3a01db83d583f282d2f9a03`.
It matches the final raw kernel. **v6 has not been flashed**, at the user's request
to leave it ready for later; phone boot timing remains pending. Its rebuilt bundle
is 917,504 bytes versus v5's 913,408 bytes, including other workspace changes.
The host benchmark isolates the scheduler sources instead.

Reproduce the warm host dispatch stress comparison with
`scripts/test/benchmark-scheduler.py --baseline-dir out/sched-opt/baseline`.
It uses 200,000 dispatch rounds with 1/8/32 runnable threads and 1,024 unrelated
live object records, taking medians over five runs. Hardware boundaries are
substituted; these are scheduler stress costs, not phone boot predictions.
Final timings are in `out/sched-opt/host-performance.json`; the isolated 128-byte
versus 64-byte layout comparison is in `out/sched-opt/cacheline-comparison.json`.

| Runnable threads | Original dispatch round | Final dispatch round | Speedup |
| --- | ---: | ---: | ---: |
| 1 (reselect itself) | 886.2 ns | 79.6 ns | 11.13x |
| 8 | 910.0 ns | 121.1 ns | 7.51x |
| 32 | 909.5 ns | 127.8 ns | 7.12x |

The isolated one-line/two-line comparison is approximately even: 79.0 versus
78.2 ns with one thread, 121.6 versus 123.2 ns with eight, and 125.3 versus
127.4 ns with 32. These small differences do not establish a latency gain from
the layout alone; its demonstrated benefits are the 64-byte record and lower
static memory use. The larger stress gain also includes the cached lifetime
lookup, combined metadata reads, and reduced return/dispatch work above.

<a id="boot_performance-scheduler-lock-free-dispatch-v7-ready-phone-measurement-pending"></a>
### Scheduler lock-free dispatch: v7 ready, phone measurement pending

Normal preemption/yield now accounts, requeues, and activates CPU-owned threads
without taking lifecycle locks. Remote wake/bind paths reject running/queued
threads before inspecting mutable dispatch fields. Blocking, queue-overflow
parking, and deferred remote wakes keep their saved-frame/lifecycle handshake.
Allocation, teardown, stealing, and object creation/final unlink still use locks;
this is a lock-free ordinary dispatch path, not an entirely lock-free kernel.

The DOP change moves principal runtime into CPU-owned, cache-aligned rows indexed
by principal ID. Diagnostics aggregate on demand. Principal generation/weight
publish atomically in one 64-bit word. Shared budgets use aligned 128-bit CAS
windows containing full-width used time and period start, so rollover cannot
erase a concurrent charge. Cached object retains and nonfinal releases are atomic;
target lookup cannot resurrect a record awaiting final unlink. The hot principal
record remains exactly 64 bytes and each station remains 207,808 bytes. The
changed global principal/context/runtime/guard arrays use 10 KiB less BSS than v6.

The scheduling-context syscall keeps its existing 80-byte wire record. Kernel
budget alignment is private, and the kernel explicitly translates policy fields.
Compile-time checks pin the wire size/offsets and require native lock-free
128-bit atomics. The linked AArch64 Release image uses LDXP/STXP sequences and
has no unresolved atomic-library symbols; it needs no LSE extension.

Seven-run medians against the captured v6 sources are in
`out/sched-lockfree/host-performance.json`:

| Host case | v6 | v7 | Ratio |
| --- | ---: | ---: | ---: |
| Dispatch, 1 runnable thread | 80.4 ns | 73.9 ns | 1.09x |
| Dispatch, 8 runnable threads | 119.6 ns | 114.0 ns | 1.05x |
| Dispatch, 32 runnable threads | 131.0 ns | 121.0 ns | 1.08x |
| Shared unbudgeted principal, 4 CPUs | 87.3 ns/charge | 5.7 ns/charge | 15.32x |
| Shared principal + context budgets, 4 CPUs | 111.3 ns/charge | 109.0 ns/charge | 1.02x |

The concurrent cases report wall time divided by all charges: throughput, not
one thread's latency. Each CPU makes 100,000 charges. Budgeted results vary enough
that the 2% median difference does not establish an improvement; those shared
atomic windows still contend. Dispatch improves modestly, while unbudgeted
accounting benefits substantially from removing the shared write owner.
Hardware clock/IRQ/address-space boundaries are substituted and process-owner
aggregation is absent in this host fixture. These are not phone boot timings.
Reproduce with `scripts/test/benchmark-scheduler.py --baseline-dir
out/sched-lockfree/baseline --runs 7`.

The final source passed 39 focused tests, including ASan/UBSan scheduler and
capability probes, plus a ThreadSanitizer scheduler run. New checks cover eight
concurrent budget chargers, full-width rollover and saturation, CPU runtime
aggregation/reuse, dispatch with lifecycle guards held, simultaneous remote
wake/bind attempts, and deferred publication when a full queue parks its runner.
Seven final QEMU checks passed: three full boots, two eight-core EL1/EL2 pinned
workloads, and four-core lifecycle/supervision regressions. Synthetic Anne builds
retain the existing private EL3 compile fixture; no EL3 command is exercised.

The signed Release package is `out/anne-boot-speed-package-v7`, run tag
`anne-boot-speed-v7`, native image SHA-256
`1c62fc1b135e4eade9a163da8c30739a596f666c233d96b9180fda5aed66eb81`.
Its raw kernel matches the packaged build. **v7 has not been flashed**; v6 remains
available too, following the request to leave the handset/image for later.
Results and source hashes are in `out/sched-lockfree/validation.json`, with
package state in `out/anne-device-private/boot-speed-v7/state.json`.

<a id="boot_performance-ipc-dop-rewrite-v8-flashed-and-boot-verified"></a>
### IPC DOP rewrite: v8 flashed and boot verified

IPC now keeps one 64-byte metrics row per CPU, a compact occupancy index for
capability allocation/teardown, 32-byte retained capability snapshots, and
CPU-owned timer tables with separate deadline/link columns. Doorbells remain
exactly 64 bytes with cached lifetime identity; ring objects cache identity in
32 bytes. Normal caller resolution uses the executing process directly, and
doorbell wake metadata/routing/publication uses one thread lifecycle guard.
Cold object lookup uses 256 buckets with unchanged 32-byte lifetime records.
Byte-run producers publish up to 16 slots together and console readers drain
eight-slot batches. The shared page/handle ABI and saved-frame handshake remain.

Seven-run host medians versus v7 show: small-registry notify 17.14 -> 14.07 ns;
empty timer poll 21.49 -> 2.49 ns; empty capability clear 2,035.15 -> 16.12 ns;
two-thread ring throughput 37.60 -> 9.13 ns/slot. Singleton ring latency is
approximately unchanged. Independent four-CPU notifications improve from
33.82 to 1.49 ns/event in aggregate throughput. Stress cases with 2,048 unrelated
objects also improve substantially from removing linear registry searches.
These are substituted-hardware host workloads, not phone timings. Full workload
definitions and caveats are in [the IPC notes](ipc.md#ipc).

The final source passed 47 focused host tests, IPC/ring ThreadSanitizer probes,
and seven QEMU checks including both eight-core workloads and process
lifecycle/supervision. Results, stack reports, snapshots, and source hashes live
in `out/ipc-opt`. The signed Release package is
`out/anne-boot-speed-package-v8`, run tag `anne-boot-speed-v8`, native image SHA-256
`f5fa04b6f17f39a4b20f095decd9766b1df1f1391fcf144abcd9ca4dec6aaed2`.
v8 was flashed and boot-verified on the ANE-LX1. One boot reached msh in
0.29 s; kernel to services ready was 299.881 ms. All eight CPUs were online
with MMU, instruction cache, and data cache enabled, and the USB diagnostic
shell responded without boot fault markers. This single sample does not
establish an IPC throughput gain on the phone. The image retains sink-only
USB operation, so it does not provide keyboard host power. v7 is preserved.
Package state and capture are in `out/anne-device-private/boot-speed-v8`.

<a id="boot_performance-automatic-usb-switching-and-startup-batching"></a>
### Automatic USB switching and startup batching

The initial automatic switching build added independent Type-C control register
round trips and measured 0.36 s to msh (368.234 ms kernel to services ready).
Reading the contiguous ID/Modes/Control bytes, writing the Modes/Control image
and verifying that image takes three I2C transactions instead of six. The
revised image measured 0.30 s (308.783 ms); the reconnect revision also measured
0.30 s (309.569 ms). Kernel initialization stages remained effectively unchanged.
These are single-boot samples, not averages. Negotiation timers run after service
readiness and do not hold the shell startup path.

The user verified that auto-v2 powers the keyboard and accepts typing, then
reported the Mac accepting source power after the swap back. Auto-v3 adds a
USB data-connection generation check and explicitly restarts Unattached.Sink
after removing host power. Its boot and initial Mac peripheral selection are
verified. The user reported keyboard power cycling with auto-v3. Its retained
log shows successful keyboard enumeration followed by disconnect roughly one
second into each source session. Auto-v4 qualifies latched charger faults
against live OTG_ON, follows the vendor recovered UVP/SCP check, and records
source-stop and connection-epoch details. It measured 0.30 s / 309.113 ms, with
the user again reported cycling. The detailed auto-v4 capture proves the order:
charger software sees `status=0x40 faults=0x08` (healthy live OTG plus latched
OCP), cuts source power, HID disconnects, and then the role guard reacts.
Auto-v5 permits recovered OCP/UVP/SCP at the first healthy maintenance sample
only; stopped boost, OVP/overtemperature, I2C failure and recurring faults still
shut down. Hardware current limits remain at the vendor 500 mA encoding. It
measured 0.30 s / 308.521 ms. The user confirmed stable keyboard typing and
Mac reconnection without receiving power from the phone. The retained capture
shows recovered startup OCP acknowledged with boost healthy, a 6.10 s interval
from keyboard-ready to physical unplug, then peripheral mode with source off
and responsive CDC. It also records one initial stopped-boost/SCP shutdown
and retry before the successful keyboard session; initial attachment can
therefore take longer than the normal negotiation window.
Implementation and policy limits are documented in [the CDC notes](../boards/huawei-anne/operations.md#cdc_serial).
Results and immutable package state are in `out/anne-device-private/usb-auto-v5`.

<a id="boot_performance-verification-and-remaining-costs"></a>
### Verification and remaining costs

The production parser passed ASan/UBSan tests with its input mapped `PROT_READ`:
alias resolution, property ordering, nested console paths, disabled ancestors,
deep and overflowing paths, malformed/truncated data, and compatibility priority.
The original and revised parsers returned identical RAM, UART, GIC, and USB fields
for the captured phone DTB.

For v2, 41 focused host tests passed across DT probing/formats, CPU topology, USB,
serial sender, msh input, and app streaming. Three QEMU boot checks passed:
EL1, EL2, and eight-core EL1. The existing QEMU EL3 diagnostic code lacks its
request-buffer constant in the synthetic build, so these boot-only checks used
a private compile fixture for that constant; no EL3 diagnostic command was run.
The QEMU cache flag was restored afterward.

All eight phone CPUs reported MMU, instruction cache, and data cache enabled
after each v2 and v3 boot. For v2, app tunnel start/run/stop passed with native test exit zero
and memory released. CDC accepted a command after a USB bus reset. The sink-only
Type-C marker remained present.

For v3, 29 focused tests passed across the production DT transform, formats,
CPU topology, and temporary cache tables/CPU transitions. The new cases cover
late console selection, alias/candidate/path-pool overflow, every identity-map
page's permissions, invalid extents, repeated cache entry, and complete register
restoration. Full Debug QEMU boots passed at EL1, EL2, and eight-core EL1 using
the same private EL3 compile fixture. The fixed Debug image also booted on the
phone, returned a complete diagnostic report, and accepted `help` before Release
measurement. All three Release boots were fault-free, retained the sink-only
Type-C marker, and reached the USB shell; two were ordinary shell-requested reboots.

For v4, 30 focused tests passed, including ASan/UBSan execution of the production
identity-map builder for generic, Anne, and synthetic Anne policies. Independent
table decoding checks every page against the original attribute policy, dirty
allocation pages, unmapped holes, allocation failures, duplicate-batch rejection,
user W^X and ownership, and noncached retagging across existing and new spaces.
Full Debug QEMU boots passed at EL1, EL2, and eight-core EL1. Three Release phone
boots reached msh, retained the sink-only marker, and reported MMU, instruction
cache, and data cache enabled on all eight CPUs without a fault.

The host benchmark of the captured 475,354-byte phone DTB improved from a median
486.077 us to 154.627 us (3.14 times), with identical hashes of every semantic
`HwInfo` field. Reproduce using `scripts/test/benchmark-dtb.py` and optionally
`--baseline-dir` pointing to saved `probe.cc`/`fdt.cc`. Host timings describe
warm host caches; use the phone checkpoints for boot-performance claims.

Separate reservation parsing still takes about 75.1 ms in v4; page-table
construction now takes about 1.04 ms. The reservation walker decodes only 28
ranges from the full phone DTB, making another full uncached traversal costly.
v5 removes that traversal; its first measured reservation interval is 0.469 ms.

Private captures, comparisons, and results are in
`out/anne-device-private/boot-speed-v1`, `boot-speed-v2`, `boot-speed-v3`, and
`boot-speed-v4`; averages and per-boot samples are in their `performance.json` files.
The v5 package preparation, first phone capture, and `performance.json` are in
`out/anne-device-private/boot-speed-v5`;
range comparisons, stack reports, and injected-DTB QEMU captures are in
`out/reservations-opt`.
Scheduler snapshots, host benchmarks, and QEMU results are in `out/sched-opt`;
the unflashed v6 package state is in `out/anne-device-private/boot-speed-v6`.
The boot-fix Debug capture is in `out/anne-device-private/dt-cache-fix-v1`;
the original baseline
is `out/anne-device-private/boot-speed-baseline.txt`.

<a id="framebuffer_performance"></a>
## Framebuffer rendering and acceleration

<a id="framebuffer_performance-framebuffer-rendering-and-acceleration"></a>

The interactive path is `msh` → terminal byte ring → `tty.fb0` character grid →
framebuffer command ring → `io.framebuffer`. Huawei's kernel boot console uses
a separate bounded renderer and retains its one-row wrap policy.

The character grid already rotates rows and stores dirty column ranges
separately from character data. Scrolling now shifts that small dirty metadata
with the grid and coalesces pending scrolls into one overlap-safe pixel copy.
Only exposed rows and existing unsent changes are redrawn. The old cursor cell
is restored as part of the dirty ranges. Command admission advances state only
after the ring accepts a command. A burst that replaces every row skips copying.

The raster walks text runs in destination scanline order, resolves font pointers
once per run and expands complete glyph rows with fixed-scale stores. Fill and
glyph stores use ordinary normal-memory accesses so the compiler can combine
adjacent pixels. Full-width copies use one contiguous move; inset copies keep
overlap-safe row ordering. The kernel renderer shares the pixel span helpers.
Ordinary shell typing appends a byte instead of redrawing the complete edit line.

Framebuffer arenas remain uniformly Normal Non-Cacheable. Cacheable character
metadata/font data and sequential pixel accesses help, but the framebuffer itself
does not gain CPU caching. Startup, explicit presents and automatic appliance
presents publish stores with a barrier. The native boot console retains its
dirty-row cache maintenance. No SIMD instructions are enabled by these changes.

<a id="framebuffer_performance-measurement-and-validation"></a>
### Measurement and validation

An AArch64 host comparison against commit
`0d4108bf44f175ea49f2a25014643fddecebe3f2`, compiled at `-O3` with
`-mgeneral-regs-only` in both engines, produced median times across seven samples:

| Operation | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| Clear 1080 × 2280 | 565.3 µs | 144.1 µs | 3.92× |
| 28 characters, scale 1 | 2401.5 ns | 846.0 ns | 2.84× |
| 28 characters, scale 2 | 6594.5 ns | 1793.2 ns | 3.68× |
| 28 characters, scale 3 | 12336.7 ns | 2707.3 ns | 4.56× |
| 28 characters, scale 8, right clipped | 39227.9 ns | 10182.1 ns | 3.85× |

These measure cached host memory and exclude IPC/display consumption. They are
not phone timings. A copy reads and writes pixels, so Normal NC read bandwidth
could limit native scrolling even though command count and glyph work decrease.
Measure native copy versus redraw before claiming a native scrolling speedup.

```sh
python3 scripts/test/benchmark-framebuffer.py
python3 -m unittest tests/test_framebuffer.py tests/test_console.py \
  tests/test_msh_parse.py tests/test_usb.py tests/test_anne_framebuffer.py \
  tests/test_panic_screen.py tests/test_mochi_ring.py
```

The raster probe compares against an independent per-pixel oracle, including
hostile coordinates, clipping, scaling, guard pixels and every copy-overlap
direction. The terminal probe replays actual commands into pixel memory and
checks cursor restoration, margins, repeated/coalesced scrolls and writes during
partial command admission/consumption, including multi-command dirty rows.

<a id="framebuffer_performance-dma-assessment"></a>
### DMA assessment

Display scanout already reads the framebuffer with DMA. It is distinct from a
memory-to-memory copy/fill engine. `libk/dma.h` supplies ordering barriers, not
copy hardware. The framebuffer service currently receives pixel memory and
optional display control/GPU endpoints; there is no generic framebuffer blit
DMA driver, command queue or completion contract in this tree.

Large fills and rectangle copies are candidates for a verified board DMA/2D
engine. Controller capabilities must be checked separately: memory copies,
constant fills and strided rectangle transfers are different capabilities; see
the [DMAengine capability descriptions](https://www.kernel.org/doc/html/latest/driver-api/dmaengine/provider.html).
An inherited DSS read channel alone does not establish a supported blit engine.

Enabling one requires controller discovery/ownership, clock/reset/IRQ support,
device-visible addresses and routing/IOMMU policy, buffer lifetime rules and
bounded completion/recovery. An overlapping scroll requires ordered safe row
transfers or staging; a general asynchronous memcpy cannot be assumed to have
memmove semantics. Later drawing and presentation must wait for completion.
Small glyph commands should stay on the CPU unless measurements justify setup
cost. A DMA timeout must establish that the engine has stopped before CPU fallback
can modify the same memory. No speculative DMA registers are programmed here.

<a id="framebuffer_performance-simd-follow-up"></a>
### SIMD follow-up

The global `-mgeneral-regs-only` flag matches the current register-save paths:
`CpuContext`, `TrapFrame` and host/guest transitions do not preserve vector
registers. Basic ARM64 FP/NEON support needs all 32 128-bit registers plus FPCR
and FPSR (520 bytes of payload, normally 528 with 16-byte alignment), initialized
per thread and saved before that thread can be dispatched on another CPU.

Start with eager saves on actual thread switches, retaining scalar kernel/IRQ
code. Restore on first dispatch and exception return, configure FP access on
every CPU and isolate host/guest vector state at EL2. This avoids placing vector
state on every syscall's hot frame. Test all registers/control bits across
preemption, blocking IPC, migration, destruction/reuse and guest transitions.
Only then enable SIMD for selected userland objects and benchmark vector fills,
copies and glyph expansion. Shared runtime objects need separate build variants
if kernel code must remain scalar.

General kernel SIMD is a further step: explicit ownership/save scopes and rules
for preemption, nesting and interrupts, or preservation at every relevant entry.
Removing the global compiler restriction before that work is unsafe. SIMD can
reduce CPU work in bulk operations, but does not increase DRAM bandwidth or
remove noncached framebuffer traffic.

<a id="performance_refactor"></a>
## Boot, runtime and library refactor

<a id="performance_refactor-boot-runtime-and-library-refactor"></a>

Baseline: uploaded archive revision `7bd9ed13d2d2f3e1ce293bb0c70f3c1a9e1ccc18`.
Measurements use the QEMU development preset, with optional Linux guest
artifacts disabled. They describe this workload and host, not phone or Switch
performance, device power consumption, or a whole-system sampling profile.

<a id="performance_refactor-findings-and-changes"></a>
### Findings and changes

Boot instrumentation now records bundle verification, manifest/image lookup,
suspended process creation, IPC wiring, and service launch/readiness separately.
The longest readiness wait names its service. That wait includes concurrently
running services; it is not the named service's exclusive CPU consumption.
These summaries run only during boot, outside steady-state service records.

The largest unnecessary runtime work found by inspection was keeping threads
runnable without work: the filesystem repeatedly scanned every session,
completed hello/ping/authority demos kept yielding, the PCI placeholder yielded forever,
and xHCI polled continuously. These paths now block through libmochi:

- Both session servers receive one shared incoming wake doorbell. Request data
  stays in separate SPSC rings; reply notifications remain per client.
- `ServerSessions::wait` builds a compact column of live ring addresses and
  uses `InboxWait` to arm/recheck/sleep. Init notifies generation changes before
  waiting for acknowledgement, including changes with no queued request.
- Filesystem idle checks retain a 100 ms deadline to reap dead clients whose
  process-cap revocation produces no doorbell notification.
- `mochi::park` blocks completed demos, the authority probe, placeholder drivers and the manager's
  idle path on a private doorbell, retaining service lifetime and supervisor
  behavior. It exits on allocation/wait errors instead of spinning.
- xHCI bounds ready bursts, then waits for request notifications or its next
  transfer/poll deadline. It still polls device completions because the current
  PCI path lacks a usable interrupt binding. The 1 ms requested fallback is
  limited by the kernel's current 10 ms timer tick; input/completion latency can
  increase accordingly. Ring requests still notify immediately.

The kernel's idle wait now uses WFI with IRQs masked through the sleep boundary.
It unmasks, executes ISB to service the wake, and masks again before touching
queues. Cross-core work publication already sends a GIC SGI; timer interrupts
cover replenishment. WFI does not wake on SEV alone. Lock waiters retain CPU
relax hints because IRQ-masked lock holders cannot safely depend on WFI.

<a id="performance_refactor-library-and-memory-contracts"></a>
### Library and memory contracts

- `libk/cpu.h` owns relax, ISB, DSB and privileged WFI instructions. Interrupt
  policy stays in the scheduler. Drivers use libk primitives without open-coded
  generic assembly; eMMC keeps its existing outer-shareable barrier strength.
- `libk/spin.h` owns acquire/release lock words. Contended waiters poll with
  shared reads instead of repeatedly exchanging/invalidation of the owner line.
  Existing IRQ exclusion and cache-line-separated per-CPU statistics remain.
- `libk/hash.h` owns IEEE and Castagnoli CRC engines, with immutable aligned
  1 KiB tables per polynomial. Boot-disk checksum code delegates to libk.
  Shared format code retains its watchdog/progress policy, polling between
  bounded chunks. It no longer owns duplicate CRC implementations/tables.
- The framebuffer protocol raster moved from the driver into
  `mochi/framebuffer_raster.h`. Contiguous scanline rendering, clipping and
  overlap-safe copies remain; libk retains the pure pixel/byte operations.
- `mochi::driver::map_dma` validates arena type, nonzero size and non-cacheable
  attributes before mapping, publishing output only on success. Virtio and
  xHCI share it. This is a startup check, not a per-transfer cost.
- Virtio's kernel DMA allocation now registers its physical extent as
  non-cacheable before its first write, matching xHCI. The existing mapping
  machinery cleans/invalidates cached identity mappings and makes later
  aliases agree. A registration failure fails closed. A barrier alone cannot
  make mixed cached/non-cacheable aliases safe.

Ring acquire/release publication and the sleep-flag RMW handshake remain intact.
The scalar consumer now rejects cursor distances larger than capacity, matching
the batch consumer, without touching output or advancing ownership. CRC masked
ranges use subtraction after a lower-bound check to avoid offset/length wrap.
Authentication, signatures and on-disk checksums are retained.

Linux host portability fixes cover the printf probe's `uint64_t` type, board-only
delay/MMIO compilation, IPC relaxation instructions and missing libk include
paths. The SIMD fixture now gives each worker a longer first computation so
timer-preemption coverage does not depend on where short yield rounds land
within a 10 ms tick.

<a id="performance_refactor-reproduction"></a>
### Reproduction

From the configured workspace:

```sh
source /workspace/.ame-tools/activate.sh
cmake --preset qemu-dev
cmake --build --preset qemu-dev --target anko mochi_system kernel_regressions -j 4
ASAN_OPTIONS=detect_leaks=0 ctest --preset qemu-dev

python3 scripts/test/benchmark-integrity.py --baseline-dir /path/to/original --runs 7
python3 scripts/test/benchmark-ipc.py --baseline-dir /path/to/original --runs 7
python3 scripts/test/benchmark-scheduler.py --baseline-dir /path/to/original --runs 5
python3 scripts/test/benchmark-boot.py \
  --baseline-build /path/to/original/out/qemu-dev --build out/qemu-dev \
  --output out/boot-comparison --runs 3

python3 scripts/test/simd.py --build out/qemu-dev --cpus 1 2 4
python3 scripts/test/durable-persistence.py --build out/qemu-dev
```

The boot benchmark alternates original/current run order and samples QEMU's
Linux host process CPU time over a fixed post-readiness window. Use a quiet
host. Checksum and IPC benchmarks use cached x86 host memory, not DMA/scanout.
The test command disables only LeakSanitizer, which cannot inspect processes
in this sandbox; AddressSanitizer and UndefinedBehaviorSanitizer stay enabled.

USB key/release/reconnect acceptance requires a development bundle configured
with `-DMOCHI_USB_TRACE_INPUT=ON`, then:

```sh
python3 scripts/test/usb-input.py --build out/qemu-dev --reconnect 2
```

Restore `MOCHI_USB_TRACE_INPUT=OFF` and rebuild afterward. The local sandbox
also needs socket access for QEMU's Unix monitor during this check.

Physical Huawei/Switch execution and hardware cache/power measurements were
not available. Native-board compilation and QEMU validation are separate from
physical acceptance.

<a id="performance_refactor-measured-results"></a>
### Measured results

Three alternating original/current QEMU runs on this host, two emulated CPUs:

| Metric | Original | Refactored | Change |
| --- | ---: | ---: | ---: |
| Readiness, median wall seconds | 1.714 | 1.361 | 20.6% less |
| Post-readiness host CPU seconds / wall second | 1.725 | 0.0349 | 98.0% less |

The CPU value sums QEMU host process time, so it can exceed one on a multicore
host. Each observation used a two-second window after a 0.5-second settle.
Original readiness samples ranged from 1.614 to 1.814 s; current samples from
1.358 to 1.412 s. These are small-sample emulation results, not device power
or hardware boot-time guarantees. The combined change removes runnable service
work and allows WFI; these measurements do not isolate WFI's contribution.

Before parking the authority probe, 20 stopped-guest PC snapshots found CPU1
at the WFI return address while CPU0 alternated between scheduler/syscall work
and `dango.auth_probe`. The probe's completed-success loop still called yield.
Its authority test, gate notification and readiness remain before parking.

Remaining measured boot costs, median of the current three runs:

| Phase | Milliseconds |
| --- | ---: |
| Authenticated bundle validation | 422.5 |
| Dependency launch and readiness | 414.5 |
| Suspended process creation | 214.1 |
| Channels, sessions and capability wiring | 88.8 |
| Manifest parsing/hash and image lookup | 49.8 |

The longest observed readiness wait was `dango.manager` in all three runs,
median 162.5 ms. It overlaps other work; it does not identify an exclusive CPU
hotspot inside that service. Early platform setup and host emulator startup are
outside this phase table.

Seven-run optimized cached host checksum medians:

| Workload | Original ns/byte | Current ns/byte | Speedup |
| --- | ---: | ---: | ---: |
| IEEE CRC, 4 KiB | 5.200 | 2.192 | 2.37x |
| Castagnoli CRC, 1 MiB | 4.410 | 2.365 | 1.86x |
| Castagnoli CRC with masked field, 1 MiB | 4.351 | 2.341 | 1.86x |
| SHA-256, 1 MiB | 3.923 | 3.815 | 1.03x |

SHA-256 itself was not changed. IPC single and batch roundtrips remain broadly
unchanged in the sampled host workloads. No general scheduler throughput
speedup is claimed; concurrent accounting results are sensitive to host noise.
Raw observations and validation outcomes are in `performance-results.json`.

<a id="performance_refactor-validation"></a>
### Validation

- Host suite: 262 tests, 39 skips, all non-skipped tests pass with ASan/UBSan.
- New regressions: CRC polynomial/reference equivalence, chunk progress,
  unaligned/empty/streamed input, overflowing zero ranges, malformed ring
  cursors, parking without yields, DMA rejection before mapping, and session
  sleep/generation races with inactive or queued inputs.
- Kernel lifecycle at 1/2/4 CPUs and supervision restart pass on QEMU.
- SIMD isolation/preemption at 1/2/4 CPUs, EL1 and EL2, passes on QEMU.
- USB key, modifier and release input plus two detach/reconnect cycles pass.
- A 3 MiB durable file survives the write/reboot/read acceptance check.
- Capability manifest validation passes (17 services, 20 resources, 101 channels).
- QEMU, Huawei Anne and Switch development kernel/userland builds succeed.
- USB tracing is restored to OFF in the final QEMU build.

Workspace logs and captures are under `out/qemu-dev/refactor-*`. No physical
device was flashed or written.
