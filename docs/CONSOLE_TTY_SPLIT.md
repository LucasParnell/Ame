# Console / TTY Split — Architectural Rules

## Rule: kernel log rings are diagnostic only

Kernel log rings (`CAP_SHARED_RING` granted at boot) carry raw kernel log
records. They are drained exclusively by `io.console`. No terminal service
(`tty.fb0`, any future shell) may read kernel log rings directly.

## Data flow

```
kernel log rings  →  io.console  →  UART (always)
                              └─→  tty.fb0 write endpoint (non-blocking mirror)

USB kbd  →  drv.usb.xhci  →  drv.usb.hid  →  ui.input  →  tty.fb0 (line discipline)

shell/msh stdout ring  →  tty.fb0 (render)  →  io.framebuffer (blit)
```

## Capability policy

| Service        | Allowed caps                                              |
|----------------|-----------------------------------------------------------|
| io.framebuffer | `"fb.memory"` CAP_MMIO                                   |
| tty.fb0        | io.framebuffer write endpoint + ui.input subscription + shell rings |
| io.console     | tty.fb0 write endpoint **only** (NOT framebuffer memory)  |
| drv.usb.xhci   | `"xhci.mmio"` CAP_MMIO + `"xhci.irq"` CAP_IRQ + `"xhci.dma"` CAP_ARENA |
| drv.usb.hid    | USB interface endpoint from xhci + ui.input write ring    |
| ui.input       | drv.usb.hid read ring + tty.fb0 write ring                |
| msh (shell)    | tty.fb0 stdin/stdout rings + restricted nameservice + io.fs |

## Invariants

- `io.console` never holds framebuffer memory.
- `io.framebuffer` is the only service that maps `"fb.memory"`.
- `tty.fb0` never reads kernel log rings.
- msh (the shell) must never see USB MMIO.
- Typed characters never appear in kernel log rings or UART output.

## Why

Mixing kernel diagnostic output with interactive terminal state couples the
log facility to terminal session lifecycle. A crashed terminal must not silence
kernel logs. A kernel log flood must not corrupt terminal state.
