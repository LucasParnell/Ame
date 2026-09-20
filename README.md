# Ame

Ame is an experimental AArch64 operating system with the Anko microkernel,
Mochi userland services, and Dango virtual machine support. Internal `midix`
and `mx` names remain in the source and ABI.

The development-readiness cleanup is in progress. See
[the implementation checkpoint](docs/CLEANUP_STATUS.md) for completed checks
and remaining work. Architecture and older protocol documents include design
targets that are not yet implemented.

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

Release builds require an Ed25519 private key in PKCS8 format and explicitly
configured trusted public keys (PEM or raw 32-byte files). Keep private keys
outside the repository. The release configuration rejects the development key.

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
CMake target and QEMU firmware with `scripts/build-u-boot-qemu-arm64.sh`.

Switch packages can be staged without writing an SD card:

```sh
cmake --preset switch
scripts/build-hekate-package.sh --build-dir out/switch --no-configure
```

Packaging checks the kernel's linked entry point and ARM64 image header against
board configuration. Package validation does not constitute physical-device
testing.

## Source map and limitations

- `kernel/`: capabilities, scheduling, memory, IPC, platform boot, and VMs.
- `shared/`: ABI definitions and bounded image, bundle, and integrity routines.
- `userland/`: init, discovery, console, polling drivers, filesystem, and shell.
- `scripts/mx/`: native image conversion, manifests, signing, and packaging.
- `tests/`: host sanitizer tests and QEMU kernel lifecycle fixtures.
- `third_party/monocypher/`: pinned signature implementation and provenance.

Normal service supervision, per-client IPC sessions, filesystem mount semantics,
and application interfaces still need cleanup. Existing writable filesystem
state is volatile; a successful `sync` is not proof of persistence. Hardware IRQ
bind/ack operations return not implemented. Durable storage, hardware IRQ routing,
and physical Switch acceptance remain later milestones.
