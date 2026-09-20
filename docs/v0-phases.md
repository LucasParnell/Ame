# V0 Development Phases

Directly from spec §12 Development Order. Use this as the working checklist.

## V0 Profile

- V0 target: bare minimum Anko microkernel + Mochi userspace.
- Primary boot path: direct QEMU `-kernel build/kernel/anko`.
- `boot/` shim is intentionally out of the V0 critical path.

## Ordered Acceptance Milestones

- [x] Milestone 1: Kernel UART (`ANKO` visible over serial)
- [x] Milestone 2: EL0 init (`init` executes and can issue `sys_debug_puts`)
- [x] Milestone 3: Console hello (`io.console` prints `hello world`)
- [x] Milestone 4: IPC demo (`demo.ping`/`demo.pong` ring+doorbell exchange)
- [x] Milestone 5: Supervision (`demo.pong` crash + restart)
- [x] Milestone 6: Metrics (boot counter report printed)

Current note: acceptance markers now come from real Mochi services
(`init`, `io.console`, `demo.ping`, `demo.pong`) rather than the old EL0
stub path. Full benchmarking remains open work.

## Phase 0 — Build and Run
**Goal:** `kernel prints hello over UART in QEMU`

- [x] Cross-compiler toolchain (`aarch64-none-elf-g++`, or the Homebrew LLVM clang fallback)
- [x] `scripts/linker/kernel.ld`
- [x] `kernel/arch/arm64/entry.S` — stack, BSS clear, call `kernel_main`
- [x] `kernel/drivers/uart/pl011.cc` — raw MMIO putchar
- [x] `kernel_main` prints `ANKO\n` and halts
- [x] QEMU boot command works
- [x] `panic` path halts with message

## Phase 1 — Kernel Base
**Goal:** `kernel receives timer ticks and survives invalid exceptions`

- [x] `kernel/arch/arm64/vectors.S` — VBAR_EL1 table; debug dump handler
- [x] `kernel/arch/arm64/entry.S` / `kernel/mm/vm_space.cc` — MMU enable (page tables + SCTLR_EL1)
- [x] `kernel/mm/phys.cc` — physical page allocator (bitmap)
- [x] `kernel/mm/vm_space.cc` — page table build/map/unmap
- [x] `kernel/init/fdt.cc` — DTB parse: RAM, UART, GIC, timer → `HwInfo`
- [x] `kernel/drivers/gic/gicv2.cc` — distributor + CPU interface init
- [x] `kernel/drivers/timer/generic_timer.cc` — 10ms tick via CNTP

## Phase 2 — Userspace
**Goal:** `init runs in EL0 and prints through syscall`

- [x] `kernel/arch/arm64/syscall.S` — SVC entry/exit frame
- [x] `kernel/syscall/dispatch.cc` — switch on syscall number
- [x] `SYS_DEBUG_PUTS` implemented (bring-up escape hatch)
- [x] `SYS_EXIT` implemented
- [x] `kernel/init/elf_loader.cc` — ELF64 static loader
- [x] `kernel/init/boot_caps.cc` — issue 5 initial caps to init
- [x] `userland/init/main.cc` boots and orchestrates core services

## Phase 3 — Scheduler
**Goal:** `two userspace threads take turns and exit cleanly`

- [x] `kernel/sched/thread.cc` — Thread/Process create/destroy
- [x] `kernel/sched/runqueue.cc` — round-robin queue, `tick`, `yield`, `block`, `wake`
- [x] `kernel/arch/arm64/context.S` — `context_switch`
- [x] `SYS_YIELD` wired to `sched::yield()`
- [x] Timer tick preempts current thread

## Phase 4 — Capabilities
**Goal:** `userspace can only use objects it has caps for`

- [x] `kernel/cap/cap_table.cc` — `alloc`, `lookup`, `revoke`, `dup`
- [x] `SYS_CAP_DUP`, `SYS_CAP_REVOKE`, `SYS_CAP_IDENTIFY`
- [x] Rights checks in every syscall that touches a cap
- [x] `SYS_SPAWN` creates a process with a fresh empty cap table

## Phase 5 — Arenas and Shared Pages
**Goal:** `two processes observe the same shared memory page by capability only`

- [x] `Arena` struct and per-process `ArenaTable`
- [x] `SYS_ARENA_CREATE`, `SYS_ARENA_REGISTER`
- [x] `SYS_MAP_PAGE` maps a CAP_PAGE into the calling process's VA space
- [ ] Shared page mapped into two processes; both read/write it

## Phase 6 — Doorbells
**Goal:** `one process wakes another without polling`

- [x] `kernel/ipc/doorbell.cc` — `doorbell_create`, `notify`, `wait_doorbell`
- [x] `SYS_DOORBELL_CREATE`, `SYS_DOORBELL_SHARE`
- [x] `SYS_NOTIFY`, `SYS_WAIT_DOORBELL`
- [x] Futex-shaped race: `wait` checks counter vs expected before sleeping

## Phase 7 — Rings
**Goal:** `ping and pong exchange 1,000,000 messages with no kernel data copy`

- [x] `kernel/ipc/ring.cc` — `ring_create`, `ring_share`
- [x] `SYS_RING_CREATE`, `SYS_RING_SHARE`
- [x] `userland/lib/include/mochi/ring.h` — `ring_push` / `ring_pop`
- [x] Ring layout verified: head/tail on separate cache lines, 64B slots
- [x] `static_assert` checks on `RingHeader` offsets and `RingSlot` size
- [ ] `demo.ping` + `demo.pong` exchange 1M messages

## Phase 8 — Services
**Goal:** `init starts nameservice, console, ping, and pong from manifest`

- [x] `userland/init/main.cc` — read manifest, spawn all services
- [ ] `userland/nameservice/main.cc` — flat `register`/`resolve` table
- [x] `userland/nameservice/main.cc` — fixed V0 resolve path used by `demo.ping`
- [x] `userland/console/main.cc` — UART cap, log ring, print output
- [x] Boot prints: `hello world` (from `io.console`)
- [x] Boot prints: `MIDIX V0 BOOT OK`

## Phase 9 — Supervision
**Goal:** `demo.pong crashes, init restarts it, demo.ping reconnects`

- [x] Child death notifies supervisor cap
- [x] `init` applies `restart = "always"` / `restart = "never"` from the manifest
- [x] `demo.pong` crashes on `MSG_CRASH`; init restarts it
- [x] Boot prints: `SUPERVISION OK`

## Phase 10 — Measurement Harness
**Goal:** `V0 prints repeatable timing data at boot or on command`

- [ ] `CNTPCT_EL0` accessible to trusted userspace
- [x] Kernel counters: IPC syscall count, context switch count
- [ ] Benchmark report: syscall cost, cap lookup, ring push/pop, ping/pong RTT, notify cost, context switch
- [x] `SYS_DEBUG_COUNTERS` returns requested counter value

## V0 Completion Criteria (spec §13)

All 13 items must be true:

1. QEMU ARM64 boots to userspace
2. `init` runs at EL0
3. Services are spawned from a manifest
4. Capabilities required for all kernel objects
5. Shared ring mapped between two processes
6. `demo.ping` and `demo.pong` communicate through the ring
7. Doorbells wake sleeping receivers
8. `io.console` prints userspace logs
9. Demo service crash notifies `init`
10. `init` restarts the crashed service
11. Kernel IPC counters show no kernel data copy for ring payloads
12. Benchmark report printed
13. Kernel structure allows real ARM hardware with only platform-driver changes
