# Midix Native Image Format V1

V1 introduces two runtime formats:

- `MXI1`: native executable/library image. Build inputs remain ELF objects and
  archives; `scripts/mx/mxld.py` converts final static AArch64 ELF services into
  `MXI1`.
- `MXB1`: signed boot/application bundle. `scripts/mx/mxpack.py` packages
  `MXI1` services, libraries, manifests, and resources into one container.

The stable ABI structs live in `shared/include/midix/abi/image.h`.

## Integrity Model

- Every bundle object carries SHA-256 and CRC32C.
- The bundle carries a whole-bundle CRC32C for fast corruption detection.
- The boot manifest is wrapped in an `MXM1` signed-manifest envelope with an
  Ed25519 signature, SHA-256 body digest, and public-key id.
- The kernel enforces bundle CRC32C, per-object CRC32C, per-object SHA-256,
  the signed manifest body SHA-256, and an Ed25519 signature over the manifest
  body checked against the configured trust store
  (`generated/manifest_trust.h`). The development seed key is the default only
  for development builds; release builds require explicitly configured trusted
  public keys and reject the development key.

CRC32C is never an authority mechanism; it is only an accidental-corruption
check. SHA-256 is the object identity mechanism. Manifest policy remains the
authority source for principals and cap grants.

## Boot Path

1. CMake builds each Mochi service as a static ELF.
2. `mxld.py` converts each service to `*.mxi`.
3. `mxpack.py`, driven by `mksystem.py`, signs the boot manifest into an `MXM1`
   object and packages all services and resources into `system.mxb` (with the
   JSON boot manifest written alongside as `manifest.json`).
4. Anko validates the `MXB1` bundle (from the kernel command line or the disk
   boot path) and spawns `init` from an `MXI1` image.
5. `init` reads the verified manifest body, verifies service object digests
   before spawning, and publishes `launch.id`, `image.sha256`, and
   `policy.sha256` value entries in each service `MxBootInfo`.

Runtime ELF, tar, and embedded-userland fallbacks have been removed; the
signed `MXB1` bundle is the sole boot archive format.
