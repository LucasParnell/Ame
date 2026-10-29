# Huawei workflow

Use one entry point from any working directory:

```sh
scripts/huawei build
scripts/huawei run
scripts/huawei build-run
scripts/huawei console
```

`build` configures a fresh or existing Release build, builds the native kernel,
services, apps and benchmark images, builds the Linux/Mesa GPU appliance, and
creates an AVB-verified kernel package. It never accesses the phone. Linux runs
the guest builds directly; macOS uses the included Docker builder. The first
build reuses `~/Code/linux` when present, or downloads Linux v6.12, and fetches
the pinned Mesa/dependency sources. Build trees are retained for incremental
rebuilds. `-j N` bounds native and guest build jobs. Full GPU builds require at
least 12 GiB of free host disk space; the command checks before starting.

`run` uses the exact package recorded by the last successful build. It never
configures, compiles or repackages. Put the handset into BootROM mode; the
command waits for it, uploads the pinned RAM LK/TEE handoff, verifies the loader,
flashes only the kernel partition, checks its complete readback, and issues
`oem boot`. A verified RAM loader already running is reused. Kernel installation
records remain under `out/anne-device-private/kernel-install-*`.

`build-run` performs those two operations in order. A build, packaging or local
validation failure prevents any phone access. `console` opens the Ame CDC
terminal, with automatic port discovery on Linux and macOS. Ctrl-] exits.

## Options you are likely to need

```sh
# Native USB system, without building or embedding the Linux GPU guest.
scripts/huawei build --native-only

# Build native code around an explicitly supplied matching GPU appliance.
scripts/huawei build --guest-artifacts /path/to/dango-linux-panfrost

# Show the full build plan, without building or touching USB.
scripts/huawei build-run --dry-run

# Validate an existing package and all private firmware locally; no USB access.
scripts/huawei run --dry-run

# Run a specific package, optionally retaining the previous installed kernel.
scripts/huawei run --package /path/to/package --backup

# Send one shell command without opening an interactive terminal.
scripts/huawei console --send 'diag el2'
scripts/huawei console --send 'diag gpu vulkan' --read 60
```

`--wait SECONDS` limits the BootROM wait; the default of zero waits until the
handset appears. More than one matching handset is rejected. `--serial SERIAL`
requires the discovered RAM loader to have that serial. `--firmware DIR`
selects a different copy of the pinned private handoff inputs.

Build output lives under `out/huawei/`: native objects in `build/`, Linux guest
artifacts in `guest/`, and immutable package directories
in `packages/`. `latest.json` is replaced atomically only after successful
packaging and image verification. Other old package directories are never
selected by timestamp. `--build-dir DIR` and `--output DIR` override these paths.
Linux hosts keep Mesa/source caches beside the guest. The macOS Docker builder
keeps those caches in a persistent `ame-huawei-*` Docker volume and exports only
the finished guest artifacts, avoiding host filesystem case/symlink differences.

## Local inputs

The launcher automatically uses `.venv/bin/python3` when available. Install the
Python dependencies there with `pip install -r requirements-huawei.txt`.
Native builds also need CMake, Ninja, LLVM and LLD; macOS GPU builds need a
running Docker daemon. The Docker image supplies the Linux/cross build tools.
On a native Linux host those tools must already be installed; the Dockerfile
lists the required packages.

Release bundle keys default to `~/.ame-keys/release-signing-key.pem` and
`release-trusted-key.pem`. Use the existing release identity, or create one
with `scripts/make-release-key.sh` before the first release build. Override
these with `--signing-key` and `--trusted-key`. `--dev-signing` explicitly
selects the development bundle identity instead.

The AVB key defaults to `~/.ame-keys/anne-avb-key.pem`, falling back to the
existing `out/anne-native-build/dev-avb-key.pem`. `--avb-key` overrides it. Stock
kernel backups and their verified manifest live in
`out/anne-device-private/boot-backups/` (`--backups` overrides this). Missing
keys or backups stop the build before compilation. The pinned AVB tool is
downloaded and hash-verified if needed.

BootROM runs also need the four private files in
`out/anne-device-private/el2-firmware/`: `xloader.img`,
`stock-lk-el2-tee.bin`, `teeos-decrypted.bin`, and `manifest.json`.
Their expected origins and pinned identities are documented in
[Anne operations](../../boards/huawei-anne/operations.md#flash_el2).

## Implementation and research tools

`cli.py` implements the workflow. `tools/` contains the tested packaging,
transport and serial modules, plus specialized firmware inspection, emulation,
monitor-probe and recovery experiments. They are not alternative build/run
entry points. The old `build-release.sh` and `flash-latest.py` wrappers and the
hard-coded historical PanVK flashing entry point have been removed.
