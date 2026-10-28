# Ame programmer ABI and packaging

The current system contract is summarized in [architecture](architecture.md). Shared headers and the descriptor parser are authoritative for exact values. These references cover formats and cold control operations; historical migration TODOs have been removed.

- [Ame Programmer ABI v0](#programmer_abi_v0)
- [Ame Native Image Format V1](#image_format_v1)
- [System descriptor](#system_descriptor)

<a id="programmer_abi_v0"></a>
## Ame Programmer ABI v0

<a id="programmer_abi_v0-midix-programmer-abi-v0"></a>

This document describes the code implemented in this archive for the programmer ABI layer. It is intentionally small: the kernel ABI stays C-shaped, the pleasant API lives in `libmochi`, and all high-frequency IPC remains shared-memory first.

<a id="programmer_abi_v0-api-shape"></a>
### API shape

<a id="programmer_abi_v0-kernel-abi"></a>
#### Kernel ABI

The stable scalar layer uses:

```cpp
using mx_handle_t = uint64_t;
using mx_status_t = int64_t;
using mx_rights_t = uint32_t;
```

Handle-producing calls should prefer out-parameter wrappers:

```cpp
mx_handle_t ring = MX_NULL_HANDLE;
mx_status_t st = mx_ring_create(MX_DEFAULT_RING_ORDER, 0, &ring);
```

This avoids the old ambiguity where a raw `int64_t` was sometimes a status and sometimes a handle.

<a id="programmer_abi_v0-capabilities"></a>
#### Capabilities

Use `MX_CAP_*` and `MX_RIGHT_*` from `midix/abi/cap.h`. Existing V0 code can still use `RIGHT_READ`, `RIGHT_WRITE`, and `kCapTypeSharedRing` compatibility names, but new services should use the namespaced `MX_` constants.

<a id="programmer_abi_v0-ipc"></a>
#### IPC

The normal programmer API is non-blocking by default:

```cpp
mochi::SpscEndpoint ep{tx, rx, order, tx_db, rx_db};
bool sent = ep.try_send(type, data, len);
bool got = ep.try_recv(&slot);
```

The only APIs that may sleep are named explicitly:

```cpp
ep.wait_recv(&slot);
ring_wait_pop(...);
mx_doorbell_wait(...);
```

`ring_try_push` never waits for space. It only enters the kernel if the receiver has set `receiver_about_to_sleep`, which preserves the Ame hot path: shared-memory publication first, kernel doorbell only for sleep/wake.

<a id="programmer_abi_v0-endpoint-descriptors"></a>
#### Endpoint descriptors

Nameservice now has a common return shape:

```cpp
struct MxEndpointDesc {
    uint32_t magic;
    uint16_t version;
    uint16_t handle_count;
    uint32_t protocol;
    uint32_t flags;
    mx_handle_t handles[4];
};
```

It fits exactly into one 48-byte ring payload.

<a id="programmer_abi_v0-bootinfo"></a>
#### BootInfo

`midix/abi/boot.h` defines a generic `MxBootInfo` page. Named boot entries carry service resources, values and generation/image/policy identity. Early console bootstrap retains its specialized configuration.

<a id="programmer_abi_v0-security-and-progress-rules"></a>
### Security and progress rules

1. Default APIs are try-style and do not block the kernel.
2. Blocking is opt-in and named.
3. Full-ring backpressure is returned to userspace as `false`/status rather than hidden in the kernel.
4. Cap rights are explicit and come from one shared ABI header.
5. Service endpoints are bundles, not ambient global objects.
6. Terminal input is provisioned as a separate channel in `ConsoleBootConfig`; it is not mixed into kernel/service log rings.

<a id="programmer_abi_v0-native-driver-support-extensions"></a>
### Native driver support extensions

`MX_NR_WAIT_DOORBELL_UNTIL` (67) takes a waited doorbell, its expected counter
and an absolute monotonic deadline. Counter mismatch, notify and expiry all
return `MX_OK`; consumers recheck their rings/hardware and clock. A zero deadline
retains the untimed wait contract. Timer records are bounded per CPU and retain
thread/doorbell lifetimes; a wait sequence prevents an old expiry waking a later
wait. Expiry never overwrites another syscall's saved return register.

`MX_CAP_PLATFORM` (22) and `MX_NR_PLATFORM_CONTROL` (68) expose cold, fixed board
mechanisms. The caller first claims one domain with its process incarnation.
The capability permits only that domain's enumerated operations and reset masks;
arguments cannot specify arbitrary physical addresses. IRQ routing, PIO register
access, role policy and USB classes retain their separate contracts. A controller
owner exit fences hardware before arena/capability reclamation.

System-domain operations include `MX_PLATFORM_SYSTEM_OFF` (10),
`MX_PLATFORM_SYSTEM_RESET` (11), `MX_PLATFORM_SYSTEM_FASTBOOT` (12), and the
read-only `MX_PLATFORM_SYSTEM_CHECK` (13). CHECK takes a requested power operation
as its argument and returns a status; transition operations require argument
zero. Init checks before service teardown. Unsupported modes return
`MX_ERR_NOT_IMPLEMENTED`; refused transitions return an error with host interrupt
state restored. Successful transitions do not return. Firmware calls and
board-specific PMIC/GPIO/NMI accesses remain in the kernel.

`MX_PLATFORM_I2C_PINS` (9) selects the fixed Type-C bus pad function
(GPIO_037/038 -> I2C4) and pad-config defaults, then reports pad and
clock/reset readbacks to the kernel log; it takes no arguments and is
idempotent.

Signed manifest bindings can name a declared hardware boot resource (`boot`) or
an inline configuration value (`value`) as alternatives to a channel. Init grants
only rights derived from its actual capability. The descriptor admits 32 service
definitions and 128 channels; a selected profile remains bounded by the bounded
service registry. Session directories admit eight clients and 32 exports.

<a id="programmer_abi_v0-bring-up-diagnostic-reads"></a>
### Bring-up diagnostic reads

`MX_NR_DEBUG_READ` (69), wrapped by `sys_debug_read(request, index, out, size)`,
copies read-only observations into a writable user buffer and returns the byte
count or a negative status. Layouts and request IDs are in
[`midix/abi/diagnostics.h`](../shared/include/midix/abi/diagnostics.h).

- `MX_DIAG_SYSTEM`, index zero: a versioned `MxDiagSystem` with the sampled CPU's
  EL1-accessible registers, board, online mask, effective timer rate and retained
  log length/capacity. These are observations, not firmware calls.
- `MX_DIAG_CPU`, index a logical CPU: a versioned `MxDiagCpu` with boot entry EL,
  installed monitor readiness and scheduler/timer counters. Offline slots are
  readable; entry level zero means no entry was recorded.
- `MX_DIAG_BOOT_LOG`, index a byte offset: a non-consuming retained-log read of
  up to 256 bytes. Zero means EOF; boards without retention return
  `MX_ERR_NOT_IMPLEMENTED` and advertise capacity zero.

Every request is bounded to 256 output bytes. Fixed records require enough space
for the whole structure. Invalid sizes/indices fail with `MX_ERR_INVALID`;
unwritable user ranges fail with `MX_ERR_FAULT`. Log bytes are copied under the
existing writer lock into kernel scratch storage before copying to userspace.
The shell snapshots the log length before emitting its report so concurrent log
growth cannot extend a capture indefinitely. Live counters are independent
samples. This extends the existing bring-up debug interface; it grants no
hardware-control capability.

<a id="image_format_v1"></a>
## Ame Native Image Format V1

<a id="image_format_v1-midix-native-image-format-v1"></a>

V1 introduces two runtime formats:

- `MXI1`: native executable/library image. Build inputs remain ELF objects and
  archives; `scripts/mx/mxld.py` converts final static AArch64 ELF services into
  `MXI1`.
- `MXB1`: signed boot/application bundle. `scripts/mx/mxpack.py` packages
  `MXI1` services, libraries, manifests, and resources into one container.

The stable ABI structs live in `shared/include/midix/abi/image.h`.

<a id="image_format_v1-integrity-model"></a>
### Integrity Model

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

<a id="image_format_v1-boot-path"></a>
### Boot Path

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

<a id="system_descriptor"></a>
## System descriptor

<a id="system_descriptor-system-descriptor"></a>

`userland/system.json` is the source for the board service profiles. Edit
it to change service selection, dependencies, restart policy, CPU placement,
resource aliases, channel geometry, or fixed channel grants. `profiles` lists
services in each board's boot profile; omitted services are not packaged. Checks
are filtered to that selection.

Each service's `bindings` names a channel, its child-visible boot name, and the
rights to grant. Ring rights are `read`, `write`, `map`, and `duplicate`;
doorbell rights are `wait`, `notify`, and `duplicate`. They are explicit because
a consumer that may wait is not automatically entitled to notify or delegate.
`affinity` is a preferred CPU index; init falls back to CPU 0 when it is offline.
Omitting affinity preserves the scheduler's default placement. `endpoints` lists
pairs that share one geometry; their ring orders must match. Configurable ring
mapping windows reserve the maximum supported ring size, including the header.

The packer validates policy with `scripts/mx/system_descriptor.py`, adds image
hashes, and signs the resulting `manifest.json` in `system.mxb`. The same module
backs `check-caps.py`. Duplicate JSON keys, unknown references, dependency cycles,
oversized names/tables, invalid rights, and invalid CPU/ring values fail the build.
CMake tracks descriptor changes for both init and the signed bundle.

Generated `system_channels.h` supplies channel identifiers and kinds to init;
it contains no grants or ring sizing. Init validates the signed manifest again,
then stores bindings in fixed arrays. Channel lookup is performed during policy
loading; restart and supervision use indexed handles. There is no JSON parsing
or allocation on the IPC data path.

`build/userland/manifest.v0` is a generated compatibility listing installed
alongside the signed JSON. It is not a second policy input. Disk installation
requires this build output so a Switch install cannot fall back to a QEMU list.

The descriptor governs fixed channel wiring. Device discovery, optional GPU
assignment, console configuration, session creation, and capability lifetime
remain init mechanisms. Resource aliases document those bootstrap roles; they
are not a claim that all device grants are dynamically interpreted yet. The GPU
service remains hosted by `dango.manager`, with its conditional endpoint unchanged.
