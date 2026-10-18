# USB app sessions

The `huawei-anne-usb` system includes `dev.apps`, a userland runner for native
AArch64 Ame `.mxi` applications and tests. Start it from `msh`:

```text
container start
container status
container stop
```

The upload tunnel stays closed until `container start` acknowledges readiness.
One app runs at a time. After it exits, the runner becomes ready for the next
upload. `container stop` cancels an upload or kills a running app, waits for
its exit, and closes the tunnel. USB disconnect/reset also closes the session.

On the computer, with PyUSB and libusb installed:

```sh
cmake --build out/anne-usb-native-dev --target anko mochi_system stream_test provenance
python3 scripts/anne/app-stream.py out/anne-usb-native-dev/userland/app.stream-test.mxi
python3 scripts/anne/app-stream.py path/to/test.mxi --run-ms 60000
```

This requires booting an Ame image containing these changes. Building the
system does not install it on the phone. The sender claims vendor interface 2
on VID `1209`, PID `0001`; CDC serial remains available for `msh`.
Use `scripts/anne/usb-input.py console` from the computer, and `diag log` in
`msh` to read app output from the retained log.
The existing bulk keyboard protocol continues to work alongside app uploads.

Uploads live in temporary arenas, with an 8 MiB file limit. The kernel validates
MXI structure and its checksum before creating executable mappings. Apps run
as `native_sandboxed` with the existing 4 MiB resident page quota and receive
BootInfo, logging, readiness, and CPU metadata. Filesystem, device, VM, and
supervisor capabilities are not granted to uploaded apps. ELF executables must
first be converted with `scripts/mx/mxld.py`; Linux/Android binaries are not
native Ame apps.

The runner clears and releases the upload arenas immediately after the loader
copies the image. App code, stack, scratch arenas, log ring, and BootInfo are
released after the child exits; the physical allocator zeros pages before
making them available again. A cancelled session remains `stopping` until its
exit is acknowledged, preventing reuse while another core still runs it.
Apps also die if their runner dies. Nothing is installed in persistent storage.

Incomplete uploads expire after ten seconds without accepted data. Runtime is
bounded to 30 seconds by default, configurable from 1 to 300000 milliseconds.
The sender cancels its upload/app on errors or interruption. A generation and
session identifier prevent an earlier upload from continuing in a new session.

The wire protocol uses existing AMK1 framing and CRC32 with types `0x100` through
`0x105`: hello, begin, data, run, poll, cancel. Hello has an empty payload; other
requests begin with the session's LE u64 identifier. Begin adds image size and
runtime limit (u32 each); data adds a u32 offset and at most 52 image bytes.
Replies contain status i32, state u32, received offset u32, exit code i32, and
session ID u64. CRC is transport integrity; local `msh` activation authorizes
the tunnel. Keyboard frames retain their original types and handling.

Validation commands:

```sh
python3 -m unittest discover -s tests -p 'test_app_stream.py'
cmake --build out/qemu-dev --target anko app_stream_regressions
python3 scripts/test/boot.py --build out/qemu-dev --mode direct --cpus 4 \
  --bundle out/qemu-dev/app-stream.mxb --timeout 30 \
  --marker 'app streaming kernel regression passed' \
  --output out/qemu-dev/app-stream-validation
```

The kernel fixture uses the production session/resource owner to run ten apps,
checks stable object/page counts, tests malformed images and cancellation,
verifies supervisor-death cleanup, and checks that reused scratch pages are
zero. Host sanitizer tests cover protocol bounds, lifecycle failures, and USB
router backpressure. Physical phone results are recorded in
[APP_STREAM_HARDWARE.md](APP_STREAM_HARDWARE.md).

Validation completed: native Anne build, five app-stream host tests, existing
USB/sender/descriptor tests, and QEMU four-core app, lifecycle, and supervision
regressions. MXI-format and power tests pass using the project's `.venv` Python.
The broader host run still has failures in the separate minimal-framebuffer
fixtures (timing assertions and a framebuffer probe compilation failure).
