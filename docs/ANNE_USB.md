# Anne native USB and command input

The `huawei-anne-usb` development profile connects a direct USB-C keyboard to
`drv.usb.dwc2 -> io.usb -> drv.usb.hid -> ui.input -> tty.fb0 -> msh`.
The normal `huawei-anne` profile remains the established recovery/console boot.

The USB-C keyboard plugs straight into the phone. The FUSB301 detects the
partner UFP, the role service requests host mode and the Hi6523 supplies a
500 mA source lease. The same connector attached to the Mac detects partner
DFP and selects peripheral mode. These uses are sequential on one controller.

Controller register/packet handling has no keyboard, terminal or charger code.
The standalone services are:

| Service | Authority and responsibility |
| --- | --- |
| `drv.i2c.dw` | DesignWare I2C PIO transactions; signed per-client address leases |
| `drv.typec.fusb301` | CC attachment, partner role and VBUS observation |
| `drv.power.hi6523` | 500 mA boost, fault checks and expiring source lease |
| `drv.usb.anne-phy` | Hi6250 PHY sequence and bounded settling delays |
| `io.usb.role` | Quiesce, power, PHY and controller role coordination |
| `drv.usb.dwc2` | PIO host/device packet execution and USC1/UST1 transport |
| `io.usb` | Host descriptors, interface claims and endpoint leases |
| `drv.usb.hid` | Boot/report keyboard descriptors, reports and LED encoding |
| `ui.input` | Source state, layout, modifiers, locks and repeat |
| `io.usb.gadget` | Standard EP0 requests, descriptors and function routing |
| `usb.fn.acm` | CDC control requests and opaque serial bytes |
| `usb.fn.bulk` | Opaque vendor bulk byte streams |
| `dev.usb-input` | Bounded diagnostic framing and its own input producer ring |

The controller defaults to PIO. Shared payloads and state are write-back CPU
arenas; ring release/acquire publication orders them. Registers use Device
mappings. No physical arena address is given to USB, and DMA is disabled.
A controller/PHY owner death asserts reset before capabilities and arenas are
reclaimed. The kernel exposes fixed board clock/reset masks rather than shared
CRG/PCTRL mappings. ABB gate changes use the firmware hardware lock.

The FUSB301/Hi6523 bus is `fdf0d000` — the kernel DT labels the controller
`i2c4` while Linux enumerates it as `i2c-2`. Its SCL/SDA pads are
GPIO_037/GPIO_038; mux value 1 selects the I2C4 function and 0 parks them.
`MX_PLATFORM_I2C_PINS` applies the function mux and pad-config defaults once
at I2C service startup and logs pad, clock and reset readbacks; before V7 no
native code programmed these pads.

The verified physical framebuffer is handed to `io.framebuffer` with consistent
Normal-NC aliases. Kernel screen painting stops at handoff while persistent
native tracing continues. The signed terminal view is 59 columns by 61 rows,
with origin `(64,224)` and 2x glyphs; terminal storage lives in an arena. Init
activates input explicitly rather than letting a log string control it. `msh`
needs no filesystem in this profile; filesystem commands report unavailable.
The early kernel diagnostic surface recycles a single text row at overflow;
it never scrolls the full scanout while holding the boot logger's IRQ lock.
The userspace terminal retains ordinary scrolling after framebuffer handoff.
Cold bundle CRC/SHA checks report bounded progress to the boot layer, which
feeds the inherited watchdog while CPU0's timer interrupts are masked. Every
signature and object digest is still checked before bundle publication.

## Build and bounded hardware capture

```sh
cmake --preset anne-usb-native-dev
cmake --build --preset anne-usb-native-dev --target anko mochi_system provenance
python3 scripts/anne/build-native-package.py --build out/anne-usb-native-dev \
  --output out/anne-usb-native-package
```

The preset requests 60 seconds of runtime after service readiness and then the
existing LPM3 reset transport. Packaging prepares the matching ELF, image and
verified full stock restore image; it performs no phone writes. Native execution
uses this phone's shared `kernel` partition. Restore stock before attempting to
return to TWRP. Follow [the established capture/restore procedure](../boards/huawei-anne/BOOT_DIAGNOSTICS.md).

## Mac key injection

After booting Ame, locate its `/dev/cu.usbmodem*` node. TWRP's ADB connection
is a different gadget; the native sender requires Ame's diagnostic function.

```sh
python3 scripts/anne/usb-input.py --serial /dev/cu.usbmodemNN text 'help' --enter
python3 scripts/anne/usb-input.py --serial /dev/cu.usbmodemNN key ctrl+c
python3 scripts/anne/usb-input.py --serial /dev/cu.usbmodemNN interactive
```

Interactive mode forwards the Mac terminal's keys; Ctrl-] exits. Command output
appears on the phone. Each command is acknowledged only after its key events
are accepted by the dedicated input ring. Disconnect/reset releases that source.
Physical HID and diagnostic sources have independent generations and sequences.

Optional vendor bulk uses `--bulk` and PyUSB/libusb, with interface 2 and
endpoints `0x04/0x85`. CDC uses Python's standard library. The development USB
identity is [pid.codes' private test PID](https://pid.codes/1209/0001/),
`1209:0001`; it is shared and requires an allocated identity for distribution.

## Hardware references and limits

Board sequences and chip registers come from the pinned
[Hi6250 vendor kernel](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/tree/65e545cbaa11d132e243287b150146bc2a51c45e),
including `dwc_otg_hi6250`, `fusb301`, `schargerV300` and `hisi_hwspinlock`.
The physical resources were checked against this phone's captured recovery DTB.
A non-destructive recovery register dump confirmed GSNPSID `4f54310a`,
GHWCFG1..4 `00000000`, `22affc50`, `0f80d4e8`, `fff08060`: 16 host channels,
16 bidirectional device endpoints, 3968 FIFO words, dedicated dynamic FIFOs,
and selectable 8/16-bit UTMI. Recovery uses the vendor default of 8-bit UTMI;
the native service makes the same choice when both widths are supported.
DWC2 field encodings also use
[Linux v6.12's register reference](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/dwc2/hw.h).

This implementation admits one directly attached USB2 device, multiple claimed
keyboard interfaces, and control/bulk/interrupt transfers. It does not advertise
hub/split, isochronous or DMA support. Type-C observation currently uses bounded
polling through the I2C service. The development gadget requires dynamic,
dedicated device FIFOs and enough endpoints, checked from GHWCFG before use.
Unsupported hardware geometry fails closed. Hardware acceptance still requires
observing both Mac ACKs and physical command entry on this phone; builds and
register-model tests cannot establish either result.

The first v2 hardware capture reached eight online CPUs but ended in a firmware
watchdog FIQ during bundle authentication, before USB services started. Stock
was restored and verified in TWRP. V4 added console/progress corrections and
cache snapshots. The root cause was a duplicate native CRG mapping that made
bootstrap silently continue with MMU/caches off. V5 fixes the mapping and stops
boot on MMU setup failure. It reached `tty.fb0: ready`; CDC/physical command
entry remain pending, and stock restore was acknowledged again. V6 named the
startup operations and the failing service, identifying `drv.typec.fusb301`
as the first casualty of the unprogrammed I2C pads. V7 muxes those pads,
names each Type-C/power startup stage and logs the controller id and abort
source; its capture is pending. Cortex-A53 CPUECTLR observations use
[Arm DDI 0500J, section 4.3.79](https://documentation-service.arm.com/static/6040c321ee937942ba301626);
they preserve firmware-controlled coherency and retention settings.
