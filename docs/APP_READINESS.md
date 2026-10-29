# App readiness: what Anko and Mochi still need

Installer removal (2026-10-19): the handset capture showed `dev.apps` holding
startup for 145.378 ms while writing the 241,664-byte benchmark. The boot
installer and init's payload allocation/copy/grant have been removed, along
with the embedded benchmark object and the container's unused FS session.
The existing `/data/apps/bench.ipc` is retained on the handset. App snapshots,
stdout, supervision and shell launch remain available on demand. The QEMU
regression now uses a host-prepopulated disk image rather than installing an
application during guest startup.

Current follow-up (2026-10-19): static analysis found and fixed the ordinary-app
launch mismatch in `bench.ipc`, stale benchmark reuse in release builds,
page-rounded installation lengths, shell image-map leaks and unsafe resume on
wiring failure, stdout truncation after 1024 yields, and missing arena writes
in the volatile FS backend. The shell and USB container now share one resource
owner and startup contract, including immutable executable snapshots and a
supervisor delegation scoped to the app. The benchmark relays separate worker
output rings and returns its exit status. The shell's unconditional startup
benchmark/debug probe was removed.

Validation: `test_app_system.py` boots the complete Cortex-A53 system with one
and four CPUs, uses a preinstalled executable, rejects missing/malformed executables,
runs it twice, parses complete results and verifies filesystem access after
exit. Same-core and cross-core blocking/spin cases pass. Streamed-app cleanup
and immutable image snapshots pass the ten-cycle kernel fixture; the native
P20 Lite release kernel and system bundle build successfully. The preceding
image booted on the handset and exposed the synchronous installer delay.
The image with the installer removed subsequently booted on the handset:
the shell reported 0.14 s and services became ready at 152.745 ms from entry.

Public API follow-up: `bench.ipc` itself is the reference application for
`mochi::app::start()`, stdout, executable snapshots, `app::Process`, suspended
startup wiring and `wait_all()`. The shell and USB launcher use that same public
owner; see [the app API guide](APP_API.md). The Release API image was flashed
without reinstalling the app or adding work to the boot path. The existing
installed benchmark completed all ten same-core cases on P20 Lite, produced a
complete parseable capture and exited with status 0. That installed executable
predates the public API refactor; the new example passes QEMU at one and four
CPUs, including cross-core placement. After the handset run, the terminal
stopped accepting serial and keyboard input although USB requests still
received replies. Reliable post-exit input and running the newly built example
on hardware remain under investigation; the successful benchmark run does not
establish those checks.

See [the app startup contract](PROGRAMMER_ABI_V0.md#native-application-startup)
and [benchmark app instructions](IPC_BENCHMARK.md#running-the-benchmark-as-an-app-on-p20-lite).

The earlier investigation below records the original blockers. Its outstanding
items are updated where this follow-up supplies the missing implementation.

Status of running an ordinary application (the IPC benchmark is the reference
payload) on the current tree, the defects fixed while getting it to run, and the
gaps that remain.

Measured on this tree, QEMU `virt` (Cortex-A53, EL2), `qemu-dev`:

| Check | Result |
|---|---|
| Full system boot, 1 / 2 / 4 CPUs | `init: all services ready` in 1.03 / 1.26 / 1.05 s |
| Kernel lifecycle fixture (revoke, restart, VM, channel) | `lifecycle regression passed` |
| App-stream fixture (spawn, exit, revoke, reuse; 10 cycles) | `app streaming kernel regression passed` |
| IPC benchmark smoke (`--runs 1 --iterations 33 --rounds 3 --window 16`) | results produced; e.g. 256 B different-CPU RPC 65.1 us/op |

## Defects fixed in this pass

### 1. Kernel stack overflow corrupted `.data` (boot blocker)

`integrity::crc32c_zero_field()` put a 4 KiB `zero_block` on the stack. Image
validation runs on the kernel's single 16 KiB boot stack, and the
`parse_image` -> `validate_image_header` -> `crc32c_zero_field` chain already
used about 15.7 KiB of it. A hardware watchpoint on `g_online_cpu_mask`
(`0x40107ef0`) caught the memset writing `0x40107210..0x40108210`, straight
through `.data` (`0x40107d00..0x40107f88`).

Effect: the first `sys_thread_set_affinity()` in init failed (E_INVALID, because
`online_mask()` had been zeroed), so the system stopped at
`init: service affinity failed`. The same smash during `mx_spawn` is the
mechanism behind "the shell loses its terminal after a child exits": the write
lands on kernel globals, not on anything the caller did wrong.

Fix: `shared/format/integrity.cc` keeps the zero block as a `static constexpr`
array in read-only storage. The stack is 4 KiB shallower and the tree boots at
1/2/4 CPUs.

### 2. IPC benchmark mapped BootInfo twice

`benchmarks/ipc/ame.cc` began with `appio::attach(mochi::boot::map(), ...)` and
then `mochi::boot::map()` again. The second `mx_map_page` at `0x10000000` is
rejected because the arena window is already registered, so `map()` called
`invalid_boot()` and the benchmark died with `invalid or incompatible BootInfo`
before printing anything. Map once and pass the pointer to `attach`.

### 3. `SYS_CAP_REVOKE` could unbind a freed IRQ line

The handler copied the `IrqLine*` out of a temporary `cap::Ref`, released the
ref, then called `cap::revoke()` (which drops the table's reference) and only
then `irq::unbind_line()`. A line whose last capability was the revoked one
would already be destroyed when `unbind_line` read `line->irq_id`. Unbind first
and keep the `Ref` alive across both operations.

### 4. `msh exec` never released the child's endpoints

The teardown was commented out ("revoking here is the current suspect..."). Each
exec leaked the ring mapping, the boot-arena mapping and three capabilities.
Worse, `arena_unmap_page()` only releases a mapping once every page is unmapped,
so a partial unmap leaves the window registered and the next `mx_map_page` at
the same VA fails. `cmd_exec` now unmaps every page of the log ring and the boot
arena, then revokes ring, doorbell and arena. Process and thread need no revoke:
the kernel revokes capabilities naming a dead thread or process.

### 5. `dev.apps` was board and app specific

The container hard-coded `bench.ipc`, required `usb.fn.bulk`, was excluded from
`qemu-virt`, and aborted when the USB transport was absent. It now:

- starts after `io.fs` and is part of the QEMU profile;
- installs whatever the bundle carries, under the name published as `app.name`
  (init, which owns the verified bundle, packs it into the entry's two aux words
  with `mochi::boot::string_pack`), defaulting to `app.bin`;
- runs without a host transport: it installs the payload and then idles instead
  of calling `invalid_boot()`.

Device wiring stays in the manifest and in init; the container has no board
knowledge.

## What still needs implementing

Ordered by what blocks a normal app, not by subsystem.

### Anko

1. **Stack headroom and a guard.** A single 16 KiB boot stack serves all
   syscalls; image validation alone reaches about 15.7 KiB before any 4 KiB
   frame. The static zero block buys headroom, but the margin is still thin.
   Either move syscalls onto a per-thread kernel stack or add a canary at
   `__boot_stack_bottom` checked on exception entry, so an overflow panics
   instead of rewriting `.data`.

2. **Revocation of derived capabilities.** `cap::revoke()` invalidates one slot;
   `dup()` and `SYS_CAP_GRANT` create independent capabilities that survive it.
   `docs/architecture.md` promises `revoke_subtree` with a derivation tree.
   Either implement it or state the weaker contract, because a service cannot
   currently withdraw authority it handed out.

3. **Revoke result.** `SYS_CAP_REVOKE` returns `E_OK` for a handle that never
   existed or was already revoked, so a caller cannot distinguish "dropped" from
   "was not mine". Return `E_NOTFOUND` for an invalid handle; existing callers
   ignore the status.

4. **Partial unmap is a trap.** `SYS_UNMAP_PAGE` removes one PTE but
   `arena_unmap_page()` keeps the window registered until every page is
   unmapped. A caller that unmaps one page of a multi-page ring both pins the
   backing and blocks the window. Either release on the last mapped page and
   keep the window usable, or reject a partial unmap explicitly.

### Mochi

5. **Richer application context.** The common startup contract supplies stdout,
   CPU topology, immutable self-image snapshots and scoped child supervision.
   Arguments, cwd, nameservice and per-app filesystem sessions remain absent.

6. **App deployment.** Installation is outside the boot path. The previous
   boot installer was removed after the app reached the handset filesystem.
   A future explicit deployment mechanism needs atomic replacement; the FS
   rename protocol currently refuses an existing destination.

7. **Application policy.** Both launchers already use `native_sandboxed`
   (principal **2**, not trusted). The benchmark's two measurement workers use
   `native_trusted`. Per-app filesystem scopes, quotas and restrictions on
   delegated spawn policy still need a defined security contract.
