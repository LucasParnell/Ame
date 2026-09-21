# Phase G (hardened), H (`dango-gpud`) and I (shared staging framebuffer)

Base `d6eaf06`. Commits `3f9cd43` (plumbing), `49252a1` (guest gpud), `654210c` (host + gate).

## Phase H — `dango-gpud` control plane (done)

`linux-artifacts/gpud/` is the appliance-side daemon (spec §14): `gpud_main.c` (event-driven
loop), `gpud_channel.c` (Dango channel client), `gpud_protocol.c` (opcode dispatch + fixed-capacity,
boot-generation-scoped object table), `gpud_drm.c` (raw DRM/KMS). It serves `GPU_HELLO`,
`GPU_ENUM_OUTPUTS`, `GPU_GET_MODES` and `GPU_ERROR`, validates every object id and buffer range, and
never forwards a raw DRM ioctl. The host side (the bootstrap `drv.gpu.linux` role, in
`dango.manager`) negotiates HELLO → ENUM_OUTPUTS → GET_MODES and logs
`gpu control: hello/outputs=/mode=`.

## Phase I — shared staging framebuffer (done)

- A VM creates a fixed staging object (`MX_VM_MEMORY_FLAG_SHARED_STAGING`); it is host-writable and
  guest-mappable but **excluded from every DMA domain** (`collect_vm_ram` skips it), so staging stays
  non-DMA until explicitly registered. `SYS_VM_CHANNEL_STAGE` publishes its guest PA/size into the
  channel descriptor; the guest channel driver exposes rings + staging through one mmap.
- The bootstrap pipeline runs end to end: the manager renders a 4-bar XRGB8888 frame into staging,
  `GPU_BUFFER_REGISTER`s it, `GPU_SET_MODE`s, then `GPU_PRESENT`s; gpud copies staging into a DRM dumb
  buffer, programs the CRTC and returns the **asynchronous** `GPU_PRESENT_COMPLETE`; the host asserts
  the displayed frame with a `screendump`.

## Phase G — hardened

The channel binding stays single-sourced in `midix/abi/dango_channel_guest.h`; the staging window and
the bootstrap mode were added there and the guest DTS is generated from them. The guest driver now
accepts and exposes the staging window (it previously rejected it). The channel self-test's ring
sequence/counter checks became relative, because the bootstrap GPU exchange shares the channel.

## Verification (macOS arm64, Clang 23.1.1 / CMake 4.4.0 / QEMU 11.1.1; guest rebuilt in the container)

- `scripts/verify.sh` — **all checks passed**.
- `gpud` (`AME_VERIFY_GPUD=1`) — success: `gpu control: hello major=1 minor=0`, `outputs=1`,
  `mode 1024x768`, `gpu staging: registered`, `gpu present: complete`, `faults=0 quarantines=0 smmu=1`.
- `gpu-appliance`, `kms-display`, `dango-channel`, `assignment`, `iommu-dma` — all green.

## Deliberate decisions / deviations

- The bootstrap GPU client lives in `dango.manager` (which already owns the VM, the channel and the
  caps) rather than a new manifest service; extracting it into `drv.gpu.linux` is the §15.1 step and
  needs no ABI change (Phase J will consume it).
- The manager drives the GPU exchange **before** the channel ping/pong probe, matching the guest order
  (gpud holds DRM master, then the self-tests).
- The bootstrap mode is part of the shared binding, so manager/DTS/harness no longer restate `1024x768`.

## Outstanding rule item

`shared/include/midix/abi/protocols/gpu.h` is C++-only, so `linux-artifacts/gpud/gpud_protocol.h`
carries a byte-exact **C projection** of it (spec §13.1 requires that header to compile in the native
client, `drv.gpu.linux` **and** the Linux `dango-gpud` build). Fix: make `gpu.h` C/C++-compatible
(guard the `types.h`/`ring.h` includes with a C branch, plain `enum`s, a `MX_STATIC_ASSERT` macro,
`typedef` for the object id, `#define` for the scalar protocol constants, C++-guard the
constexpr uniqueness check) and delete the projection. Mechanical; not done here to avoid an unverified
last-minute header refactor.
