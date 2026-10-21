# CDC serial and USB-C reconnection

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

The interactive helper reopens the same serial path after unplug/reset.
Ctrl-] exits, including while disconnected. Unsent input is discarded after a
disconnect. Select a port explicitly with `--serial /dev/cu.usbmodem...` if more
than one exists. Reconnection follows that path; moving to a different physical
Mac port can require restarting the helper with the new path.

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

## Host topology cleanup after disconnect

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

## Physical validation of the previous sink-only image

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

## Automatic switching verification

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
