# Writing an app with libmochi

The reference app is [`bench.ipc`](../benchmarks/ipc/ame.cc). Its portable IPC
workload remains in `benchmarks/ipc/workload.h`; the Ame adapter demonstrates
application startup, stdout, spawning workers, granting protocol endpoints,
affinity and waiting for their exits. No application runs during normal boot.

Include `mochi/app.h` for an ordinary app and `mochi/app_process.h` when it
launches children. In `_start`, call `mochi::app::start()` to obtain the context.
It maps BootInfo and parent stdout once, even when library helpers call it again.
Write with `mochi::app::write()` or `puts()` and finish with `app::exit(status)`.
The output helpers preserve complete writes under backpressure; `try_write()`
returns the accepted prefix. An isolated launcher without stdout uses debug
output automatically. Applications do not choose startup mapping addresses.

The benchmark's parent follows this sequence:

1. Obtain `context.self_image()`. This maps its immutable executable snapshot
   once; the shell's upload buffer can already have been wiped and released.
2. Keep one `app::Process` per worker and call `configure(context.boot, writer,
   slot)` with distinct slots. The default writer forwards to the parent's
   stdout. The benchmark uses two slots and a shared output wake doorbell.
3. Call `spawn(image, options)`. The child remains suspended with standard
   BootInfo, stdout, topology, thread identity and scoped supervision installed.
   The benchmark disables redundant self-image copies for its worker roles and
   explicitly selects its existing trusted measurement-worker principal.
4. Add only application-specific startup data. The benchmark's `wire()` uses
   `share_ring()`, `share_doorbell()` and `value()` for its transport and role.
   Slot zero, grants, mapping and publication belong to libmochi. A failed grant
   prevents `resume()` from starting that child.
5. Call `resume(affinity_mask)`. Affinity is set before the thread can run.
6. Call `wait_all(children, count, wake, &exit_status)`, then `app::exit()`.
   The group owns that supervisor's complete child-exit stream, forwards each
   child's separate stdout ring, kills peers on failure, reaps every child,
   drains final output and releases their mappings/capabilities.

For a single child, `wait(&exit_status, deadline)` drains output and reaps it.
Expiry returns `MX_ERR_WOULD_BLOCK` and retains ownership so the caller can wait
again or `cancel_and_wait()`. `spawn()` rolls back failed setup automatically.
Cancel a suspended child after failed custom wiring. Keep `Process` state in
stable storage, do not copy a live owner, and assign distinct child slots.

The shell uses this same API: `configure()`, `load(fs, file_handle, file_size)`,
`launch(size, name)`, `discard_upload()` and `wait()`. The USB container feeds the
same bounded upload API incrementally and uses `poll_exit()` in its event loop.
No shell module includes private container implementation headers.

`mochi::va::app` defines the app mapping plan. The upload span, snapshot scratch,
child BootInfo/stdout slots and runtime self-image/stdout windows are separate
from msh's terminal, filesystem and registry windows. A different service that
adopts these APIs must reserve this layout in its own address space too.

Build the example explicitly with `cmake --build BUILD --target ipc_bench_app`.
Use the existing installed `/data/apps/bench.ipc` from msh, or stream the built
`userland/bench.ipc.mxi` through `scripts/anne/tools/app-stream.py` after
`container start`. Apps are excluded from the production boot bundle; the app
installer remains removed.

`test_app_system.py` runs the benchmark twice with msh's terminal addresses
reserved, checks those mappings survive, and validates complete IPC results at
one and four CPUs with both placements. `app_stream_regressions` additionally
checks multi-arena image snapshots, failed-wiring rollback, wait deadlines,
group failure/cancellation, and exact object/page reclamation.

For app-owned copied IPC, use `mochi/app_ipc.h` (`ChannelPair`) and
`mochi/framed_channel.h` (`FramedChannel`). They handle scoped handoff, capacity
planning, named grants, blocking data/credit waits and batches. See the
[IPC API guide](IPC_OPTIMIZATIONS.md) for ownership and examples.
