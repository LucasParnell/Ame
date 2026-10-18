# Ame

Ame is an experimental AArch64 operating system with the Anko microkernel,
Mochi userland services, and Dango virtual machine support. Internal `midix`
and `mx` names remain in the source and ABI.

See [feature status](docs/FEATURE_STATUS.md) for implementation coverage and
[the architecture specification](docs/architecture.md) for the design. Architecture
and older protocol documents include targets that are not yet implemented.

## Build

Host dependencies: CMake 3.25 or newer, Ninja, Python 3, Clang/LLD and LLVM
tools (or an AArch64 bare-metal GCC toolchain). Host regression tests also
require Clang with AddressSanitizer and UndefinedBehaviorSanitizer. QEMU tests
require `qemu-system-aarch64`. Disk packaging additionally uses `dtc`, mtools,
and FAT formatting tools; Switch staging requires `dtc`.

Install the Python signing dependency in a virtual environment, then configure:

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements-build.txt
cmake --preset qemu-dev
cmake --build --preset qemu-dev --target anko mochi_system
ctest --preset qemu-dev
```

Linux guest construction is optional and disabled in the presets. Enable
`DANGO_BUILD_LINUX_ARTIFACTS` explicitly when preparing guest regression runs.

### Build targets

An ordinary build (`cmake --build --preset qemu-dev`) builds everything; the
targets below are the ones worth knowing by name.

| Target             | Produces |
|--------------------|----------|
| `anko`             | The microkernel ELF and `anko.bin` raw image |
| `mochi_system`     | The Mochi services plus the signed `system.mxb` bundle and `manifest.json` |
| `kernel_regressions` | `lifecycle.mxb` (dev builds only), the kernel lifecycle fixture |
| `ame_vfs`          | A QEMU virtual-filesystem image (`fs.img`) with the built Ame tree under `/ame` |
| `ame_disk`         | A U-Boot/QEMU raw-boot + AmeFS disk image for the disk boot path |
| `provenance`       | `generated/provenance.json` and `generated/provenance.h` (see below) |

The packaging targets share the same sources as `anko` and `mochi_system` and
forward the build directory to their scripts, so they can be built on their own:

```sh
cmake --build out/qemu-dev --target ame_vfs
cmake --build out/qemu-dev --target ame_disk
```

### Build provenance

Every build emits a machine-readable provenance record next to the other
generated headers:

```
out/qemu-dev/generated/provenance.json   # canonical record
out/qemu-dev/generated/provenance.h      # C++ view (ame::provenance::…)
```

It captures the source revision (`git describe`, commit, branch, dirty flag),
the configuration (build type, board, dev/release mode), the signing identity
(trust mode, key fingerprint, trusted-key count), and the sha256 of the
produced `system.mxb`. Generation is best-effort and data-oriented: when `git`
is unavailable every revision field is recorded as `null` rather than failing
the build, and identical content is not rewritten, so a no-op rebuild stays a
no-op.

For a single end-to-end health check — configure, build, host regressions, a
headless direct boot, and the kernel lifecycle matrix — run:

```sh
scripts/verify.sh
```

It records the exact toolchain it validated (Homebrew Clang 23.1.1, CMake
4.4.0, QEMU 11.1.1 at the last pass) and exits non-zero on the first failure.
`AME_VERIFY_BUILD=out/qemu-dev scripts/verify.sh` reuses an existing tree and
`AME_VERIFY_CPUS="2" scripts/verify.sh` narrows the CPU matrix.

Release builds require an Ed25519 private key in PKCS8 format and explicitly
configured trusted public keys (PEM or raw 32-byte files). Keep private keys
outside the repository. The release configuration rejects the development key.

Generate a keypair with:

```sh
scripts/make-release-key.sh ~/.ame-keys
cmake --preset qemu-release \
  -DAME_SIGNING_KEY=$HOME/.ame-keys/release-signing-key.pem \
  -DAME_TRUSTED_KEY_FILES=$HOME/.ame-keys/release-trusted-key.pem
```

A release configure without a key fails closed by design.

```sh
cmake --preset qemu-release \
  -DAME_SIGNING_KEY=/path/outside/repository/signing-key.pem \
  -DAME_TRUSTED_KEY_FILES=/path/to/trusted-public-key.pem
cmake --build --preset qemu-release --target anko mochi_system
```

## Run and test

The supported kernel inputs are a raw ARM64 kernel with an external signed MXB
bundle, or a U-Boot disk boot. Runtime ELF, tar, and embedded-userland fallbacks
have been removed from the kernel. Rebuild all services and regenerate the
bundle together after ABI changes.

```sh
python3 scripts/test/boot.py --build out/qemu-dev --mode direct --cpus 2
cmake --build out/qemu-dev --target kernel_regressions
python3 scripts/test/boot.py --build out/qemu-dev --mode direct --cpus 4 \
  --bundle out/qemu-dev/lifecycle.mxb --marker 'lifecycle regression passed'
```

The runner saves host and guest serial logs plus a JSON result, enforces a
deadline, and uses a QEMU disk snapshot when a disk is supplied. For disk boot,
pass `--mode disk --disk PATH --firmware PATH`. Build a disk with the `ame_disk`
CMake target, a virtual filesystem image with the `ame_vfs` target, and QEMU
firmware with `scripts/build-u-boot-qemu-arm64.sh`.

Switch packages can be staged without writing an SD card:

```sh
cmake --preset switch
scripts/build-hekate-package.sh --build-dir out/switch --no-configure
```

Packaging checks the kernel's linked entry point and ARM64 image header against
board configuration. Package validation does not constitute physical-device
testing.

The Switch package uses `boot.scr` and legacy `bootm` images: the Switchroot
U-Boot inspected on the test SD card has neither `sysboot` nor `booti`, so an
extlinux entry naming the raw `Image` cannot launch Ame. Install with
`scripts/build-hekate-package.sh --build-dir out/switch --no-configure --no-build /Volumes/SD_NAME`
to retire the old Ame extlinux entry as well. Keep `bl31.bin` and `bl33.bin` in
`ame/l4t/`, and Hekate's shared SC7/BPMP firmware in `bootloader/sys/l4t/`.
See [Switch boot investigation](docs/SWITCH_BOOT.md) for the evidence and remaining
hardware check.
The installer validates BL31's linked address against the board's Hekate
firmware profile. To build a matching secure monitor from pinned Switchroot
source, run `scripts/build-switch-atf.sh`, then pass
`--bl31 build-switch/atf/bl31.bin` to the package command. This replaces BL31
only for Ame; it also generates the DTB firmware reservations from that profile.
For a freeze before any Ame output, add `--handoff-probe` when building and
installing to include a separate **More Configs → Ame handoff probe** entry.
It deliberately stops on magenta if the ATF-to-BL33 handoff succeeds and the
active display can be written. See the investigation notes for interpretation.

## Source map and limitations

- `kernel/`: capabilities, scheduling, memory, IPC, platform boot, and VMs.
- `shared/`: ABI definitions and bounded image, bundle, and integrity routines.
- `userland/`: init, discovery, console, polling drivers, filesystem, and shell.
- `scripts/mx/`: native image conversion, manifests, signing, and packaging.
- `scripts/provenance.py`: emits the per-build provenance record (see above).
- `tests/`: host sanitizer tests and QEMU kernel lifecycle fixtures.
- `third_party/monocypher/`: pinned signature implementation and provenance.

Normal service supervision, per-client IPC sessions, filesystem mount semantics,
and application interfaces still need cleanup. `/data` uses disk-resident keyed
B+trees, full-volume bitmaps, CoW data and a metadata redo journal. The P20 Lite
uses its existing 50.6 GiB userdata partition through an 8-bit ADMA2 eMMC driver.
`scripts/test/durable-persistence.py` verifies a 3 MiB file across two QEMU boots
and checks immutable read-arena lifetimes. `/tmp` remains volatile; packaged root
handles remain read-only. Without durable backing, `FS_SYNC` reports
not-implemented. Disk snapshots, hardware IRQ bind/ack, and physical Switch
acceptance remain later milestones. See [FS_SPEC](docs/FS_SPEC.md) for the
implemented protocol, journal extensions and bounded working sets.
