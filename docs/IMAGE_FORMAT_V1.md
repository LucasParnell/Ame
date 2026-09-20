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
- The kernel currently enforces bundle CRC32C, per-object CRC32C, per-object
  SHA-256, signed manifest body SHA-256, and the embedded development trust
  anchor id. The Ed25519 curve verification hook is intentionally isolated in
  the MXM1 cold path and must be completed before non-development boot media.

CRC32C is never an authority mechanism; it is only an accidental-corruption
check. SHA-256 is the object identity mechanism. Manifest policy remains the
authority source for principals and cap grants.

## Boot Path

1. CMake builds each Mochi service as a static ELF.
2. `mxld.py` converts each service to `*.mxi`.
3. `mxpack.py` signs `userland/manifest.v0` into an `MXM1` object and packages
   all services and resources into `build/initrd.mxb`.
4. Anko detects `MXB1` before legacy ustar, validates the bundle, and spawns
   `init` from an `MXI1` image.
5. `init` reads the verified manifest body, verifies service object digests
   before spawning, and publishes `launch.id`, `image.sha256`, and
   `policy.sha256` value entries in each service `MxBootInfo`.

Legacy ustar remains as a development fallback while the migration is underway.
