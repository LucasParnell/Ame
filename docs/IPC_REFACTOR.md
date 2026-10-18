# IPC and scheduler refactor

The subsequent [Cortex-A53 IPC execution-path changes](IPC_FLOOR.md) add ASIDs,
packed records, producer credits, combined waits and bounded direct replies.
The results below describe the supplied earlier refactor, not that later pass.

This pass reduces copied-message overhead and scheduler work while preserving
the existing IPC workload contract, capability checks, affinity, budget policy,
and exception-frame ownership rules. Native Linux/Mach performance leadership
remains a measurement target; QEMU results cannot establish it.

## Changes

- `mochi/framed_ring.h` implements allocation-free copied framing with bounded
  publication and receive batches, defaulting to 16 slots. Singleton messages
  publish immediately. Reassembly copies payload directly into caller-owned
  storage before releasing slots. FIRST/LAST, sequence, opcode, buffer bounds,
  zero-length control messages and cursor wrap remain checked. The transport
  interoperates with the previous slot-at-a-time wire format.
- Ordinary ring, multi-ring and inbox waits now share `ring_publish_sleep`.
  Both publisher and waiter use acquire/release exchanges on the sleep flag.
  If publication wins, arming acquires its head publication; if arming wins,
  publication claims the flag and notifies. The former release-store arm could
  miss publication. An intermediate stress run demonstrated a sleeping client
  with 22 unread reply fragments; that stalled run is excluded from results.
- Empty remote-wake and steal queues use read checks before destructive atomic
  exchanges. Remote publishers signal only empty-to-nonempty transitions. A CAS
  racing a drain retries against null and therefore requests a fresh IPI.
  Release publication still precedes the GIC's store-completion barrier and SGI.
- Thread wake/ownership fields occupy one aligned 64-byte line. Identity/wait
  references and accounting occupy separate lines; saved general and SIMD
  registers are cold and aligned. Thread size is 1,280 bytes. Process identity,
  accounting and supervision occupy separate lines in a 192-byte object.
  Peer-readable CPU load summaries no longer share local dispatch counters.
- Doorbell counter writers retain the IRQ-masked doorbell lock and release
  publication, but avoid an additional atomic increment loop inside that lock.
  Waiters still acquire the counter and register under the lock.
- Scheduler wake metadata reuses one timestamp. Tick accounting reuses its
  already captured time. Generation-checked CPU-local principal weights avoid
  repeated shared-policy reads; default-weight accounting avoids division.
- `libk::CounterScale` precomputes an exact reduced tick-to-nanosecond ratio.
  Integral ratios need a multiply; other ratios use multiply-high and exact
  remainder correction. Read-side division and floating point are absent.
  Tests compare full-width counter values and thousands of frequencies against
  a 128-bit arithmetic oracle, including platform frequencies and overflow.
- Counter reads, interrupt instructions, store synchronization and cache hints
  live in `libk/cpu.h`. Dispatch prefetches the selected register state while
  preparing the context switch. Board FIQ policy remains in the kernel.

## Coherency and correctness

CPU ring pages use normal, inner-shareable mappings. Acquire/release publication
establishes visibility and ownership. Cache prefetch is only a hint; it neither
publishes a frame nor authorizes its reuse. DMA cache policy remains separate.
No per-message cache clean/invalidate is added to coherent CPU communication.

The scheduler still saves the outgoing trap and SIMD frames before releasing
execution ownership. Deferred remote wakes are published only after that point.
Shared budgets, principal generations and bounded handoff donation remain checked.
The original WFI idle sequence and publish-before-IPI barrier are retained.

Blocking stress covers single-ring, multi-ring and inbox waits with real host
threads. Protocol tests cover malformed fragments, poisoned readers, backpressure,
partial progress, old/new interoperability and concurrent cursor wrap. Scheduler
tests cover concurrent multi-producer wake batches, exact membership and renewed
IPIs after drain. Doorbell tests include counter wrap and notification races.

## Reproduction

The normal benchmark selects the production batched adapter:

```sh
python3 scripts/test/ipc-bench.py ame --output out/ipc-results/after-refactor
python3 scripts/test/ipc-bench.py compare \
  benchmarks/ipc/baselines/2026-10-18/ame-qemu.json \
  out/ipc-results/after-refactor/results.json
```

An optional `--reference-framing` control retains the old fragment-at-a-time
adapter while using the current kernel and repaired wait helpers. It isolates
framing changes from scheduler changes; it is not selected for ordinary runs.
The runner records implementation/library identities and publication limits.
The portable workload, payload generation, full validation, clock instrumentation
and operation counting are unchanged. See [the benchmark guide](IPC_BENCHMARK.md)
for interpretation and matching native-machine requirements.

The runner now defaults to `CMAKE_BUILD_TYPE=Release`. The original saved baseline
used RelWithDebInfo. Comparisons across these collections include the compiler
configuration change as well as the code changes; they do not isolate refactoring
alone. Development diagnostic counters remain enabled and are explicitly recorded.

## Final Release results

The final run used `CMAKE_BUILD_TYPE=Release`, verified `-O3 -DNDEBUG` kernel
compiler commands, the unchanged `-O2` portable workload, two cortex-a57 vCPUs,
EL2, and the same three independent observations per case as the initial run.
No tests or builds ran concurrently with the accepted measurement collection.
The old baseline used RelWithDebInfo, so these gains include that build change.

| Same-CPU workload | Initial baseline | Final Release | Observed speedup |
| --- | ---: | ---: | ---: |
| 256-byte RPC | 160.73 µs/RTT | 98.19 µs/RTT | 1.64× |
| 4096-byte RPC | 1539.29 µs/RTT | 621.06 µs/RTT | 2.48× |
| 256-byte streaming | 29391 ns/message | 16355 ns/message | 1.80× |
| 4096-byte streaming | 456129 ns/message | 265740 ns/message | 1.72× |

Same-CPU 4 KiB RPC diagnostic medians fell from 34.52 to 13.14 selected-thread
switches, 18.09 to 6.76 doorbell sleeps, and 5.53 to 2.48 yields per operation.
These counts support the reduction in scheduling and per-fragment wake work.

This is not an across-the-board win. Different-CPU 4 KiB RPC changed from
119.20 to 153.09 µs, an observed regression of about 28%; its three-run aggregate
intervals overlap. Other small-message cases also remain noisy or regress in
their medians. Several cases exceed 10% run CV. Do not infer native CPU costs or
Linux/Mach superiority from this TCG collection. Native testing must resolve
cross-core batching, cache transfer costs and scheduler wake latency before that
performance goal can be considered achieved.

The [complete raw Release collection](../benchmarks/ipc/baselines/2026-10-18/ame-refactored-release.json)
retains every sample, round, counter, build identity and warning. The result
archive also preserves serial logs and exact kernel/bundle binaries. The initial
baseline is retained unchanged. Intermediate candidates and the stalled run are
excluded from the accepted comparison.

## Verification

- Before the scheduler extension, the full host suite passed: 276 tests,
  39 skipped. After the scheduler changes, focused scheduler/IPC sanitizer
  probes passed, including multi-producer IPI coalescing, fairness, budgets,
  exception-frame ownership, doorbell wrap and 2,000 notification races.
- The exact counter-conversion oracle passed with platform, power-of-two,
  arbitrary 32-bit frequencies and full-width tick values.
- Blocking single/multi-ring/inbox stress, framed wire interoperability,
  malformed messages, buffer guards and concurrent cursor wrap passed.
- All 12 benchmark integrity/native-adapter tests passed.
- Four-core normal boot and lifecycle regression passed. SIMD register-state
  regressions passed at two/four cores and EL1/EL2 entry.
- The exact measured Release kernel subsequently passed four-core normal boot,
  lifecycle and EL1/EL2 SIMD checks; its raw image hash remained unchanged.
- The final Release IPC matrix completed all 20 cases with three independent
  observations per case, without a stall or payload-validation failure.
- Optimized ARM disassembly confirms the clock read uses MRS, MUL/UMULH and
  correction arithmetic with no UDIV on the read path. No coherency barrier or
  capability/ownership check was removed to obtain these results.
