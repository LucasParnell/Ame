# Native framebuffer capture

Current device state: TWRP is connected and the full stock kernel checksum
is verified after v5. Two identical DFX records prove 60 seconds of healthy
runtime with eight CPUs ticking and 67 watchdog feeds. The later watchdog FIQ
was deliberately requested after observation for firmware trace retention.
The inherited watchdog timeout is addressed by keepalive, with 42 local checks
passing. Normal v5 is packaged and unflashed; indefinite uptime is unverified.
Controller and acceptance: `out/anne-device-private/watchdog-v5-test/`.
See [FIQ evidence and watchdog implementation](FIQ.md).

Updated 12 October 2026. The first native full-kernel framebuffer capture is
physically confirmed: the user saw a green screen with `ANKO EL1` at the top.
The label was slightly clipped. Four bounded records recovered from Huawei's
DFX backup confirm painting, eight-core worker completion, service readiness
and intentional LPM3 reset, with no faults or panics. After that capture, TWRP
and the complete stock shared-kernel SHA-256 were verified.

Console follow-up: the user confirmed the inset heading and boot/service logs
are visible and work on the phone. Clipping has been addressed and the verified native
scanout is now integrated with early boot output and the existing `io.console`
service. The normal Huawei target enables `ANKO_ANNE_FRAMEBUFFER` by default.
The heading begins at (64,144), with 3× 8×16 glyphs; the scrolling log viewport
begins at (64,224), uses 2× glyphs, and contains 59 columns × 61 rows. The
viewport preserves side margins, the heading and at least 96 bottom pixels.

The kernel owns this boot console. It replays retained early messages after
MMU setup, then renders boot stages, kernel output and service logs forwarded
by `io.console` through the native debug syscall. Raw constructor/register
metadata stays in the retained trace. Kernel ring messages have one visible
copy when `io.console` drains them. Writes are serialized by the existing
native trace guard and dirty pixel rows are cleaned for display DMA. The normal
image keeps the console after readiness; the bounded capture gives five extra
seconds to inspect it before resetting.

The console v1 capture is physically verified with retained-log readback. Its image is
`out/anne-console-capture-package-v1/`, SHA-256
`220e2afc5f62f4d3148191490ecc932affa80c9ad2ab1d04d353a4f0bc634c49`;
the normal image is `out/anne-native-console-package-v1/`, SHA-256
`ec2314a3a43783c6a7a79aca6054df581e33811ab4896126d0eed215fef5a7f0`.
Each includes its own matching ELF and stock restore image. The normal image
has no diagnostic deadline or readiness reset. It was flashed for the requested
demonstration after capture readback: visible console and readiness were
confirmed, followed by an FIQ fault. It is an archived failing image, not a
stable release.
The completed capture controller is
`out/anne-device-private/console-v1-test/control.py`.

Huawei DFX retained four tagged records from two executions. Two identical
35,667-byte copies reach `BOOT 36 services.ready`, contain `io.console: ready`
and `init: all services ready`, and confirm eight completed workers with positive
timer, preemption, context-switch and IPI counts on every core. The renderer
replayed 30,323 early trace bytes and processed 4,269 visible characters by
readiness, then intentionally reset after the viewing hold. The other two
34,978-byte copies initialize the console and finish all eight workers, but
hit the ten-second capture deadline before service readiness. Neither execution
records a fault or panic; repeated-startup reliability remains unverified.
Private raw traces, decoded records, stock verification and the evidence summary
are under `out/anne-device-private/console-v1-test/results/`.

The next planned native subsystem is [userspace USB](../../docs/USB_PLAN.md),
covering generic host peripherals, keyboard input and device-role debugging.

Local verification passes 17 focused host checks and 15 full instrumented QEMU
checks. The QEMU tests decode the actual framebuffer glyphs to verify the inset
`ANKO EL1/EL2` heading, `io.console: ready` and `init: all services ready`,
including eight-core startup, scrolling, IOVA rejection and unchanged margins.
The saved review surface is `out/anne-console-validation/el1-console.png`.

```sh
cmake --preset anne-console-capture-dev
cmake --build --preset anne-console-capture-dev --target anko provenance
python3 scripts/anne/build-native-package.py \
  --build out/anne-console-capture-dev --output out/anne-console-capture-package-new
```

To collect a future run after restoring stock and returning to TWRP:

```sh
python3 scripts/anne/collect-native-log.py \
  --package out/anne-console-capture-package-v1 \
  --output out/anne-device-private/console-v1-test/results --dfx-fallback
```

The new `RUN tag=anne-console-v1` identifies this capture. DFX fallback extracts
bounded persistent_ram records only when their run tag matches the package,
so older first-light traces cannot be decoded as this image. `--offline
--dfx-fallback` re-decodes the saved backup without touching the phone.

The read-only AArch64 Linux probe in `scripts/anne/fb-ioctl-probe.c` obtained
`FBIOGET_FSCREENINFO` and `FBIOGET_VSCREENINFO` in installed TWRP. The shared
stock kernel partition SHA-256 matched the backup before collection. It found:

| Property | Recovery observation |
| --- | --- |
| Panel | BOE_OTM1911A 5P84 VIDEO TFT, 1080 × 2280 |
| Virtual geometry | 1080 × 4560 |
| Active page offset | x=0, y=2280 |
| Pixel channels | R16:8, G8:8, B0:8, A24:8 (ARGB8888) |
| Stride | 4320 bytes |
| Allocated framebuffer bytes | 29,552,640 |
| `fix.smem_start` | **IOVA `0x40000`, not a physical framebuffer address** |
| Recovery H porches/pulse | back=80, front=130, sync=6 |
| Recovery V porches/pulse | back=26, front=16, sync=10 |
| Raw `var.pixclock` | 180,000,000; retain raw value rather than assuming picoseconds |

The matching EMUI 8 [`hisifb_alloc_fb_buffer` implementation](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/lineage-20/drivers/video/fbdev/hisi/dss/hisi_fb.c)
allocates through the ION system heap and assigns `iommu_format.iova_start` to
`fix.smem_start`. The recovery buffer cannot be treated as one contiguous
physical allocation, or as the native boot-logo buffer. The actual DTB's BOE
panel node specifies a 180 MHz pixel clock, four DSI lanes, 550 MHz DSI bit
clock and uncompressed video mode. Recovery has no exposed `/dev/mem`,
`/dev/kmem`, clock summary or display-register debug interface usable by the
inspection. Raw captures and downloaded source hashes are retained privately
under `out/anne-device-private/framebuffer-v1-inspection/` and
`out/anne-framebuffer-research/`.

The native capture establishes the actual inherited scanout, separate from
recovery's IOMMU allocation:

| Native property | Verified register evidence |
| --- | --- |
| Read channel | D0, logical channel 6, RDMA offset `0x50000` |
| Physical buffer | `0x5d000000` |
| Pixel format | ARGB8888, `DMA_CTRL=0x28` |
| Width/height | 1080 × 2280 |
| Stride | 4320 bytes, `DMA_STRIDE0=0x10e` (270 × 16) |
| Channel control | `CH_CTL=0xf001`, enabled physical linear path |
| Overlay path | D0 → enabled overlay0 layer0 → MCTL0/DBUF0 → LDI0/DSI0 |

The configuration is unchanged between entry and MMU completion. D2, D3, V1,
G1 and D1 are disabled. The earlier three-channel probes never examined D0;
their rejection was correct for the channels they inspected. Full path
validation found D0 without relaxing the memory or format checks.

`ANKO_ANNE_DISPLAY_CAPTURE` enables `display_diagnostics.cc` in the full native
Anko target. It records named register offsets and values at entry before MMU
enable, then again at `MmuDone` after installing Device mappings for DSS and
CRG. It preserves and maps the complete 48 MiB logo reservation as RAM. All
implemented Hi6250 read channels (D2, D3, V1, G1, D0 and D1), MCTL0/overlay0,
DBUF0, LDI0, DSI0, SMMU and clock/reset state are captured. Each candidate
records the first failing scanout check with a named reason.

Painting requires enabled nonsecure physical linear RGB565/ARGB8888/XRGB8888,
the full portrait rectangle, a bounded address in the reserved logo pool,
enabled DSI video with matching geometry, MCTL routing through overlay0 and
DBUF0, and an enabled full-screen layer selected from that channel. Cropping,
padding, masks, stretching, rotation, compression and IOVA addressing are
rejected. A successful candidate receives the existing white `ANKO EL1/EL2`
label on a black band and a green/magenta field; writes are cleaned to memory
for DMA and held for five seconds before boot continues. An absent scanout
produces `DISPLAY paint=skip no_verified_scanout`. This capture does not yet
program a new display-controller path.

Build and package into a fresh directory:

```sh
cmake --preset anne-framebuffer-capture-dev
cmake --build --preset anne-framebuffer-capture-dev --target anko provenance
python3 scripts/anne/build-native-package.py \
  --build out/anne-framebuffer-capture-dev \
  --output out/anne-framebuffer-capture-package-new
```

The preset preserves the eight-core worker profile and bounded readiness reset.
The package includes the exact ELF and verified stock restore image. Image
SHA-256: `2e830014802b48b5cb744b062e3ddb818b29446a805b09c1d0945c999bdb0c85`.
The tested controller is
`out/anne-device-private/framebuffer-v1-test/control.py`; its state was created
fresh for this image and now records the completed run and verified restore.
Create fresh state for a new image. The earlier local
`v1` package has incorrect display metadata; use `v1a`.

Focused validation: 12 full instrumented QEMU checks pass, including eight-core
EL1 capture with painted memory, EL2 painting, IOVA rejection with unchanged
buffer memory, readiness ordering, and faults. Host tests validate the ioctl
ABI, address/routing rejection, native packaging and framebuffer rendering.
QEMU reserves its synthetic logo RAM below its DTB, so painting preserves
topology discovery and all eight workers. There are 13 focused host checks and
12 instrumented QEMU checks. The physical observation is separate evidence
from these tests. The native DFX records also show positive switch, preemption,
tick and handled-IPI counts on all eight cores.

Collect only after restoring the stock shared kernel and returning to TWRP:

```sh
python3 scripts/anne/collect-native-log.py \
  --package out/anne-framebuffer-capture-package-v1a \
  --output out/anne-device-private/framebuffer-v1-test/results
```

The decoder reports display phases, rejected channels and paint completion
alongside existing boot/SMP/fault evidence. This recovery boot exposed no pstore
files, but its 16 MiB `dfx` partition retained four records from the framebuffer
image: two copies of each of two complete executions. Each bounded trace is
35,402 bytes. The remaining AMEANKO1 records belong to an earlier image and
were excluded. Use explicit header offsets and the exact packaged ELF:

```sh
python3 scripts/anne/extract-dfx-native-log.py \
  --input out/anne-device-private/framebuffer-v1-test/results/dfx-private.bin \
  --package out/anne-framebuffer-capture-package-v1a \
  --output out/anne-device-private/framebuffer-dfx-decoded-new \
  --header-offset 132096 --header-offset 656384 \
  --header-offset 919040 --header-offset 1443328
```

The saved decode and summary are under
`out/anne-device-private/framebuffer-v1-test/results/dfx-decoded/`. Extraction
uses validated persistent_ram header lengths; bytes beyond those lengths
contain stale recovery logs or binary data and must not be included. Select
offsets belonging to the tested run rather than interpreting every common tag
with the current ELF.

The first-light run confirmed visible output and automatic reboot from native
EL1. The console follow-up above implements the inset layout and normal
boot/service log integration; its physical verification is tracked separately.
