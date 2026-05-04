# Ame Claude Code Rules

These rules are optimized for the Ame repo on Arch Linux. The goal is to keep Claude Code out of generated trees and Linux artifacts and focused on CMake, scripts, kernel, userland, lib, and boot sources.

## Repo scope

Inspect these by default:
- `CMakeLists.txt`
- `boot/CMakeLists.txt`, `kernel/CMakeLists.txt`, `userland/CMakeLists.txt`, `lib/CMakeLists.txt`
- `scripts/build/**`, `scripts/linker/**`, `scripts/run-qemu.sh`
- `kernel/**/*.cc`, `kernel/**/*.h`, `kernel/**/*.S`
- `userland/**/*.cc`, `userland/**/*.h`, `userland/manifest.v0`
- `lib/**/*.cc`, `lib/**/*.h`
- `boot/**/*.{cc,h,S}` only when boot/linker/MMU context is necessary
- `scripts/linux/**` only when the task explicitly mentions Dango/Linux artifacts

Do not read unless explicitly asked:
- `build/**`, `build-*/**`, `out/**`, `.git/**`, `cmake-build-*/**`
- `linux-artifacts/**`
- `docs/**`
- generated logs, binaries, zips, base64 blobs, patch rejects
- `Midix_Architecture_3.md`, `sched_spec_v_2.md`, large spec/architecture markdown

## Token discipline

- Before reading files, state the smallest file set needed.
- Prefer `scripts/claude/ame-files.sh` and `scripts/claude/ame-rg.sh` over raw `find`, `tree`, `grep`, `rg`, `Glob`, or broad reads.
- Read full files only after targeted search shows they are relevant.
- Never paste full CMake, Ninja, QEMU, or guest logs.
- For build failures, include only failing commands, compiler/linker errors, panic/fault lines, and the final summary.
- Use focused CMake/Ninja targets when possible: `anko`, `mochi_initrd`, or one userland service.
- Stop after the requested fix and focused validation. No drive-by refactors.
- Use `/compact` after exploratory debugging. Preserve only: goal, touched files, current hypothesis, exact errors, and next command.

## Default Ame commands

- List relevant source/config files: `scripts/claude/ame-files.sh`
- Search relevant source/config only: `scripts/claude/ame-rg.sh <pattern>`
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake`
- Build tersely: `scripts/claude/ame-build.sh`
- Build target tersely: `scripts/claude/ame-build.sh anko`
- Run tersely: `scripts/claude/ame-run.sh`
- Read guest log tersely: `scripts/claude/ame-log.sh build/dango-guest.log`
- Audit context sinks: `scripts/claude/ame-context-audit.sh`

## Optional token tools

- If RTK is installed, prefer RTK for raw noisy shell commands.
- If Squeez is installed, do not also use RTK on the same command until tested.
- If Context Mode is installed, use it for commands/searches likely to produce huge output.
- If Serena is installed, prefer symbol navigation before broad file reads.
- Do not install random Claude plugin packs globally. Project-scoped installs only.

## Output style

Be terse. No praise, no tutorial, no recap unless asked. For each turn return only:
1. files touched or inspected
2. command(s) run
3. result
4. next blocker, if any
