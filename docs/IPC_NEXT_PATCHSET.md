# Next IPC patchset

Status: subsequently built, flashed and benchmarked at the user's request.
Three native same-core runs passed all 30 cases. 4 KiB streaming measured
21.95 µs/message versus 23.60 µs on the preceding default; empty RPC measured
5.63 µs versus 5.50 µs. The bulk result matches the earlier order-9-only
exploration, so no additional gain from cursor caching has been established.
Raw records and limitations: `out/ipc-next-flash-bench/results.md`.
Cross-core and full regression validation remain outstanding.

| Change | Layer | Purpose |
| --- | --- | --- |
| Publish/release without ending a transaction | libmochi `FrameBatchWriter` / `FrameBatchReader` | Retain exclusive cursor caches while the peer makes progress; end ownership explicitly with `commit()`. |
| Complete-record receive policy | libmochi `FramedChannel`, opt-in `FrameProtocol::CompleteRecords` | Reuse consumer state and cached head across receives, coalesce local tail publication and credit claims, and bypass fragmented reassembly. |
| Seven-record bulk capacity | Benchmark application via existing `FrameGeometry` API | Use order 9, which previously measured 21.96 µs per 4 KiB stream message versus 23.60 µs at order 8. Those results do not validate the new receive/cache changes. |

The channel commits held receive credits before sending, blocking, flushing,
changing polling policy or destruction. Local credit batches release after at
least a quarter ring of cursor units; a final partial batch is released at those
boundaries. Cross-core receives release credits after every complete record so
production can overlap consumption. Cross-core buffered sends publish each
record while retaining the producer cache until a transaction boundary.

Both peers must agree on complete-record framing. This mode rejects sends that
would need fragmentation, and rejects fragmented input. The default compatible
mode and the slotwise benchmark reference retain their existing protocol.
The library's default geometry remains three records; seven is an application
choice. The syscall ABI and kernel are unchanged by this patchset.

The benchmark still generates and validates every byte, uses the same ACK
window, and keeps RPC to one outstanding request. Runner metadata records the
new capacity, receive protocol and local credit threshold. No zero-copy or
validation shortcut is introduced.

Before deployment, validation should concentrate on partial credit batches,
producer backpressure, cursor wrap, malformed frames, policy transitions and
final RPC handoff replies. Cached ring views must remain exclusive, and callers
must flush before unrelated blocking work. Performance needs a fresh native
comparison; no improvement is claimed for the combined patchset yet.
