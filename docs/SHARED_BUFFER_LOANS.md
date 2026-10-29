# Shared-buffer loans

`mochi/loan.h` provides bounded, allocation-free ownership bookkeeping over a
mapped shared arena. `mochi/app_loan.h` creates and grants that arena; application
protocols carry 24-byte descriptors and matching returns over `FramedChannel`.
No syscall ABI or kernel change is needed.

## Setup and ownership

A suspended-child builder creates one pool per producer direction:

```cpp
mochi::app::SharedLoanPool pool;
check(pool.create(4096, 32) == MX_OK);
check(pool.share(producer, true, "bulk") == MX_OK);  // read/write mapping
check(pool.share(consumer, false, "bulk") == MX_OK); // read-only mapping
```

The producer calls `SharedLoanPool::map_writer(boot, address, "bulk")`; the
consumer calls `SharedLoanPool::map_reader(boot, address, "bulk")`. These return
a `LoanWriter` or `LoanReader`; check `valid()` before use. Reserve a page-aligned
mapping window large enough for the arena. Pools support up
to 64 slots. Slots have a cache-line-aligned stride; the app factory rounds the
requested maximum buffer size upward. Allocation remains subject to the kernel's
arena limit (currently 1 MiB). The factory returns allocation errors rather
than silently shrinking the pool.

```cpp
mochi::WriteLoan loan;
auto status = writer.acquire(payload_bytes, loan);
// MX_ERR_WOULD_BLOCK means all slots are owned or in flight: process returns.
check(status == MX_OK);
fill(loan.bytes());
mochi::LoanDescriptor descriptor{};
check(loan.publish(descriptor) == MX_OK);
check(channel.send(Data, sequence, &descriptor, sizeof(descriptor),
                   mochi::FrameFlush::OnWait) == MX_OK);
```

After checking the received frame type, sequence and exact descriptor size:

```cpp
mochi::ReadLoan received;
check(reader.accept(descriptor, received) == MX_OK);
consume(received.bytes());
mochi::LoanDescriptor returned{};
check(received.release(returned) == MX_OK);
check(channel.send(Return, sequence, &returned, sizeof(returned)) == MX_OK);
// The producer validates the return frame and calls writer.reclaim(returned).
```

The sender must use an ordered transport with release/acquire publication,
such as `FramedChannel`; plain copying of a descriptor without synchronization
does not publish the payload. Return descriptors can be batched by the app.
`reclaim(returned, &write_loan)` reacquires a returned buffer for reply validation
or further writing; republishing it uses a fresh generation.

Each object belongs to one thread and cannot be copied. Slots transition from
free to writable to in-flight to free. Writer cancellation before publication
returns a slot locally. Reader release invalidates the view and produces a
return token; it does not itself transmit that token. Duplicate/stale returns,
wrong-pool tokens, out-of-bounds slots/lengths and repeated receives are rejected.
The pool cookie is channel-local correlation, not authority. Do not mix unrelated
sessions or recreate pool objects while descriptors remain outstanding.

Pools must outlive their lease objects. Views retained separately must not be
used after publication/release. Dropping a read loan without releasing it
quarantines its slot. Ambiguous send failures also retain in-flight ownership;
do not reclaim speculatively. Recovery closes the connection and stops both
peers before discarding/recreating the pool. Stop peers before dropping the
parent's arena ownership: revoking a handle does not retract existing mappings.
Manually mapped callers must unmap the entire page-rounded arena extent after
finishing all loans.

These are cooperating shared-memory peers. The consumer maps the whole pool,
and the producer retains writable mapping access. Logical ownership prevents
accidental reuse through this API; it does not enforce per-buffer isolation or
stop a hostile producer from modifying a published payload. Protocols needing
immutable untrusted input still need copied snapshots or stronger isolation.

## Separate 4 KiB benchmark

Build `ipc_loan_app` to produce `userland/bench.ipc.loan.mxi`. This image is
excluded from normal system bundles. After `container start`, stream it with
`scripts/anne/tools/app-stream.py`, then collect `diag log` through CDC. Parse it with:

```sh
python3 scripts/test/benchmark-ipc-loans.py capture.log --output out/loan-results \
  --image BUILD/userland/bench.ipc.loan.mxi
```

The same app runs a copied-ring control and shared loans for both RPC and
streaming. Both generate and validate every payload byte. Loan generation writes
directly into the shared slot; the receiver validates that view. Loan RPC
returns ownership of the same buffer, then the client validates it again. The
copied control sends a copied echo. Streaming returns ownership for each window
(32 by default), including the final partial window. Pool exhaustion reports
backpressure; the benchmark never overwrites unreturned storage. Setup, result
output and allocation are outside timing; ownership bookkeeping is inside.

This is `ame.ipc-loans/v1`, a distinct application contract. Do not merge it with
copied IPC v1 reports or claim a Linux comparison. It does not measure a service
that must independently allocate and populate a different response buffer.

Three native same-core launches, with five rounds of 1024 operations and 256
warmup operations, measured approximately 34.90 → 31.34 µs/RPC and
18.30 → 15.34 µs/stream message (10.2% and 16.2% less time). These are paired
controls from this fixture, not the older benchmark's numbers. The governor and
background services remained active. Raw captures, artifact and parsed results:
`out/ipc-loans-native/`. A further cross-core smoke run uses 33 operations and a
16-message window to exercise partial returns; it is an integrity check, not a
timing baseline. Focused sanitizer tests cover ownership, generations, all 64
slots, malformed tokens, cancellation and repeated reuse.
