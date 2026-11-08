# AmeFS application packages

Applications use `mochi::app::Process`, ordinary sandbox policy, stdout and
supervision. A `.amp` file groups an MXI executable with immutable resources.
Store it on AmeFS, conventionally `/data/apps/NAME.amp`, and launch with
`exec /data/apps/NAME.amp`. Existing bare MXI files still work.

Build the example explicitly:

```sh
cmake --build out/qemu-dev --target app_package_demo
```

The result is `out/qemu-dev/userland/package-demo.amp`. Package other apps with:

```sh
.venv/bin/python scripts/mx/mkapppackage.py \
  --executable out/qemu-dev/userland/app.package-demo.mxi \
  --resource ui/label.txt=userland/app/package-demo/label.txt \
  --output /tmp/example.amp
```

Explicitly prepopulate a **new** AmeFS image (choose a new output path):

```sh
.venv/bin/python scripts/mx/mkamefs.py create \
  --build-dir out/qemu-dev --output /tmp/ame-ui.img --size 128M \
  --app package-demo.amp=out/qemu-dev/userland/package-demo.amp
```

`--app NAME=FILE` is repeatable and also accepts bare executables. It is offline
image provisioning, not an installer or an atomic update mechanism for a live
filesystem. No new application is included in the boot manifest or started at
boot.

Provisioned `/data` entries use the mutable store's reserved inode range and
parent metadata, so they remain visible through the normal `/data` mount.

In a native app, call `mochi::app::start().resource("ui/label.txt")` to obtain
`midix::package::Bytes` (`data`, `size`, boolean presence). Empty resources are
present with size zero; missing resources have a null pointer. Resource bytes
remain mapped read-only until process exit. `Context::self_image()` still
returns the executable alone, suitable for existing worker-spawn code. A
worker spawned from that executable alone does not inherit package resources;
pass the complete package to `Process::spawn()` when resources are required.

The launch owner copies the complete package into the existing child-owned
`app.image.*` snapshots before releasing its upload arenas. The kernel receives
only the embedded executable. `SpawnOptions::self_image = false` disables these
snapshots and resource access as it does executable self-access. Applications
receive no additional filesystem or graphics authority from package metadata.
Low-level `map_image()` maps the complete startup snapshot; use Context APIs
when an executable-only view is needed.

The compiler will put generated C++ in the executable and package metadata,
shader binaries, fonts and images as resources. V1 treats resources as opaque
bytes; resource-specific decoders must validate their own input. The package
version describes the container, not a shader or UI resource schema. Those
consumers must version their own schemas and cache keys.

V1 wire format is little-endian with no implicit structure packing:

| Offset | Field |
| --- | --- |
| 0 | Eight magic bytes `AMEAPP1\0` |
| 8 | u32 version, 1 |
| 12 | u32 entry count, 1..64 |
| 16 | u64 total bytes, at most 8 MiB including executable and resources |
| 24 | u64 reserved, zero |
| 32 | Entry table, 80 bytes per entry |

Each entry has a 64-byte zero-padded name, u64 absolute byte offset and u64
length. Entry zero is nonempty `app.mxi`; subsequent names are unique. Payloads
are contiguous in table order immediately after the table, with no gaps,
overlap or trailing bytes. Names contain 1..63 ASCII bytes, slash-separated
components using letters, digits, underscore, hyphen and dots; no empty or
dot-prefixed components. The packer sorts resources for reproducible output.
The loader validates the embedded MXI using the existing image validator.

This container adds no signature or publisher authentication. AmeFS checksums
and MXI validation retain their existing roles. A future signed application
distribution policy must be explicit; resource names and extensions convey no
trust.
