# Midix V0 — ARM Target

V0 is the smallest ARM-first Midix system that proves the core architecture: a small kernel, explicit capabilities, registered memory arenas, shared-memory IPC with doorbells, userspace services, and supervised restart. Anything that does not prove those ideas is deferred.

V0 is not a phone OS, not a desktop OS, not a graphics platform, and not a general Linux replacement. It is a bring-up milestone: boot, create userspace, pass capabilities, communicate through shared rings, print to console, recover a simple service, and measure the hot paths.

For implementation, V0 is explicitly the bare minimum **Anko microkernel +
Mochi userspace** needed to prove those contracts.

---

## 1. V0 Goal

V0 must demonstrate the following on an ARM target:

1. The kernel boots on ARM64.
2. The kernel parses a Device Tree enough to find CPU topology, RAM, timer, interrupt controller, and UART.
3. The kernel creates an initial userspace process, `init`.
4. `init` starts a small fixed set of userspace services.
5. Services receive authority only through capabilities.
6. Two userspace processes communicate through a shared-memory ring.
7. A sleeping receiver can be woken through a doorbell notification.
8. A simple console service can print kernel and userspace logs.
9. A crashed demo service can be restarted by its supervisor.
10. The system can measure the IPC and wakeup paths.

If V0 does these reliably, the core Midix architecture is alive.

---

## 2. Target Hardware

### Primary V0 Target

V0 targets **ARMv8-A AArch64**.

The preferred bring-up path is:

1. **QEMU `virt` machine** — primary development and CI target.
2. **PinePhone-class ARM board** — first real hardware target.

V0 development path uses direct QEMU kernel boot:

```text
qemu-system-aarch64 ... -kernel build/kernel/anko
```

The standalone `boot/` shim is intentionally out of the V0 critical path.

QEMU is the truth for early correctness. Real hardware is the truth for timing, interrupts, cache behavior, and platform friction.

### Required Hardware Features

V0 assumes:

- AArch64 EL1 kernel execution.
- EL0 userspace.
- Generic timer.
- GICv2 or GICv3 interrupt controller.
- Flat physical RAM region discovered through Device Tree.
- UART available from Device Tree.
- MMU enabled by the kernel.
- 4 KiB pages.

V0 does **not** require:

- SMP correctness beyond a minimal single-core mode.
- GPU.
- SMMU.
- PCIe.
- storage.
- networking.
- display framebuffer.
- suspend/resume.
- Linux VM support.

SMP support may exist experimentally, but V0 correctness is defined on one CPU first.

---

## 3. Non-Goals

V0 explicitly defers:

- Linux driver appliance VMs.
- GPU service.
- filesystem.
- persistent snapshots.
- full suspend/resume.
- multi-user identity.
- native GUI.
- advanced scheduler policy.
- hard real-time guarantees.
- x86 support.
- IOMMU/SMMU enforcement.
- full capability derivation trees.
- complete revocation audit tooling.
- precise cycle-budget guarantees.

These are architectural targets, not V0 requirements.

---

## 4. V0 System Shape

V0 contains:

```text
+--------------------------------------------------+
|                    userspace                     |
|                                                  |
|  init                                            |
|   |                                              |
|   +-- nameservice                                |
|   +-- io.console                                 |
|   +-- demo.ping                                  |
|   +-- demo.pong                                  |
|                                                  |
+---------------------- IPC -----------------------+
|             shared rings + doorbells             |
+--------------------------------------------------+
|                     kernel                       |
|                                                  |
|  scheduler                                       |
|  address spaces                                  |
|  threads                                         |
|  capability tables                               |
|  arena registration                              |
|  shared-page mapping                             |
|  notify / wait_doorbell                          |
|  timer                                           |
|  IRQ routing                                     |
|  UART bootstrap                                  |
|  Device Tree parser                              |
|                                                  |
+--------------------------------------------------+
|                   ARM hardware                   |
+--------------------------------------------------+
```

The kernel remains mechanism. Policy lives in `init`, `nameservice`, and service supervisors.

---

## 5. Kernel V0 Responsibilities

The V0 kernel implements only the primitives needed to boot and prove the core model.

### 5.1 Boot

The kernel must:

1. receive control from the bootloader or QEMU loader;
2. establish an early stack;
3. clear BSS;
4. parse the Device Tree pointer;
5. discover RAM;
6. create initial page tables;
7. enable the MMU;
8. initialise exception vectors;
9. initialise the generic timer;
10. initialise the interrupt controller enough for timer and UART;
11. create kernel heap or boot allocator;
12. create the first userspace address space;
13. load `init` from the boot image;
14. enter EL0.

### 5.2 Memory Management

V0 supports:

- 4 KiB pages.
- identity-mapped early kernel region during boot.
- higher-half or fixed-offset kernel mapping after MMU setup.
- per-process user page tables.
- user/kernel isolation.
- mapping and unmapping user pages.
- shared physical page mapping into multiple address spaces.
- simple physical page allocator.
- kernel-owned metadata allocation.

V0 does not need demand paging. All V0 memory is eagerly mapped.

### 5.3 Address Spaces

Each process has:

- one address space;
- one capability table;
- one or more threads;
- one registered arena table;
- a principal class;
- a supervisor relationship.

V0 may restrict each process to one thread except where the scheduler tests require more.

### 5.4 Threads and Scheduler

V0 scheduler requirements:

- single-core round-robin scheduler;
- runnable, blocked, sleeping, dead states;
- timer tick preemption;
- explicit yield syscall;
- blocking on doorbell;
- waking a blocked thread;
- simple priority field reserved but not yet policy-heavy.

V0 may implement priorities minimally:

```text
0 = idle
1 = normal
2 = service
3 = kernel-critical userspace service
```

Only correctness is required. Advanced real-time scheduling is deferred.

### 5.5 Capability Tables

V0 implements dense per-process capability tables.

Handle format:

```text
bits  0..31  index
bits 32..63  generation
```

V0 cap entry:

```c
struct cap_entry {
    void* target;
    uint32_t rights_mask;
    uint32_t generation;
    uint16_t type_tag;
    uint16_t flags;
    uint8_t reserved[44];
};
```

V0 cap types:

- `CAP_NULL`
- `CAP_THREAD`
- `CAP_PROCESS`
- `CAP_PAGE`
- `CAP_SHARED_RING`
- `CAP_DOORBELL`
- `CAP_ARENA`
- `CAP_NAMESERVICE`
- `CAP_SUPERVISOR`
- `CAP_UART`
- `CAP_TIMER`

V0 rights:

- `READ`
- `WRITE`
- `MAP`
- `EXEC`
- `NOTIFY`
- `WAIT`
- `DUP`
- `SPAWN`
- `KILL`
- `REGISTER`

V0 revocation is simple generation invalidation. Full derivation-tree revocation is deferred, but the handle format and generation model must match the later design.

### 5.6 Principals

V0 supports a minimal principal model:

- `kernel`
- `native_trusted`
- `native_sandboxed`

The principal is assigned at process creation. V0 uses the principal for:

- syscall permission checks;
- timer precision policy placeholder;
- arena quota placeholder;
- service spawn policy.

V0 does not implement human users.

### 5.7 Arenas

V0 implements fixed-size registered arenas.

An arena is:

```text
owner process
base virtual address
size
rights
flags
```

V0 rules:

- writable service memory intended for IPC or restart must live in a registered arena;
- ring buffers are allocated from registered arenas;
- arena registration is checked by the kernel;
- arena ownership is tracked by process;
- arena quota is fixed at process creation.

V0 does not implement growable arenas. V0 does not implement snapshot persistence. The arena system exists so later snapshotting has the correct shape.

### 5.8 IPC

V0 implements the Midix IPC floor shape, but with modest performance expectations.

Required IPC syscalls:

```c
sys_ring_create(size, flags) -> ring_cap
sys_ring_map(ring_cap, target_process_cap, rights) -> mapped_cap
sys_doorbell_create() -> doorbell_cap
sys_notify(doorbell_cap) -> status
sys_wait_doorbell(doorbell_cap, expected_value) -> status
```

V0 ring contract:

- SPSC only.
- Producer head on its own cache line.
- Consumer tail on its own cache line.
- 64-byte slots.
- ring size is power of two.
- producer uses release store for head.
- consumer uses acquire load for head.
- slot contents use relaxed access.

V0 does not implement MPSC, SPMC, MPMC, or `notify_handoff`.

### 5.9 Doorbells

A V0 doorbell contains:

```text
counter
waiter thread pointer or null
owner process
rights
```

Semantics:

- `notify(cap)` increments the counter.
- If a thread is sleeping on the doorbell, it is made runnable.
- `wait_doorbell(cap, expected)` sleeps only if the current counter equals `expected`.
- If the counter differs, it returns immediately.

This gives the futex-shaped race handling required by the architecture.

### 5.10 IRQ and Timer

V0 supports:

- generic timer interrupt;
- UART interrupt if practical;
- polling UART fallback;
- minimal GIC initialisation;
- routing IRQs to kernel handlers;
- waking a thread from timer expiry.

V0 does not expose general IRQ capabilities to arbitrary userspace drivers yet.

### 5.11 UART

V0 has two UART paths:

1. early kernel UART for boot logs and panic output;
2. `io.console` userspace service for normal logs.

Once `io.console` is running, kernel logs may be forwarded to it through a ring. If forwarding fails, the kernel keeps the early UART fallback.

---

## 6. Syscall ABI

V0 uses a small fixed syscall ABI.

### Calling Convention

For AArch64:

```text
x0..x5   arguments
x8       syscall number
svc #0   enter kernel
x0       return value
x1       secondary return / error detail
```

Errors are negative integers.

### V0 Syscalls

Process/thread:

```c
sys_yield()
sys_exit(code)
sys_spawn(image_ptr, image_size, principal, supervisor_cap) -> process_cap
sys_kill(process_cap)
```

Memory:

```c
sys_map_page(page_cap, va, rights)
sys_unmap_page(va)
sys_arena_create(size, flags) -> arena_cap
sys_arena_register(base, size, flags) -> arena_cap
```

Capabilities:

```c
sys_cap_dup(cap, rights_mask) -> new_cap
sys_cap_revoke(cap)
sys_cap_identify(cap) -> type_tag
```

IPC:

```c
sys_ring_create(order, flags) -> ring_cap
sys_ring_share(ring_cap, process_cap, rights) -> ring_cap_for_target
sys_doorbell_create() -> doorbell_cap
sys_doorbell_share(doorbell_cap, process_cap, rights) -> doorbell_cap_for_target
sys_notify(doorbell_cap)
sys_wait_doorbell(doorbell_cap, expected_value)
```

Debug:

```c
sys_debug_puts(ptr, len)
sys_debug_counters(counter_id) -> value
```

V0 support extensions:

```c
sys_cap_grant(process_cap, source_cap, rights) -> cap_for_target
sys_doorbell_read(doorbell_cap) -> counter
sys_supervisor_poll(supervisor_cap) -> x0 dead_pid_or_error, x1 exit_code_or_reason
```

`sys_debug_puts` is allowed in V0 only as a bring-up escape hatch. Normal output should go through `io.console`.

---

## 7. Userspace V0

### 7.1 Init

`init` is the first userspace process.

Responsibilities:

1. receive boot capabilities from the kernel;
2. parse the boot manifest;
3. spawn V0 services;
4. assign principals;
5. pass declared capabilities;
6. register services with nameservice;
7. supervise child service death;
8. restart demo services when configured.

`init` is policy. The kernel does not know service names.

### 7.2 Boot Manifest

V0 manifest is static and line-oriented.

Example:

```text
name image principal restart caps
nameservice nameservice native_trusted never -
io.console io.console native_trusted always uart
demo.pong demo.pong native_sandboxed always -
demo.ping demo.ping native_sandboxed never -
```

The manifest is data, not code.

### 7.3 Nameservice

V0 nameservice maps static service names to capabilities.

Required operations:

```text
register(name, cap)
resolve(name) -> cap
```

V0 nameservice may use a simple fixed array. No persistence. No complex policy. No string namespace hierarchy required.

### 7.4 io.console

`io.console` is the first real userspace service.

Responsibilities:

- own the UART capability;
- receive log messages over a shared ring;
- write messages to UART;
- maintain a small in-memory text buffer;
- expose a console write capability to other services.

V0 console ABI:

```text
console.write(message_ptr, length)
```

Implementation may use either:

- direct syscall-mediated writes during earliest V0, then
- shared-ring writes once IPC is stable.

The final V0 demo should use the shared-ring path.

### 7.5 demo.ping and demo.pong

These prove IPC.

`demo.ping`:

1. resolves `demo.pong` through nameservice;
2. creates or receives a shared ring;
3. sends numbered messages;
4. notifies the pong doorbell if needed;
5. waits for replies;
6. prints latency and success counts to console.

`demo.pong`:

1. waits on its doorbell;
2. drains the ring;
3. writes replies;
4. optionally crashes when asked, to test supervision.

---

## 8. V0 Boot Flow

1. Bootloader or QEMU loads:
   - kernel image;
   - Device Tree blob;
   - initial ram image containing `init`, services, and manifest.
2. Kernel starts at EL1.
3. Kernel sets up early UART.
4. Kernel parses Device Tree.
5. Kernel discovers RAM, timer, GIC, UART.
6. Kernel creates page tables and enables MMU.
7. Kernel initialises exceptions and syscalls.
8. Kernel creates init process.
9. Kernel gives init:
   - manifest cap;
   - boot image cap;
   - root supervisor cap;
   - initial nameservice placeholder cap;
   - UART cap or console bootstrap cap.
10. Kernel enters init at EL0.
11. Init starts nameservice.
12. Init starts io.console.
13. Init starts demo.pong.
14. Init starts demo.ping.
15. demo.ping and demo.pong exchange ring messages.
16. console prints boot-complete marker.

Boot success marker:

```text
hello world
MIDIX V0 BOOT OK
IPC PING/PONG OK
CONSOLE OK
SUPERVISION OK
```

---

## 9. V0 IPC Ring Layout

V0 SPSC ring page layout:

```text
offset  size    field
0x0000  64      producer head cache line
0x0040  64      consumer tail cache line
0x0080  64      flags cache line
0x00c0  64      reserved
0x0100  N*64    slots
```

Producer head line:

```c
struct ring_head_line {
    uint64_t head;
    uint8_t pad[56];
};
```

Consumer tail line:

```c
struct ring_tail_line {
    uint64_t tail;
    uint8_t pad[56];
};
```

Flags line:

```c
struct ring_flags_line {
    uint32_t receiver_about_to_sleep;
    uint32_t reserved0;
    uint64_t doorbell_counter_snapshot;
    uint8_t pad[48];
};
```

Slot:

```c
struct ring_slot {
    uint32_t type;
    uint32_t length;
    uint64_t seq;
    uint8_t payload[48];
};
```

V0 supports inline payloads only. External payload arenas come after V0.

---

## 10. V0 Performance Targets

V0 measures performance but does not promise final architecture numbers.

Required measurements:

- syscall entry/exit cost;
- cap lookup cost;
- same-process ring push/pop cost;
- cross-process ring ping/pong latency;
- `notify` when receiver is awake;
- `notify` when receiver is sleeping;
- timer interrupt latency;
- context switch cost;
- service restart time.

V0 success means the numbers are captured repeatably, not that they hit the final target budgets.

The final architecture’s zero-kernel-cycle IPC claim is considered structurally demonstrated when:

1. producer and consumer share a resident ring;
2. receiver is actively polling or draining;
3. producer publishes without syscall;
4. consumer observes without syscall;
5. kernel counters show no IPC syscall on the steady-state path.

---

## 11. V0 Safety Rules

V0 must preserve these architecture rules from the beginning:

- no ambient authority;
- no global service access without a capability;
- no kernel copies on the IPC hot path;
- no service name hardcoded in the kernel;
- no userspace access to kernel memory;
- no writable executable user pages by default;
- no device access without a capability;
- no hidden privilege attached to process identity except through principal class;
- no scheduler policy embedded in IPC beyond wake/block mechanics.

V0 may be simple, but it must not teach the codebase bad habits.

---

## 12. Development Order

### Phase 0 — Build and Run

Deliverables:

- cross-compiler toolchain;
- linker script;
- QEMU boot command;
- serial output from kernel;
- panic path;
- basic test harness.

Success:

```text
kernel prints hello over UART in QEMU
```

### Phase 1 — Kernel Base

Deliverables:

- exception vectors;
- MMU setup;
- physical page allocator;
- kernel heap;
- Device Tree parser for RAM, UART, timer, GIC;
- timer interrupt.

Success:

```text
kernel receives timer ticks and survives invalid exceptions
```

### Phase 2 — Userspace

Deliverables:

- ELF loader;
- user address space;
- EL0 entry;
- syscall entry/return;
- `sys_debug_puts`;
- `sys_exit`.

Success:

```text
init runs in EL0 and prints through syscall
```

### Phase 3 — Scheduler

Deliverables:

- thread struct;
- process struct;
- run queue;
- timer preemption;
- yield;
- blocking and waking.

Success:

```text
two userspace threads take turns and exit cleanly
```

### Phase 4 — Capabilities

Deliverables:

- per-process cap table;
- handle generation;
- cap lookup;
- cap dup;
- cap revoke;
- basic rights checks.

Success:

```text
userspace can only use objects it has caps for
```

### Phase 5 — Arenas and Shared Pages

Deliverables:

- fixed arena registration;
- shared page object;
- map shared page into two processes;
- rights enforcement.

Success:

```text
two processes observe the same shared memory page by capability only
```

### Phase 6 — Doorbells

Deliverables:

- doorbell object;
- `notify`;
- `wait_doorbell`;
- sleeping receiver wakeup;
- futex-style expected-value semantics.

Success:

```text
one process wakes another without polling
```

### Phase 7 — Rings

Deliverables:

- V0 SPSC ring layout;
- userspace ring library in C or Rust;
- ring create/share/map path;
- ping/pong demo.

Success:

```text
ping and pong exchange 1,000,000 messages with no kernel data copy
```

### Phase 8 — Services

Deliverables:

- boot manifest;
- init service spawning;
- nameservice;
- io.console;
- demo services.

Success:

```text
init starts nameservice, console, ping, and pong from manifest
```

### Phase 9 — Supervision

Deliverables:

- child death notify;
- supervisor cap;
- init restart policy;
- demo crash and restart.

Success:

```text
demo.pong crashes, init restarts it, demo.ping reconnects or reports dependency loss cleanly
```

### Phase 10 — Measurement Harness

Deliverables:

- cycle counter access for trusted code;
- kernel counters;
- IPC benchmark;
- context switch benchmark;
- notify benchmark;
- boot-time report.

Success:

```text
V0 prints repeatable timing data at boot or on command
```

---

## 13. V0 Completion Criteria

V0 is complete when all of the following are true:

1. QEMU ARM64 boots to userspace.
2. `init` runs at EL0.
3. Services are spawned from a manifest.
4. Capabilities are required for kernel objects.
5. A shared ring is mapped between two processes.
6. `demo.ping` and `demo.pong` communicate through the ring.
7. Doorbells wake sleeping receivers.
8. `io.console` prints userspace logs.
9. A demo service crash notifies `init`.
10. `init` restarts the crashed service.
11. Kernel IPC counters show no kernel data copy for ring payloads.
12. A benchmark report is printed.
13. The same kernel image structure can be brought toward real ARM hardware with only platform-driver changes.

---

## 14. Deferred to V1

V1 begins only after V0 is stable.

V1 candidates:

- SMP scheduler.
- cross-CPU wakeups.
- MPSC rings.
- `notify_handoff`.
- priority donation.
- real snapshot capture and restore.
- Linux VM prototype.
- IRQ capabilities for userspace drivers.
- framebuffer service.
- simple filesystem or initrd-backed file service.
- SMMU support.
- driver-VM experiment.

V0 should not absorb these early unless they are needed to unblock ARM bring-up.

---

## 15. V0 Design Principle

V0 must be small enough to finish and strict enough to matter.

A shortcut is acceptable only if it does not violate the future architecture. For example, a simple round-robin scheduler is acceptable. A global ambient device namespace is not. A debug print syscall is acceptable. Kernel-mediated message copying as the main IPC path is not.

The V0 question for every feature is:

```text
Does this prove the Midix kernel contract on ARM?
```

If yes, build the smallest version.
If no, defer it.
