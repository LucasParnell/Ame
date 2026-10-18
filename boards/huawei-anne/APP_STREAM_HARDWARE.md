# Physical USB app-stream test — 2026-10-14

Tested on the identity-checked Huawei P20 Lite ANE-LX1, firmware
`8.0.0.151(C782)`, with the unlocked bootloader. The release image was flashed
to `kernel` and booted successfully. The retained log confirms
`RUN tag=anne-app-stream-hw-v1` and all eight CPUs online.

Installed package: `out/anne-app-stream-package-v1`.
Kernel partition image SHA-256:
`47983fcd451c47e32c149295262013d4cdbfbad2a21ae5cc0eb32ce02f9816dc`.

| Check | Hardware result |
| --- | --- |
| Start from msh | `container start` opens the USB upload tunnel |
| Repeated uploads | 20,480-byte native scratch tests upload and exit 0, approximately 0.85 seconds per upload/run/cleanup cycle |
| Scratch clearing | Raw scratch arenas filled with `0xa5`, released, reallocated, and checked for zero before any initialization; passed |
| Memory growth | Same metrics app reports 246 live objects and 8,830 allocated pages before and after repeated apps, cancellation, timeouts, and USB reset |
| Cancel running app | Returns to ready with exit -1 |
| Cancel incomplete upload | Returns to ready with received offset 0 |
| Runtime limit | Busy app killed at configured 300 ms limit; exit -1 |
| Abandoned upload | Discarded after approximately 10.04 seconds |
| Stop running app from msh | Child terminated; session closes |
| Closed tunnel | New request rejected with status -11 |
| USB reset during app execution | App terminated, session closed, CDC reconnected, memory counts unchanged |
| Restart after reset | New upload succeeds |

The memory snapshots were taken while the same metrics app was running; they
include that app's own fixed footprint. Identical samples show no growth across
the tested sessions. The scratch test checks actual contents of reused arenas;
the process/image reclamation check uses the live object and page counters.

App output reaches the framebuffer and retained log. It is available from the
computer through `diag log` in the USB shell; it is not mirrored automatically
into the live CDC shell stream.

The phone was left running this Ame image with the container closed. Open it
again with `container start`, then run:

```sh
.venv/bin/python3 scripts/anne/app-stream.py \
  out/anne-usb-native-release/userland/app.stream-test.mxi
```

Private raw records and test harnesses are in
`out/anne-device-private/app-stream-hardware-v1/`. The verified original stock
kernel restore image remains in the package.
