# Serial console: the Midix shell over USB CDC ACM

When the phone is plugged into a capable host, the `Ame USB diagnostics` CDC
port is a raw serial console for the Midix shell. Any serial terminal works:

```
screen /dev/cu.usbmodem* 115200        # macOS
picocom /dev/ttyACM0                   # Linux
```

Type at the host; msh's own line editor provides echo, backspace, arrows,
history, tab completion and control keys. No host-side software is required —
CDC ACM is class-standard.

## Data path

```
Mac serial terminal
  |  raw bytes both ways (/dev/cu.usbmodem*)
usb.fn.acm            (opaque CDC byte stream relay)
  |  MX_FUNCTION_RX / MX_FUNCTION_TX chunks (<=32 B)
dev.serial            (raw console bridge; no framing)
  |  terminal bytes via serial.in / serial.out rings
tty.fb0               (owns the shell session; unchanged rendering)
  |  term.input / term.output
msh                   (the shell)
```

- Host -> shell: bytes land in `serial.in.ring`; tty.fb0 merges them into
  msh's `term.input` exactly where keyboard input goes, bypassing the
  `ui.input` layout/lock policy (a serial console sends terminal bytes, not
  key usages).
- Shell -> host: tty.fb0 tees every byte it drains from msh's `term.output`
  into `serial.out.ring`; dev.serial returns them as CDC IN chunks. The
  framebuffer rendering is untouched — this is a copy, not a takeover.
- Wakeups: both producers (usb.fn.acm for host bytes, tty.fb0 for shell
  output) notify one console doorbell (`serial_console_db`), so dev.serial
  sleeps event-driven in both directions. Serial input wakes tty.fb0 through
  the shared `input_tty_db` doorbell.
- Backpressure: dev.serial retains one inbound chunk until the shell ring
  accepts it (the host sees USB backpressure) and one outbound slot while it
  drains; tty.fb0 drops (and counts) serial mirror bytes if the ring is full.
  Output produced with no host connection is dropped, never replayed stale
  across reconnects (chunks are stamped with the gadget's connection
  generation).

## Framed keycode protocol

The framed AMK1 keycode injector no longer runs on the CDC port; it lives on
the vendor **bulk** function (`dev.usb-input`, interface 2, PyUSB required):

```
python3 scripts/anne/usb-input.py --bulk text 'help' --enter
python3 scripts/anne/usb-input.py --bulk interactive
```

For the raw console from scripts, either use `screen` or:

```
python3 scripts/anne/usb-input.py console                 # interactive
python3 scripts/anne/usb-input.py console --send 'help' --enter --read 3
```

## Scope

- Profile: `huawei-anne-usb` only (the CDC device stack exists there).
- No kernel, ABI or msh changes: the service set gains `dev.serial` and
  tty.fb0 gains two optional endpoints, absent elsewhere (qemu/switch
  profiles keep the previous behavior).
- Not yet covered: terminal size negotiation (msh renders for the fb
  geometry), `^C` etc. behave as in the local terminal (the shell handles
  them), and hardware acceptance needs one native flash cycle from the Mac.
