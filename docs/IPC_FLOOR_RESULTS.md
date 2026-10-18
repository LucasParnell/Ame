# Cortex-A53 IPC results — verified checkpoint

These are matched QEMU TCG observations, not physical P20 Lite latency or proof of the hardware floor. Original means the supplied IPC-refactor ZIP, rebuilt with the same Release compiler settings and workload. The final implementation includes a bounded 64-check poll only for cross-core blocking waits; same-core waits poll zero times. Energy and CPU utilization are not measured.

## Configuration

- Cortex-A53, two vCPUs, 512 MiB, EL2, QEMU QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1).
- Clang 19.1.7; Release kernel, unchanged `-O2` portable workload.
- Five independent boots per placement; 512 warmup operations, five rounds of 2,048 operations, 4,096 separate RTT samples, stream window 32.
- All 30 cases completed, including separately labeled unlimited polling cases. Payload and sequence validation stayed inside the timed workload.
- Builds/tests did not run concurrently with accepted timed measurements. Configuration comparison found no compiler/build-mode differences.

RPC figures below are microseconds per complete round trip. Streaming figures are microseconds per logical delivered message, including completion ACK overhead. They are medians of independent run medians, not RTT divided by two. Clock instrumentation is not subtracted.

## Full matrix

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

## Remaining slower estimates

Three median estimates are slower. Their coarse five-run uncertainty intervals overlap; this does not prove equivalence or a regression. They are being investigated separately rather than omitted:

- `stream/different_cpu/blocking/256`: +5.65% (4.7993 → 5.0706 µs).
- `stream/different_cpu/spin/0`: +22.21% (0.3930 → 0.4803 µs).
- `stream/different_cpu/spin/8`: +1.06% (0.5977 → 0.6041 µs).

Several cases exceed 10% run CV; cross-core results are particularly noisy. Pooled RTT samples are correlated observations, not independent repetitions. The raw records retain independent-run distributions, p99/p99.9, counters and all warnings.

## Structural results

- Same-core 4 KiB RPC `context_switches` per operation: 12.5234 → 2.0000.
- Same-core 4 KiB RPC `ipc_syscalls` per operation: 19.6172 → 2.0000.
- Same-core 4 KiB RPC `yields` per operation: 4.2290 → 0.0000.

Normal address-space activation contains no TLBI. Production ASID activation/reuse passed on A53 at EL1 and EL2. Packed 4 KiB records use 65 cursor units instead of 86 fragments and fit in one publication. Inline frame handlers are separated from cold packed/fragmented code. See [implementation details](IPC_FLOOR.md).

## Correctness

- Full suite: 279 tests, 39 expected skips, one initial error caused by missing shell execute bits in the supplied ZIP. Shell execute permissions were restored; the entire affected 25-test Huawei/guest-timer group then passed.
- New end-to-end A53 integrity test passed all 30 cases, partial windows, both placements, polling/blocking and synchronous shutdown.
- Final framing, real blocking data/credit race, end-to-end IPC and Mochi-ring group: six tests passed. IPC rights/lifetimes and 2,000 registration races passed; legacy/current benchmark fixtures both compile.
- Eight-core A53 normal boot, EL1/EL2 SIMD matrix, 100-restart lifecycle and the Release Huawei native build passed.
- The exact measured kernel subsequently passed four-core lifecycle and SIMD workloads without its SHA256 changing.

Host scheduler probes used Clang 19 with CX16 enabled on x86-64 for native 128-bit budget atomics. Cross-builds used the ordinary ARMv8.0 toolchain. Optional skipped tests include missing external emulation/oracle dependencies.

## Artifacts and provenance

- [Original matched A53 data](../benchmarks/ipc/baselines/2026-10-18/ame-original-a53.json)
- [Checkpoint A53 data](../benchmarks/ipc/baselines/2026-10-18/ame-floor-a53.json)
- [Guarded comparison](../benchmarks/ipc/baselines/2026-10-18/ame-floor-a53-comparison.json)
- The delivered ZIP includes accepted serial/build logs and exact benchmark binaries under `performance-artifacts/`.
- Failed shutdown captures and diagnostic intermediate candidates are excluded from the accepted collection.
- Measured kernel SHA256: `6e7aaa2f9f318aa36952543571cd8fe0df3be791df128d831c5d07496ad191e0`.

The adapter, ring library, framing library and measured kernel hashes were checked against the delivered source/binary files. The older saved A57 collections are historical context, not the matched baseline used here.
