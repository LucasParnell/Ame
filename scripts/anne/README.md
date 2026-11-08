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

# Explicitly omit the kernel readback after fastboot acknowledges flashing.
scripts/huawei run --skip-readback

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
existing `out/anne-native-build/dev-avb-key.pem`. `--avb-key` overrides it.
Missing keys stop the build before compilation. The pinned AVB tool is
downloaded and hash-verified if needed.

Normal builds require no stock kernel backup. They generate an Android v0
header using the recorded ANE-LX1 load/tag addresses and 2048-byte page size,
gzip the native Image, calculate its legacy checksum, and sign AVB metadata
with rollback index zero. The footer ends 4096 bytes before the 24 MiB partition
end; the reserved final page is zero-filled. The generated header/trailer
variant passes local format/signature checks; phone acceptance has not yet
been checked. It produces no stock restore image.

To preserve a stock header/trailer and include a restore image, explicitly pass
`--backups DIR` containing `kernel.img` and its verified `manifest.json`.
An explicitly supplied corrupt or incomplete backup fails the build.

BootROM runs also need the four private files in
`out/anne-device-private/el2-firmware/`: `xloader.img`,
`stock-lk-el2-tee.bin`, `teeos-decrypted.bin`, and `manifest.json`.
Their expected origins and pinned identities are documented in
[Anne operations](../../boards/huawei-anne/operations.md#flash_el2).

On macOS, BootROM uses its vendor-specific USB bulk interface through libusb;
it does not need to expose a serial port. Linux retains the serial IDT transport.
Both upload the authentication patch after xloader and before the TEE/LK images.

`tools/recover-el2-firmware.py DUMP_DIR OUTPUT_DIR --xloader PATH` reconstructs a separately
hash-pinned LK/TEE preload from the stock `fastboot.img`, encrypted `teeos.img`
and `trustfirmware.img`, and captured `tee-shared-ram.bin`. This is an offline
tool: it performs no device operations and prints no key material. The shared
capture covers the loader-identified region starting at `0x367ff600`, length
`0xa00`. Keep captures and generated firmware private under ignored `out/`.
The recovered plaintexts must match the existing TEE/BL31 hashes; the rebuilt
LK has its own hash because the historical patched binary is unavailable.
The tool validates and copies the signed xloader into the output bundle.
No p20re checkout, imports, or prebuilt patched LK are required for recovery
or runtime. BootROM and factory-fastboot transports live in `anne_transport.py`.
Recovery of the files does not establish successful handset boot.

### macOS checkpoint — 1 November 2026

BootROM USB bulk uploads, the xloader RAM authentication patch, factory
fastboot reads, and kernel flashing were exercised on the connected ANE.
The captured encrypted firmware and loader shared RAM reproduced both existing
plaintext TEE/BL31 hashes. The replacement TEE copy hook passed bounded-write
emulation, and its LK image is separately pinned.

The reconstructed profile has **not passed a phone boot**: both the latest
29 October image and the archived, previously hardware-verified CDC control
image returned to BootROM without an Ame CDC device. A stock-header container
and private RAM-only diagnostic variants did not resolve that result. Retained
LK logs show progress before the kernel handoff, but do not establish the exact
failure instruction or that every final cached log record survived reset.
This does not isolate macOS as the cause and does not invalidate the older
successful route using the missing original patched LK.

Partition readback was explicitly skipped for the initial tests. A subsequent
full 24 MiB eMMC read at `0x1b800000` matched the installed, rebuilt 21 October
`147285d` baseline byte for byte (SHA-256
`0e7703e4ed70b740afe498c4f0aab7b4a5c16d58d4d874dfb5dcc4170e880a24`).
The gzip payload decompressed to the expected raw kernel hash, and the Android
legacy payload checksum was valid. This establishes the baseline partition
write, not a successful boot or correctness of the rebuilt image itself.
Installation records and raw captures remain under `out/anne-device-private/`.
The rebuilt baseline remains installed after the stock EL1 control tests;
`latest.json` still identifies the saved 29 October package and was not changed.

## Implementation and research tools

`cli.py` implements the workflow. `tools/` contains the tested packaging,
transport and serial modules, plus specialized firmware inspection, emulation,
monitor-probe and recovery experiments. They are not alternative build/run
entry points. The old `build-release.sh` and `flash-latest.py` wrappers and the
hard-coded historical PanVK flashing entry point have been removed.

For the experimental DTS loader analysis, `dts-el2-payload.py preflight`
checks an HSDT image's selected row bounds, the loader's 2 MiB inflate limit,
and whether its normalized DTB and verifier proof prefix match the stock
backup. Example:

```sh
python3 scripts/anne/dts-el2-payload.py preflight \
  --image out/anne-device-private/dts-el2/proof-overread-stocktail-v2.img \
  --reference boards/huawei-anne/stock-backups/anne-dts-backup.img
```

The report says `cryptographic_verification: not performed` and
`device_ready: false`; this command is an offline input comparison, not a
signature check or device-write approval. `emulate-dts-loader.py` separately
replays the stock loader and records verifier and heap behavior in Unicorn.
