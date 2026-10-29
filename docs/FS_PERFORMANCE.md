# Anne filesystem measurements, 18 October 2026

The bottleneck was measured on the connected ANE-LX1 before changing the
transport or recovery algorithm. All intervals below use the monotonic clock.
Boot means kernel entry to `services.ready`; it excludes Huawei's bootloader.

| Image | Initial metadata (ms) | Writable store mount (ms) | Recovery (ms) | Boot (ms) |
| --- | ---: | ---: | ---: | ---: |
| `anne-fs-profile-v1` | 32.799 | 52.820 | 52.686 | 341.710 |
| `anne-fs-fast-v1`, first boot | 13.691 | 36.465 | 36.328 | 311.740 |
| `anne-fs-fast-v2`, first boot | 13.849 | 7.614 | 7.476 | 281.666 |
| `anne-emmc-hs400-v4`, first boot | 13.905 | 5.694 | 5.560 | 221.760 |

The original recovery processed 12 fragments and five home pages. Six writes
(five pages and the checkpoint, 24 KiB total) took 42.549 ms; the final flush
took another 8.791 ms. Clearing the store's working state took only 0.101 ms.
The missing time in the old read-only mount counters was mostly synchronous
write/flush latency, not zeroing or tree lookup work.

The bounded block client used to yield repeatedly while waiting for replies.
It now parks on the reply doorbell with an absolute deadline, using the same
publish/recheck protocol as unbounded receives. Timeout still poisons the
connection: a late reply cannot permit reuse of its shared DMA buffer.

Recovery still validates the complete committed journal, including both
fragments of every image, target bounds, duplicate targets, checkpoint roots
and CRCs. An equal checkpoint LSN still requires recovery. Validated images
are compared exactly against home pages, in bounded contiguous read batches;
only differing runs are rewritten. The checkpoint gets the same exact
comparison. The final flush remains mandatory even when every image matches,
because reads can be served from the card's volatile cache. On this clean
volume all five images match, so mount performs zero writes. No disk-format
change or clean-unmount assumption is involved.

The DMA boundary uses libk's bounded bulk copy, and exact page comparison uses
`libk::equal`. Mount counters are cold instrumentation; optional store phase
profiling adds no clock reads to ordinary filesystem operations.

Three `anne-fs-fast-v2` boots measured 281.666, 281.734 and 281.567 ms
(median 281.666 ms). Its summed mount intervals were 21.463–23.169 ms.
The original profile is one baseline sample, not a statistical distribution.
The further hardware change and its validation are described in
[Anne eMMC](../boards/huawei-anne/EMMC.md#hs400-handoff-and-clock-audit-2026-10-18).

Sanitizer probes cover lost wakeups, spurious wakes, deadlines and disarming,
partial writes and late replies, exact comparison bounds, clean recovery with
no writes but a mandatory flush, one-byte damage with an equal LSN, power cuts
during repair, failed reads/flushes, and the existing 2,790 sector-torn and
reordered transaction crash cases. Capacity/ENOSPC and immutable file checks
continue to pass.

Private captures are under `out/anne-device-private/fs-mount-profile-v1`,
`fs-mount-fast-v1`, `fs-mount-fast-v2`, and `emmc-hs400-v4`. They include complete
boot logs, package hashes and acknowledged target-specific flashes.
