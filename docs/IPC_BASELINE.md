# IPC baseline — 2026-10-18

These are reproducible starting observations, not a cross-OS ranking. Ame ran in AArch64 QEMU TCG; Linux ran directly on the x86-64 host. The comparison guard rejects that pairing. Physical AArch64 measurements remain necessary for an OS comparison.

See [the benchmark guide](IPC_BENCHMARK.md) for commands, workload semantics, timing, limitations and comparison rules. Full raw per-round timings, RTT samples, counters and provenance are retained in the linked JSON files.

All three collections use the same compiled workload hash and default parameters: 0/8/48/256/4096-byte payloads, blocking waits, 256 warmup operations, 5 × 1,024 aggregate operations, 4,096 separate RTT samples per RPC case, stream window 32, and 3 independent process pairs per case. Ame uses six boots to obtain three pairs for each placement. Tests and other benchmarks were not run concurrently with these baseline collections.

The tables show the median of independent run medians. p99 is the separately instrumented pooled RTT, not the p99 of aggregate batches. Stream throughput counts logical messages and their completion ACK cost; MiB/s counts only delivered payload bytes. Zero-byte messages have zero payload goodput.

## Ame / QEMU TCG

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

## Linux / Unix SOCK_SEQPACKET

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

## Linux / pipe pair

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

## What to investigate next

The Ame same-CPU 4 KiB RPC takes about 1.54 ms and shows median diagnostic deltas of 34.52 selected-thread switches, 5.53 yields and 18.09 doorbell sleeps per operation. A logical message needs 86 ring slots, while the ring holds 64. Fragmentation and producer backpressure are a useful first target: inspect wake coalescing, producer-space notification and large-message transfer strategies. These counters identify work to investigate; they do not by themselves prove which change will help.

Ame inline same-CPU 48-byte RPC is about 90.69 µs with two selected-thread switches per operation, giving a separate wakeup/scheduler path to track. Different-CPU blocking cases have zero selected-thread changes when the same worker resumes after idle; the nonzero sleep/wake counters show why zero is not equivalent to no scheduler cost.

Several cases have more than 10% run CV; same-CPU 48-byte streaming reaches roughly 47% in this exploratory Ame collection. Use longer batches, more independent repetitions and a quieter or physical machine before claiming small gains. Three repetitions are a starting baseline, not strong evidence for small improvements.

## Validation

- 12 benchmark integrity/integration tests passed; both Linux transports ran under ASan/UBSan.
- Full default 20-case matrices completed with three observations per case for Ame and each Linux adapter.
- One-vCPU Ame smoke: four selected cases passed, eight unavailable cases skipped.
- Different-vCPU Ame smoke: 20 blocking/spin cases passed with all payload sizes, partial final windows and ring backpressure.
- Ame baseline self-comparison returned exactly 1.0× for all 20 cases. Native Linux vs Ame TCG was rejected.
- The recorded Ame kernel and both placement bundles rebuilt to their recorded SHA256 values.
- The normal `anko mochi_system` build and two-vCPU normal-service boot passed; boot-runner tests passed.

The short smoke runs establish correctness only. Their timings are not used in these baseline tables.
