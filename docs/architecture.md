# Midix Architecture

Canonical spec: `Midix_Architecture_3.md`, `midix_v_0_arm_target.md`.

## Component names

| Name  | Role |
|-------|------|
| Anko  | Microkernel (caps, IPC, arenas, scheduler) |
| Mochi | Userland (init, services, drivers-as-processes) |
| Midix | The overall system / specification name |

## Target

- **ISA**: AArch64 (ARMv8-A minimum)
- **ELs**: EL0 (Mochi), EL1 (Anko), EL2 (Linux VM appliance — future)
- **Pages**: 4 KB, 48-bit VA (TTBR0 = user, TTBR1 = kernel)
- **Test platform**: QEMU `virt` (`-cpu cortex-a57`)
- **First real hardware**: PinePhone-class AArch64 board

## Language

C++23, freestanding. No exceptions, no RTTI, no host libc.
Runtime: `lib/libk` — string, printf, panic.

## Anko kernel responsibilities

| Subsystem | What it does |
|-----------|--------------|
| `cap/`    | Issue, look up, revoke capabilities |
| `ipc/`    | Shared-memory ring management, doorbell notify/wait syscalls |
| `mm/`     | Arena create/validate, address-space management, slab |
| `sched/`  | Per-CPU run queues, priority scheduler, work stealing (time-share only) |
| `drivers/`| GIC, generic timer, early UART (minimal in-kernel set) |
| `vm/`     | Linux VM appliance host (future) |

## IPC tiers

| Tier | Mechanism | Kernel cost |
|------|-----------|-------------|
| 1 | Shared-memory ring, receiver polling | 0 kernel cycles on steady state |
| 2 | Ring + doorbell wakeup | ~100–200 cycles |
| 3 | Synchronous cold-path | Correctness fallback, unoptimised |

Ring ordering: producer fills slot with relaxed stores, publishes head with
`STLR`; consumer reads head with `LDAR`, reads slot with relaxed loads.

## Memory arenas

All writable memory in snapshot-capable Mochi services must live in
kernel-registered arenas. Registration O(1); snapshot validation O(A).

## Scheduling

- Per-CPU run queues; O(log n) priority, O(1) same-priority FIFO
- Cross-CPU wakeup: lock-free MPSC push + IPI; no cross-CPU locks
- Work stealing: time-shared class only; RT / near-RT / affinity-pinned exempt

## Code layout

```
lib/libk            Freestanding utilities (used by boot and Anko)
boot/               Bootloader
  entry/              _start + boot_main
  dtb/                FDT parser
  memory/             Phys map + bump allocator
  mmu/                Page tables + MMU enable trampoline
kernel/             Anko
  arch/arm64/         Exception vectors, context switch
  cap/                Capability table
  ipc/                Rings + doorbells
  mm/                 Arena, slab, VM space
  sched/              Scheduler, per-CPU state
  drivers/            GIC, timer, UART
  vm/                 Linux appliance (future)
userland/           Mochi
  init/               First process
  console/            Console/log service
```
