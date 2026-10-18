# Boot, runtime and library refactor

Baseline: uploaded archive revision `7bd9ed13d2d2f3e1ce293bb0c70f3c1a9e1ccc18`.
Measurements use the QEMU development preset, with optional Linux guest
artifacts disabled. They describe this workload and host, not phone or Switch
performance, device power consumption, or a whole-system sampling profile.

## Findings and changes

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

## Library and memory contracts

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

## Reproduction

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

## Measured results

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

## Validation

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
