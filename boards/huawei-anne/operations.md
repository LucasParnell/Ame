# Huawei Anne operations and diagnostics

Use fresh packages with matching ELF, signed bundle and provenance. These workflows retain capture, restoration and installation evidence; references to a device being installed/restored are dated observations. Private firmware and readback records stay under ignored artifact directories. See [boot](boot.md) for EL2 handoff and [USB](../../docs/usb.md) for the service/protocol boundary.

- [ANE BootROM flashing with an EL2 handoff](#flash_el2)
- [Native boot diagnostics](#boot_diagnostics)
- [CDC serial and USB-C reconnection](#cdc_serial)
- [USB app sessions](#app_stream)
- [Physical USB app-stream test — 2026-10-14](#app_stream_hardware)

<a id="flash_el2"></a>
## ANE BootROM flashing with an EL2 handoff

<a id="flash_el2-ane-bootrom-flashing-with-an-el2-handoff"></a>

`flash-el2.py` brings the p20re `anne-el2/run.py`, `install_kernel.py`, and
BootROM upload transport into this worktree. It uses the previously tested
stock LK/TEE preload route. It writes the Ame kernel partition,
verifies the complete flashed partition, and uses `oem boot`
to preserve the RAM EL2 patch. An ordinary reboot discards that patch.

Dependencies: Python with `pyserial` and `pyusb`, and access to both the Huawei
BootROM serial device and fastboot USB device.

The four private firmware inputs are copied from p20re into
`out/anne-device-private/el2-firmware/`, which is ignored by Git:

- `xloader.img`: signed `tools/potatonv/bootloaders/hisi65x_a/xloader.img`
- `stock-lk-el2-tee.bin`: stock display initialization, EL2 descriptor, TEE hook
- `teeos-decrypted.bin`: captured plaintext TEE preload
- `manifest.json`: p20re's `stock-preload-manifest.json`

The helper pins firmware hashes and RAM patch addresses. It requires the
handset's already cleared FBLOCK; it does not change FBLOCK or flash LK/TEE.
No USB drive or p20re Python modules are required after copying those inputs.

Build and package, choosing a fresh output directory:

```sh
cmake --build out/anne-usb-native-release --target anko mochi_system provenance
.venv/bin/python3 scripts/anne/build-native-package.py \
  --build out/anne-usb-native-release --output out/anne-new-package \
  --avb-key ~/.ame-keys/anne-avb-key.pem
.venv/bin/python3 scripts/anne/flash-el2.py \
  --package out/anne-new-package --check
```

With the ANE-LX1 in BootROM download mode (`12d1:3609`), flash and boot:

```sh
.venv/bin/python3 scripts/anne/flash-el2.py \
  --package out/anne-new-package --serial HANDSET_FASTBOOT_SERIAL
```

`--no-boot` retains diagnostic LK after the verified flash. `--running` skips
BootROM upload and still verifies the running LK patches and TEE staging.
Backups are opt-in with `--backup`; normal flashes reuse the existing backup.
Records, readback images, optional backups and hashes are saved under
`out/anne-device-private/kernel-install-<UTC timestamp>/`. A readback mismatch
stops before boot. Actual EL2 entry still needs the kernel's `diag el2` evidence.

<a id="boot_diagnostics"></a>
## Native boot diagnostics

<a id="boot_diagnostics-native-boot-diagnostics"></a>

The `huawei-anne` Anko target embeds the v6 probe's verified LED, counter and
LPM3 reset transport. It starts before BSS clearing and global constructors;
it requires the verified EL1/EL2, MMU-off, data-cache-off entry state.

Five records in the reserved 1 MiB ramoops area at `0x34800000` receive the
same trace. Their headers and new bytes are cleaned to memory at checkpoints,
newline boundaries and before reset. The area is reserved from allocation and
mapped in every process address space. Logging uses no heap and avoids the unverified UART. After MMU enable,
a per-CPU recursive guard serializes complete messages across cores; early
MMU-off logging uses no exclusive instructions. Each record is bounded; it stops appending when full.

Records start with `AMEANKO1`. `BOOT N name tick=...` identifies the last
checkpoint. `CTOR pc=...` identifies each constructor before it executes.
The checkpoints cover BSS, constructors, probing, memory allocation and MMU,
IOMMU, interrupts, timer, capabilities, IPC, scheduler, authenticated bundle,
init creation, scheduler entry, first timer IRQ and service readiness.

`!FAULT` captures the last checkpoint, ESR, fault PC, FAR and original stack
pointer on a private per-CPU fault stack. `!PANIC` records the panic reason.
`RESET nmi=0x4` precedes the verified LPM3 NMI
pulse. Nested faults bypass logging and attempt reset directly. A hardware hang
that raises no exception still needs the physical fastboot button sequence.

LED stage requests are red at entry, blue at `kernel_main`, green only at service
readiness, and white on fault. These transitions have no blocking LED dwell;
the former three-second pauses were removed from the boot and fault paths.
The measured 1,920,000 Hz counter rate is used by the native scheduler, timer
and kernel timestamps. The v6 phone test showed red, purple, purple, then reset: physical
LED output worked but selected colours were mixed. The current native LED
sequence fixes unselected output-control pairs, and the v3 eight-core capture
physically showed red, blue, green, reboot. Historical v6 captures used the
three-second holds; the minimal probe now records transitions without waiting.

Build and package:

```sh
cmake --build --preset anne-native-dev --target anko
python3 scripts/anne/build-native-package.py --output out/anne-boot-trace-package
```

For a bounded phone capture, configure with
`cmake --preset anne-native-dev -DANKO_ANNE_DIAG_CAPTURE_SECONDS=10` before
building. This resets through LPM3 as soon as init reports service readiness,
on a userspace fault, or after ten seconds from scheduler entry when a timer interrupt or syscall
next checks the deadline. It records `CAPTURE reason=services.ready`,
`CAPTURE reason=el0.fault` or `CAPTURE reason=timeout`. Userspace exceptions
also leave a `!USERFAULT` line with process/thread IDs and fault registers;
those PCs belong to the userspace image, not the kernel ELF. Set the option
back to zero for normal operation.
This is a diagnostic run, not a ten-second limit on the production target.
A CPU stuck without interrupts or syscalls still needs physical recovery.

With `ANKO_ANNE_DIAG_RESET_AT_READY=OFF`, the configured interval measures
runtime **after service readiness**, including framebuffer readiness. Startup
retains a separate timeout of at least 30 seconds from scheduler entry. A slow
boot therefore records `timeout` rather than claiming `runtime.observed` before
services are ready. Watchdog retention uses the same runtime interval.

The package saves the exact `kernel.elf` and raw Image with hashes, alongside
the AVB container and stock restore image. Use that ELF to decode this run;
later builds can move instruction addresses.

After testing, restore the full stock `kernel` image, then on this phone use
`fastboot reboot bootloader` and allow recovery to start. This session verified
that route to installed TWRP; `fastboot reboot recovery` enters eRecovery.
The user reconfirmed this route during native USB work: use `reboot bootloader`,
not `reboot recovery`. Check the actual transport after the command and use the
installed-TWRP button route if recovery does not appear; a reboot acknowledgement
alone does not verify TWRP startup.
Do not issue overlapping USB writes or assume the shared recovery kernel can
boot while it still contains Anko.

Collect and decode after stock restoration:

```sh
python3 scripts/anne/collect-native-log.py \
  --package out/anne-boot-trace-package \
  --output out/anne-device-private/boot-trace-results
```

The collector verifies TWRP, the stock kernel checksum and the packaged ELF.
It copies available pstore records, saves their text and decoded fault symbols,
and prints only the diagnostic summary. `--offline` decodes previously saved
records. It does not flash, reboot or modify the phone.

Local validation uses `anne-trace-qemu-dev` with synthetic RAM-backed transport
addresses, away from the DTB and kernel. It checks full EL1 and EL2
boots through service readiness, faults before constructors and after MMU
enable, and retention in all five copies across address-space switches. The
synthetic build is a QEMU target and is rejected by the native package builder.

The synthetic eight-core workload validates the EL3 diagnostic range through
`MX_DIAG_EL3_MAX_INDEX`, shared with the syscall dispatcher. Metadata remains
unavailable, every recognized native operation returns `NOT_IMPLEMENTED`, and
the next index and `u64` maximum return `INVALID`. The workload then runs and
checks one pinned worker per CPU. A stale boundary used to stop the validation
service before its workers launched, causing readiness timeouts.

Minimal-entry tests require every LED transition to be recorded in under one
second, with no configured dwell, and still verify elapsed-time boundaries,
counter wrap, framebuffer bounds, pixels and glyphs. The updated focused gate
passes all 27 boot/display tests, including eight-core EL1/EL2 workloads, fault
records and watchdog retention. Normal QEMU boot also reaches all-services-ready.

This validation found a real native bundle mapping bug: `.balign 16` left the
embedded MXB physical base unaligned for init's page mapping. The embedding
now aligns and pads the bundle to 4 KiB without changing its signed contents.
This fixes the local init failure; the subsequent phone capture also reached
init and service readiness.

The first full instrumented phone run showed red, purple, then a persistent
second purple stage. Those requests occur at entry, `kernel_main` and scheduler
entry. Stock restoration and TWRP boot succeeded, but neither pstore nor the
DFX backup retained an `AMEANKO1` record after the physical button reset.
That observation does not prove an exception or a service-readiness failure.
The bounded capture subsequently retained five identical 2916-byte records.
All checkpoints 1–36 were reached, including MMU enable, the first timer IRQ,
signed bundle authentication, init's first syscall, `io.console: ready` and
`init: all services ready`. There are no kernel faults, userspace faults or
panics. `CAPTURE reason=services.ready last=36` immediately precedes the NMI
reset. Stock restoration and TWRP startup are checksum-verified. This establishes
one-core native startup with the selected minimal services; it does not establish
longer-running stability, framebuffer or native USB operation.

The successful capture package is `out/anne-boot-trace-package-v2b/`, image
SHA-256 `104514e6eac19ae1a7a8f0030e24fdb27c87fc641e454be283aae7c2e5063bcf`.
Its matching ELF and private decoded records are retained alongside the run.
Kernel logs are also mirrored directly into the
trace after console ring attachment, so a stalled console service cannot hide
the scheduler's dispatch or userspace fault messages.

Eight-core native startup is now verified; see [SMP](hardware.md#smp) and the
[framebuffer reference](hardware.md#framebuffer) for the latest
LED correction, hardware evidence and recovery state.

<a id="cdc_serial"></a>
## CDC serial and USB-C reconnection

<a id="cdc_serial-cdc-serial-and-usb-c-reconnection"></a>

The native USB profile automatically selects peripheral mode for a connected
Mac and host mode for a keyboard/OTG adapter. Both `drv.typec.fusb301` and
`io.usb.role` receive the signed `usb.device_only=0` binding. Setting both to
one restores the previous sink-only behavior; an omitted binding also defaults
to sink-only for compatibility with older descriptors.

The FUSB301 starts in Sink mode (`0x04`) for a 600 ms preference window. If no
source attaches and VBUS is absent, it tries DRP (`0x10`). A detected sink gets
one additional sink-preference window before host fallback, allowing a dual-role
computer to become the source. An active connection stays in its selected role;
detach rearms sink preference. These windows run after service readiness and do
not block shell startup. They are software policy, not a chip Try.SNK enable bit.
Register definitions follow the [onsemi FUSB301 datasheet](https://www.onsemi.com/pdf/datasheet/fusb301-d.pdf).

The role service requires 100 ms of stable role/CC observations before attach.
It removes source power immediately on a detected detach, changed CC/role or
failed query. Before starting the existing 500 mA source lease it reads CC/VBUS
again after PHY preparation; an external supply or changed cable cancels source
startup. Active host VBUS is expected and does not cancel our own lease. Power,
PHY and controller faults disable the source and inhibit retries until detach.
The controller is quiesced before changing PHY/role, and the power service keeps
its independent two-second lease/watchdog. Software sink preference cannot
identify a computer that insists on being a sink; role selection follows CC.
A source session additionally needs a USB data connection within one second.
After that, a disconnect or changed controller connection generation revokes
power even if CC still describes the previous sink. Before any controller wait,
the role service turns boost off, selects Sink and explicitly writes FUSB301
UNATT_SNK after the required 2 ms delay. Reconnect then gets a two-second sink
preference window. This prevents an unplugged keyboard's source lease from
being transferred to a computer accepting charge. The initial bounded source
probe cannot identify a partner before power and USB attachment are established.
Vendor bootloader behavior remains separate.

Startup reads ID/Modes/Control together, writes the adjacent Modes/Control image,
and verifies both in one read. This keeps verified initialization to three I2C
round trips. The policy uses two 16-byte service-owned state records; there is
no allocation or cross-CPU shared polling state in role selection.

The Hi6523 maintenance check reads live STATUS0 and latched IRQ0 in one
contiguous register snapshot. On the first maintenance check only, recovered
overcurrent/undervoltage/short-circuit startup events are acknowledged when
OTG_ON still reports a healthy output. The
[pinned vendor handler](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/power/hisi/charger/schargerV300/hisi_scharger_v300.c#L4111)
logs/clears OCP and qualifies UVP/SCP against live OTG_ON. IRQ0 is latched
history, not an active-fault register: a phone capture showed healthy
`status=0x40` with startup `faults=0x08`, followed by software cutting power.
A stopped boost, overvoltage/overtemperature, recurring OCP/UVP/SCP after the
first sample, or failed I2C operation disables the source. Renewals cannot silently restart a failed or
expired power session. Failure logs retain the stage, live status and latched
faults; USB disconnect logs retain the observed and expected connection epochs.

Connect with:

```sh
.venv/bin/python3 scripts/anne/usb-input.py console
```

On Linux the CDC port must be reachable by your login. Install the rule shipped
in this repository rather than editing device nodes by hand:

```sh
sudo install -m 0644 scripts/anne/udev/99-ame-serial.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

That grants the `tty` node (for `screen`/this helper) and the raw `usb` node
(for the vendor bulk interface) to `OWNER` in the rule, and creates `/dev/ame`.
Without it the node appears as `root:root 0600`, so a non-root `screen` fails
with `EACCES` and the bulk path fails with "Access denied".

The helper discovers the port itself: `/dev/ame`, then `by-id`, then the macOS
`/dev/cu.usbmodem*`, then `/dev/ttyACM*`. Select a port explicitly with
`--serial /dev/ttyACM3` when more than one candidate exists. The interactive
helper reopens the same path after unplug/reset; Ctrl-] exits, including while
disconnected. Unsent input is discarded after a disconnect.

A serial-only host does not need a keyboard to reach its console: `tty.fb0`
opens its input and output gates when a CDC host attaches (the FLOW_ON session
control), in addition to the local three-ESC unlock.

For individual commands:

```sh
.venv/bin/python3 scripts/anne/usb-input.py console --send 'help' --enter --read 3
```

Terminal lifecycle records now reach msh through the same ordered, bounded
input queue as bytes. Reconfiguration clears the unfinished line and escape
sequence and redraws the prompt. A full shell-input ring retains these records
until delivery. msh preserves bytes after a submitted line, so pasted command
batches work, and treats CRLF as one newline across packet boundaries.
The Mac helper tolerates disappearance of a tty during settings restoration
and closes its descriptor without hiding the disconnect error.

<a id="cdc_serial-host-topology-cleanup-after-disconnect"></a>
### Host topology cleanup after disconnect

The DWC2 controller removes a disconnected device and revokes its endpoints
before io.usb consumes the port event. io.usb previously required a successful
second removal, so the controller's `NOT_FOUND` reply left stale interfaces,
claims and endpoint records behind and omitted the keyboard detach notice.
It now accepts an already absent controller device as removed and releases
its own topology. Other errors, timeouts and unexpected completion messages
still retain that topology.

The host regression runs the production io.usb implementation through 32
removal cycles, alternating successful and already absent replies. It checks
resource reclamation and one detach event per interface, plus preservation on
controller failure/timeout. Enumeration failures now log their port and stage
to the normal console/retained log, including reset/address, descriptor reads
and SET_CONFIGURATION.

This fixes a reproduced disconnect cleanup defect. Physical keyboard
attachment after an empty boot, keyboard replug and Linux CDC reconnection
still need retesting; those symptoms are not established as sharing one cause.

<a id="cdc_serial-physical-validation-of-the-previous-sink-only-image"></a>
### Physical validation of the previous sink-only image

Tested on ANE-LX1, firmware 8.0.0.151(C782), with release run tag
`anne-cdc-hw-v1`. Flashed kernel image SHA-256:
`c6e20e00121e820535180a7af02652287c3b8007419477c417facfc0743e852a`.
The signed package and verified stock restore are in `out/anne-cdc-package-v1`.

Passed on the phone:

- One physical cable unplug/replug with the same interactive helper process.
  It observed disconnect, reopened, executed four subsequent commands, and
  exited cleanly. An unfinished command before unplug did not contaminate the
  next command.
- Three USB bus resets with partial input before each reset.
- Four serial close/open cycles, twelve repeated commands, three commands in
  one CR/LF/CRLF batch, and three immediate `--send` commands after opening.
- Retained log confirmed the new run tag and `typec: sink-only; source disabled`
  after the driver's mode-register readback.
- USB app tunnel opened, streamed `app.stream-test` exited zero with memory
  released, and the tunnel closed successfully after the serial tests.

Power validation covers the programmed sink mode and source-disable policy;
no external electrical current measurement was taken. Private transcripts and
JSON results are in `out/anne-device-private/cdc-hardware-v1`.

The focused host suite passes 29 tests across USB, sender, msh input, app
streaming, and system descriptors. USB and msh C++ probes run with AddressSanitizer
and UndefinedBehaviorSanitizer, including full-ring lifecycle delivery.

<a id="cdc_serial-automatic-switching-verification"></a>
### Automatic switching verification

Release `anne-usb-auto-v5` is flashed, image SHA-256
`1a4fa94b211699c25809e27f542927f85b6b1e892e95c32d04b2838353594826`.
One boot reached msh in 0.30 s (308.521 ms to all-services-ready), with eight
CPUs online and MMU/instruction/data caches enabled. The user confirmed stable
keyboard power and typing, followed by normal Mac reconnection without charging
from the phone. The retained log confirms healthy startup OCP recovery,
6.10 s from keyboard-ready to physical unplug, and return to peripheral mode
with source off; a subsequent diagnostic command completed over CDC. An
initial stopped-boost/SCP attempt was shut down and retried before the stable
keyboard session, so an initial attach may still require a protected retry.

The host suite runs eleven ASan/UBSan C++ probes, including exhaustive CC/type
decoding, source connection generations, 768 startup/live fault combinations,
I2C failure injection, and a recovered source followed by recurring-fault
shutdown. Twelve descriptor tests also passed. Exact package, source hashes,
boot captures and user/trace verification are in
`out/anne-device-private/usb-auto-v5`; the package is
`out/anne-usb-auto-package-v5`.

<a id="app_stream"></a>
## USB app sessions

<a id="app_stream-usb-app-sessions"></a>

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
[APP_STREAM_HARDWARE.md](operations.md#app_stream_hardware).

Validation completed: native Anne build, five app-stream host tests, existing
USB/sender/descriptor tests, and QEMU four-core app, lifecycle, and supervision
regressions. MXI-format and power tests pass using the project's `.venv` Python.
The broader host run still has failures in the separate minimal-framebuffer
fixtures (timing assertions and a framebuffer probe compilation failure).

<a id="app_stream_hardware"></a>
## Physical USB app-stream test — 2026-10-14

<a id="app_stream_hardware-physical-usb-app-stream-test-2026-10-14"></a>

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
