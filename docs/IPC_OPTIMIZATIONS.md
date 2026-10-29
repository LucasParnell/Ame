# Reusable IPC and scheduling improvements

The two-process benchmark now uses application APIs. Payload generation,
validation, RPC/stream semantics, window size and workload hash are unchanged.
Optimisations live in the transport and kernel, rather than benchmark-only code.

For applications that can transfer shared-buffer ownership instead of copied
messages, see [shared-buffer loans](SHARED_BUFFER_LOANS.md). This is a separate
contract with explicit returns and its own paired benchmark.

| Work | Layer and interface | Reason |
| --- | --- | --- |
| App handoff | `app::Process::handoff_doorbell`, `app::ChannelPair`; retained kernel receiver binding | A controlled-thread capability authorises lending execution to that thread, without general latency hints. |
| Bulk capacity | `mochi::FrameGeometry::for_records(max_payload, queued_records)` | Apps specify record size and queue depth. Three 4 KiB records choose order 8. |
| Data/credit exchange | `FramedChannel`, `Ring::wait_writable_after_notify`; conditional-wait ABI | The library owns deferred wakes and flushes before blocking, changing policy or closing. Writable waits require room for a whole record. |
| Scheduler deadlines | `anko/timer.h`, scheduler/IPC deadline owners; `libk::earlier_deadline` | CPU timers serve slices, handoff, refill and timed waits. Separate 100 Hz maintenance preserves log time and governor cadence. |
| Authority validation | `cap::lookup_many`; IPC resolver | Validate and retain a complete authority set under one table lock, before side effects. |
| Sole receiver dispatch | CPU-owned `Eki::direct_ready`; accounting | Skip heap insertion/removal for a sole eligible local receiver. Competition restores ordinary queues; common weights use exact division-free scaling. |
| Record batching | `FrameBatchWriter`, `FrameBatchReader` | Copy complete records and publish head/tail once per transaction. Peer cursor caches cannot escape that transaction. |
| Polling/placement | `IpcWaitPolicy::between`, channel-local controller | Pinned placement determines whether polling is useful. Same-core waits always poll zero times; adaptation never silently changes other channels. |
| Bulk diagnosis | `bench.ipc.components`, `benchmark-ipc-components.py` | Profile generation, validation, copy and framing separately before changing copying or SIMD ownership. Zero-copy would require a separate contract. |

A restart regression also exposed an existing mapping bug: a default ring occupies
**two** 4 KiB pages, including the header, but session reset unmapped one.
`mx_ring_mapping_bytes` and `mochi::unmap_ring` now define and perform complete
teardown, including endpoint setup rollback.

## Application use

A parent includes `mochi/app_ipc.h` and uses its suspended `app::Process` children:

```cpp
mochi::app::ChannelPair pair;
const auto geometry = mochi::FrameGeometry::for_records(4096, 3);
check(pair.create(children[1], geometry) == MX_OK);
check(pair.share(children[0], false, "rpc") == MX_OK);
check(pair.share(children[1], true, "rpc") == MX_OK);
// Add protocol roles/peer identities, then resume with chosen hard affinities.
```

The pair owns six capabilities and rolls back failed construction. Geometry
travels in named BootInfo entries. Children include `mochi/framed_channel.h`:

```cpp
const auto endpoint = mochi::app::ChannelPair::map(boot, tx_address, rx_address, "rpc");
mochi::FramedChannel channel(endpoint.tx(), endpoint.rx(),
    mochi::IpcWaitPolicy::between(local_cpu, peer_cpu), mochi::IpcRole::Caller);
mochi::FrameInfo response;
const auto status = channel.call(opcode, sequence, libk::as_bytes(request),
                                 response, libk::as_bytes(reply));
```

The example's protocol values and buffers are supplied by the application. Use
non-overlapping page-aligned mapping windows sized by `mx_ring_mapping_bytes`.
Channel views borrow mappings/capabilities; their application owns that lifetime.
Each channel is exclusive to one thread and cannot be copied.

For a protocol whose messages always fit the ring, both peers can pass
`FrameProtocol::CompleteRecords` as the final channel constructor argument.
Large records require packed framing. Oversize sends are rejected instead of
fragmented; the default `Compatible` protocol retains fragmentation. Complete
record channels retain a consumer cursor cache across receives and return local
credits in runs of at least a quarter of the ring. They release a final partial
run before a send, wait, flush, policy change or destruction; cross-core credit
release remains prompt. Do not use other consumer views while the channel owns
its cache. Call `flush()` before unrelated blocking work after consuming records.
This receive path is part of the follow-up with native same-core validation:
[follow-up patchset](IPC_NEXT_PATCHSET.md).

A server uses `IpcRole::Server`, `receive` and `send`. Sends flush immediately by
default; `FrameFlush::OnWait` carries a wake into the next receive, including
reply-and-wait. Call `finish_reply()` after the final response before leaving the
server protocol. A thread has one active handoff chain: do not interleave replies
for unrelated channels.

`FrameFlush::Buffered` copies input bytes immediately, publishing when capacity
requires progress, on receive, or on `flush`. Cross-core channels publish each
complete record promptly so consumption can overlap production. Flush the final
partial local batch.
Errors can leave a valid prefix published; correlation remains the protocol's
responsibility. Lower-level `FrameBatchWriter`/`FrameBatchReader` transactions
support complete inline/packed records that fit the ring. Never interleave
another view while a transaction is uncommitted. `FrameWriter`/`FrameReader`
retain legacy fragmentation. Readers copy before releasing storage and poison
themselves on malformed framing.

`FrameBatchWriter::publish()` and `FrameBatchReader::release()` make progress
visible while retaining exclusive cursor ownership. `commit()` additionally
ends that ownership. This lets a cross-core producer publish promptly without
reloading its own head and the peer tail for every record; it refreshes peer
progress when capacity requires it.

Polling must be negotiated between peers. Policy changes flush pending wakes.
The adaptive controller observes kernel entries, which may return without
sleeping; it has a bounded channel-local budget. Measure CPU consumption along
with latency when tuning it.

## ABI extensions

* `DOORBELL_CREATE(HANDOFF_OK | BOUND_HANDOFF, thread_cap)` requires
  `THREAD_CONTROL`, retains that identity and hands off only to that receiver.
  Other scheduling hints remain trusted-only.
* `WAIT_RING` with direction `WRITABLE | MIN_SPACE` uses argument 4 as required
  free cursor units. Without `MIN_SPACE`, argument 4 remains ignored.
* `NOTIFY_WAIT` with `EXCHANGE_WRITABLE` uses argument 5 as required room and
  rejects synchronous request/reply flags. Readable exchanges ignore argument 5.
* `IPC_POLICY` operations 5/6 read the current thread's cumulative/max wake delay
  and dispatch count. Read outside timed work. Delay covers event publication
  through queueing/frame publication to dispatch.

Old binaries retain their syscall behavior. New applications using extensions
need the matching kernel; an older kernel rejects them.

## Validation and diagnosis

Sanitizer probes cover rights/lifetime failures, complete-record credits, timer
selection/cancellation and CPU ownership, solo dispatch with RT competition,
concurrent batching through cursor wrap, malformed records and complete mapping
teardown. QEMU app regressions cover repeated execution, fresh shell input,
connection generations, shell logout/restart, one/four CPUs and both placements.

Build the independent component app with `cmake --build BUILD --target ipc_components`.
After `container start` in msh, stream `BUILD/userland/bench.ipc.components.mxi`
using `scripts/anne/tools/app-stream.py`; this creates no persistent file. Host diagnosis:

```sh
python3 scripts/test/benchmark-ipc-components.py --output out/ipc-components
```

These are single-process component costs, not IPC RTTs. `ipc-bench.py ame
--ring-order 7`, `8` or `9` allows matched capacity experiments; the runner records
geometry and transport hashes. Keep workload, CPU placement, power and validation
fixed when comparing native measurements.
