# Midix

Capability-based microkernel OS for AArch64.

| Component | Name  | Role |
|-----------|-------|------|
| Kernel    | Anko  | Microkernel: caps, IPC rings, arenas, scheduler |
| Userland  | Mochi | Services, init, nameservice, io.console, demos |
| Spec      | Midix | Architecture document (see `Midix_Architecture_3.md`) |

## V0 milestone

Boot → DTB parse → create `init` → capabilities → shared-ring IPC → console
service → crash recovery of a demo service.

Live implementation tracker: `docs/v0-progress.md`

## Boot sequence

```
QEMU virt
    └── loads anko ELF directly with -kernel (x0 = DTB phys)
            └── anko _start
                    ├── verify/drop to EL1
                    ├── set stack + clear BSS
                    └── kernel_main()
```

## Repository layout

```
boot/           Optional boot shim for later real-hardware work (not V0-critical)
  entry/          _start (ASM) + boot_main (C++)
  dtb/            FDT/DTB parser
  memory/         Physical memory map + early bump allocator
  mmu/            Page-table builder + MMU enable trampoline

kernel/         Anko microkernel
  arch/arm64/     Exception vectors, context switch
  cap/            Capability table, issuance, revocation
  ipc/            Shared-memory rings + doorbell notify/wait
  mm/             Arena allocator, slab, VM space management
  sched/          Per-CPU run queues, work stealing
  drivers/        GIC, generic timer, early UART

userland/       Mochi userland
  init/           First process; starts all services
  console/        Console service (UART-backed log output)
  ...

lib/            Freestanding runtime (no host libc)
  libk/           string, printf, panic

scripts/
  linker/         boot.ld, kernel.ld
  build/          CMake toolchain files

docs/           Design notes
tests/          Host-side unit tests
```

## Toolchain

- Preferred: `aarch64-none-elf-g++` / `aarch64-none-elf-gcc`
- Fallback: `clang` / `clang++` with `--target=aarch64-none-elf`
- Build system: CMake with `scripts/build/toolchain-aarch64.cmake`
- Emulation: `qemu-system-aarch64 -machine virt -cpu cortex-a57`

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake
cmake --build build
```

## Running (QEMU)

```sh
scripts/run-qemu.sh
```
