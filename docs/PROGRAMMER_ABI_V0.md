# Midix Programmer ABI v0

This document describes the code implemented in this archive for the first stable user/programmer ABI layer. It is intentionally small: the kernel ABI stays C-shaped, the pleasant API lives in `libmochi`, and all high-frequency IPC remains shared-memory first.

## Files added

- `shared/include/midix/abi/types.h`
- `shared/include/midix/abi/status.h`
- `shared/include/midix/abi/cap.h`
- `shared/include/midix/abi/syscall.h`
- `shared/include/midix/abi/ring.h`
- `shared/include/midix/abi/boot.h`
- `shared/include/midix/abi/service.h`
- `shared/include/midix/abi/protocols/nameservice.h`
- `shared/include/midix/abi/protocols/terminal.h`
- `userland/lib/include/mochi/status.h`
- `userland/lib/include/mochi/handle.h`
- `userland/lib/include/mochi/boot.h`
- `userland/lib/include/mochi/endpoint.h`
- `userland/lib/include/mochi/nameservice.h`

## Files changed

- `userland/lib/include/mochi/syscall.h` now includes shared ABI headers and exposes `mx_*` wrappers that return explicit `mx_status_t` values.
- `userland/lib/include/mochi/ring.h` now cross-checks its layout against `midix/abi/ring.h`, exposes `ring_try_push`, and only calls `notify` when `receiver_about_to_sleep` was set.
- `userland/lib/include/mochi/console_config.h` is now version 2 and has optional terminal channel fields kept separate from log rings.
- `userland/nameservice/main.cc` now replies with `MxEndpointDesc` instead of only a bare handle, while still serving the existing static table.
- `userland/demo/ping/main.cc` now accepts endpoint-descriptor replies and retains fallback support for the old bare-handle reply.
- `userland/lib/include/dango/vm.h` now derives Dango cap/right/status constants from the shared ABI.
- `kernel/include/anko/cap.h` and `kernel/include/anko/syscall.h` now static-assert key values against the shared ABI.
- `userland/CMakeLists.txt` and `kernel/CMakeLists.txt` include `shared/include`.

## API shape

### Kernel ABI

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

### Capabilities

Use `MX_CAP_*` and `MX_RIGHT_*` from `midix/abi/cap.h`. Existing V0 code can still use `RIGHT_READ`, `RIGHT_WRITE`, and `kCapTypeSharedRing` compatibility names, but new services should use the namespaced `MX_` constants.

### IPC

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

`ring_try_push` never waits for space. It only enters the kernel if the receiver has set `receiver_about_to_sleep`, which preserves the Midix hot path: shared-memory publication first, kernel doorbell only for sleep/wake.

### Endpoint descriptors

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

### BootInfo

`midix/abi/boot.h` defines a generic `MxBootInfo` page. `io.console` still uses `ConsoleBootConfig` because it is the first service and must come up before richer service discovery, but new services should move toward named boot entries rather than fixed cap-slot folklore.

## Security and progress rules

1. Default APIs are try-style and do not block the kernel.
2. Blocking is opt-in and named.
3. Full-ring backpressure is returned to userspace as `false`/status rather than hidden in the kernel.
4. Cap rights are explicit and come from one shared ABI header.
5. Service endpoints are bundles, not ambient global objects.
6. Terminal input is provisioned as a separate future channel in `ConsoleBootConfig`; it is not mixed into kernel/service log rings.

## Follow-up work

- Move `init`, `demo.ping`, and Dango manager from fixed cap slots to `MxBootInfo` entries.
- Make nameservice support registration from init using `MxEndpointDesc` rather than static table seeding inside nameservice.
- Add the interactive serial terminal on top of `MxEndpointDesc` and terminal protocol constants.
- Add CI that checks the ABI headers against kernel headers and verifies that `ring_try_push` does not notify on the hot path.


## Native driver support extensions

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
definitions and 128 channels; a selected profile remains bounded by the 24-row
service registry. Session directories admit eight clients and 32 exports.

## Bring-up diagnostic reads

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
