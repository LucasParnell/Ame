# IPC benchmark

This measures application-visible communication between two processes in separate
address spaces. The same C++ workload runs on Ame rings, Linux Unix-domain
`SOCK_SEQPACKET`, and a Linux pipe pair. It measures copied messages with blocking
waits by default. It is suitable for comparing that service contract across OSes
on a matching machine, and for measuring regressions within Ame.

The default QEMU model is now Cortex-A53, matching the primary P20 Lite ISA.
Use `--cpu cortex-a57` only when reproducing the historical emulator setup.
See [the current IPC paths](IPC_FLOOR.md) for ASIDs, packed framing and combined
waits; the saved baseline documents describe the earlier implementation.

[The initial results](IPC_BASELINE.md) include raw timings and configuration. Ame
was measured under QEMU TCG; the Linux references ran on the host. Those results
are separate baselines and must not be used to rank the operating systems.

## Run

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

## Workload contract v1

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

## Timing and statistics

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

## Transport and diagnostic details

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

## Comparing operating systems and preserving baselines

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
