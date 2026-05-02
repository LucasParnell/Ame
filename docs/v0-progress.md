# Midix V0 Live Progress Tracker

This file is the implementation-time tracker for `midix_v_0_arm_target.md`.
Check boxes are only marked done when code or docs in this repo implement the
spec item.

## Build and Toolchain

- [x] AArch64 freestanding toolchain works (`aarch64-none-elf-*` or clang fallback)
- [x] `cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake` configures
- [x] `cmake --build build` builds kernel
- [x] `cmake --build build` builds Mochi userland ELFs
- [x] `scripts/run-qemu.sh` boots direct with `build/kernel/anko`

## Kernel (Anko)

- [x] Entry/EL setup + early UART
- [x] Exception vectors installed at `VBAR_EL1`
- [x] Lower-EL IRQ path saves a trap frame and returns through scheduler handoff
- [x] V0 MMU enabled with per-process TTBR0 page tables and EL0 permissions
- [x] GICv2 + generic timer initialize and produce timer ticks
- [x] SVC path (`syscall_entry` -> `syscall_dispatch`)
- [x] Basic scheduler (`yield`, `block`, `wake`, current-thread tracking)
- [x] Capability table (`alloc`, `lookup`, `dup`, `revoke`)
- [x] Arena registration primitives
- [x] Doorbell object (`create`, `notify`, `wait`)
- [x] Ring object (`create`, `share`) with V0 layout
- [x] `init` process creation path
- [x] Boot-cap issuance (manifest/initrd/supervisor/nameservice/UART)
- [x] `SYS_SPAWN` accepts image bytes from init instead of kernel service names
- [x] `SYS_MAP_PAGE` maps caps at explicit user VAs and returns status
- [x] `SYS_SUPERVISOR_POLL` reports a V0 child-death event to init

## Userspace (Mochi)

- [x] `init` builds and uses syscall ABI
- [x] `init` parses `manifest.v0` line entries and spawns services from initrd image bytes
- [ ] `nameservice` fixed-table register/resolve flow
- [x] `demo.ping` resolves `demo.pong` through the V0 nameservice resolve path
- [x] `io.console` service prints `hello world` first
- [x] `demo.ping` and `demo.pong` use ring + doorbell ABI
- [x] Supervision behavior (`demo.pong` crash/restart path)

## Acceptance Markers

- [x] `hello world`
- [x] `CONSOLE OK`
- [x] `IPC PING/PONG OK`
- [x] `SUPERVISION OK`
- [x] `MIDIX V0 BOOT OK`

## Notes

- V0 target path is direct QEMU `-kernel build/kernel/anko`.
- `boot/` shim remains out of the critical path for V0.
- Acceptance path boots real Mochi services from the boot archive/embedded payload.
- `kernel/arch/arm64/user_stub.S` is no longer part of the acceptance build path.
- IPC success is emitted by `demo.ping` after completing the ping/pong loop, not by init after wiring.
- Current ring demo uses cooperative yield-on-empty while the dedicated blocking doorbell path is hardened.
- Timer ticks are visible in the boot metrics as `METRIC TIMER_TICKS`.
- Remaining work is tracked as unchecked items above and in `docs/v0-phases.md`.
