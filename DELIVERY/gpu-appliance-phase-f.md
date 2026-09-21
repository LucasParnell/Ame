# Phase F — standalone Linux KMS self-test, asserted from the host

The appliance guest now drives a real DRM/KMS modeset of its own (raw ioctls, no libdrm) and the
host asserts the displayed pattern from a QEMU `screendump`. This proves the whole guest graphics
stack — device assignment + MMIO + IRQ + DMA + Linux DRM + `virtio_gpu` — **independently of the
Midix GPU protocol**, with Ame containing no virtio-gpu register logic.

## Guest userspace: `linux-artifacts/kms_test/kms_test.c`

A freestanding (no libc, no libdrm) translation unit linked into the appliance init binary. It
issues only syscalls (`openat`, `ioctl`, `mmap`, `write`) and hand-built DRM ioctl requests:

1. open `/dev/dri/card0`, take DRM master;
2. `GETRESOURCES` (two-call) → CRTC/connector ids;
3. `GETCONNECTOR` (two-call) → preferred mode; fetch **only** the mode array;
4. `CREATE_DUMB` + `MAP_DUMB` + `mmap` → fill four equal vertical bars (XRGB8888 R/G/B/W);
5. `ADDFB`, then legacy `SETCRTC` (virtio_gpu exposes this through
   `drm_atomic_helper_set_config`); `GETCRTC` confirms the CRTC scans our FB.

The ioctl encoding and every UAPI struct are drift-checked with `_Static_assert` (the request
values are asserted against their numeric form). It prints `dango: kms displayed` /
`dango: kms test passed` for the harness and keeps the fd open so the pattern stays on screen.

The agent calls it after the Phase E DRM probe succeeds (`DANGO_GPU_KMS`, gpu profile only).
`scripts/linux/build-agent.sh` links the extra unit; no new kernel config is required — the test
uses the DRM/KMS path the GPU fragment already enables, with `DRM_FBDEV_EMULATION=n`.

## Host capture: `scripts/test/kms-display.py`

- Boots via `scripts/test/boot.py` with the SMMUv3 topology and the assigned `virtio-gpu-pci`,
  now with `id=gpu0,edid=on,xres=1024,yres=768` (the headless console needs an EDID to offer modes;
  `id` targets the capture away from the `ramfb` console).
- `boot.py` gained an opt-in monitor path: `--monitor-socket` attaches HMP on a unix socket and
  `--screendump`/`--screendump-device` capture the display once the markers match. The default
  runner (`-monitor none`, no capture) is byte-for-byte unchanged.
- Asserts the PPM is 1024×768 and samples the mid-row at 1/8, 3/8, 5/8, 7/8 → exact R/G/B/W.
- In the gate as the opt-in row `kms-display`, `AME_VERIFY_KMS=1` (reuses the Phase E build tree).

## Interleaving fix (also removes a Phase E gate flake)

The guest UART and the Ame host console share one QEMU serial file, and the guest driver writes
byte-by-byte, so an Ame log entry can land **inside** a guest line and split a marker
(`dango: DRM card[ 5.12… ] [I] …\n0 driver=virtio_gpu …`). This was the root cause of an
intermittent `gpu-appliance` gate failure. `scripts/test/logfmt.py` removes Ame log entries
(including their newline) to rejoin the guest stream; `boot.py`, `gpu-appliance.py` and
`kms-display.py` now match markers against raw **plus** the de-interleaved guest view.

## Evidence

Validated on macOS arm64, Homebrew Clang 23.1.1 / CMake 4.4.0 / QEMU 11.1.1:

- `AME_VERIFY_IOMMU=1 AME_VERIFY_ASSIGN=1 AME_VERIFY_GPU=1 AME_VERIFY_KMS=1 scripts/verify.sh`:
  every row green, including the new `kms-display` row.
- Guest markers: `dango: kms fb=40 crtc=36 connector=37 mode=1024x768`, `dango: kms displayed`,
  `dango: kms test passed`; IOMMU counters clean (`faults=0 quarantines=0 smmu=1`).
- Captured display `kms.ppm`: 1024×768 with mid-row samples `(255,0,0) (0,255,0) (0,0,255) (255,255,255)`.
- Repeated runs: `kms-display` and `gpu-appliance` both green on 3/3 back-to-back boots.

## Files

| Concern | Path |
|---|---|
| Guest KMS self-test | `linux-artifacts/kms_test/kms_test.c` |
| Agent hook (`DANGO_GPU_KMS`) | `linux-artifacts/agent/dango-agent.c` |
| Guest userspace build | `scripts/linux/build-agent.sh` |
| Manifest regeneration (shared) | `scripts/linux/build-manifest.sh`, `scripts/linux/build-gpu-linux.sh` |
| Boot runner monitor/screendump | `scripts/test/boot.py` |
| Acceptance harness | `scripts/test/kms-display.py` |
| Log de-interleaver | `scripts/test/logfmt.py` |
| Gate row | `scripts/verify.sh` (`AME_VERIFY_KMS=1`) |

## Remaining

Phase G onward (`docs/GPU_APPLIANCE_TODO.md`): the guest-visible Dango channel, `dango-gpud`
control plane, shared staging framebuffer and TTY migration. No physical GPU or AMD-specific work
is included.
