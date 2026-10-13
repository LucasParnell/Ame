# Anne USB device startup against the vendor driver

Reference commit: `65e545cbaa11d132e243287b150146bc2a51c45e` in
[the Hi6250 vendor kernel](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/tree/65e545cbaa11d132e243287b150146bc2a51c45e).
The local reference copies are in `out/anne-usb-research/`. This comparison
uses the controller's PIO path; copying Linux's descriptor-DMA setup would
require different buffer authority, lifetime and cache maintenance.

| Operation | Reference | Ame before V17 | V17 correction |
| --- | --- | --- | --- |
| PHY clock/reset sequence | `dwc_otg_hi6250.c:init_usb_otg_phy_hi6250` | Clock/reference, AHB, POR, PHY and core steps already separate from DWC2 | No new electrical change; retains V16 device ID selection and readout |
| PHY clock restart | `dwc_otg_cil.c:dwc_otg_core_dev_init` | PCGCCTL cleared since V12 | Retained |
| UTMI bus and turnaround | `dwc_otg_cil.c:dwc_otg_core_init`; `dwc_otg_pcd_intr.c:dwc_otg_pcd_handle_enum_done_intr` | Configured before reset only; no ENUMDONE timing update; live TRDT readback was 5 with 8-bit selection | Restore selected configuration after reset; use revision/width timing and update it at ENUMDONE |
| FIFO geometry then flush | `dwc_otg_cil.c:dwc_otg_core_dev_init` | Reset flushed before the new device FIFO layout was written | Flush the newly configured FIFOs before connecting; bounded asynchronous wait |
| Explicit disconnect interval | `dwc_otg_hicommon.c:start_peripheral` | SFTDISCON held only for the register-construction interval | Keep disconnected through FIFO flush and at least 150 microseconds of settling; no spin delay |
| EP0 activation | `dwc_otg_cil.c:dwc_otg_ep0_activate` | EP0 configured, but no global nonperiodic IN NAK clear at ENUMDONE | Issue CGNPINNAK on ENUMDONE |
| SETUP receive credits | `dwc_otg_pcd_intr.c:ep0_out_start` | Three credits with transfer size 64; rewrote a potentially enabled OUT endpoint | Three eight-byte credits; revision 3.x preserves an already enabled OUT receive |
| DMA registers and descriptor RAM | Vendor DMA branches | PIO; no physical buffers or DMA authority | Intentionally remain PIO; no descriptor-DMA register writes |

The observed V16 control state was `ctrl0=14`, `ctrl1=0`, `ctrl2=bd`,
`ctrl3=6b866db`, `ctrl4=202`, `ctrl5=c22`, `bc3=0`, `bc4=c01`,
`bc5=0`, `bc=20`. macOS exposed neither an Ame USB device nor a CDC node.
That establishes failed enumeration; frame-counter movement alone does not
establish successful host traffic. V17 adds cumulative reset, ENUMDONE, SETUP,
command and completion counters plus EP0 register readback to distinguish
physical connection from control-transfer failure.

These observations do not establish that any one omission caused the hardware
failure. Acceptance still requires Mac enumeration and acknowledged key input,
followed by physical keyboard command entry. The user requested that Mac input
be resolved first. Intermediate native-to-native tests can flash directly in
FASTBOOT; stock restoration is needed before TWRP/Android, not between those
test images.

## V17 evidence and V18 control-transfer correction

The V17 phone readout established actual host traffic: 36 bus resets and
ENUMDONE events, eight SETUP requests, eight completed IN packets, and no OUT
completions. UTMI readback was corrected to `40002400`; DCFG ended at `10`
(address one). macOS still exposed no enumerated Ame device or CDC node.
This localizes the remaining failure to control enumeration rather than a
missing electrical connection. Counts alone do not identify the last request;
V18 also prints its raw SETUP fields.

The vendor's `do_set_address()` prepares DCFG before enabling status IN.
[Linux v6.12 does the same](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/dwc2/gadget.c).
Ame previously scheduled ADDRESS only after the status completion crossed back
to the gadget process. V18 stages ADDRESS before SEND on the same ring. DWC2
prepares the register with status IN and commits its software address on ACK;
an aborting SETUP discards or rolls back the uncommitted stage. A simultaneously
observed previous ACK is processed before the next SETUP.

PIO OUT completion is now accepted from the RX FIFO's OUTDONE record, with
endpoint XFRC handled as a duplicate-safe fallback. EP0 retains one admitted
packet in its private arena until the consumer posts RECEIVE. This covers a
status ZLP or control OUT data arriving during a process-hop delay. It preserves
SETUP credits in OUT transfers and waits for SETUP phase completion before
publishing the request. Reset/new SETUP invalidates the retained row.

The standalone gadget process now owns only construction and its event loop;
`userland/usb/gadget/control.h` contains controller-neutral EP0/function policy.
The production controller and policy are exercised together through real SPSC
rings. Tests cover address/status ordering, command backpressure, an aborting
SETUP, ACK plus new SETUP in one poll, early OUT status/data, duplicate
completion suppression and reset invalidation. Native build and five
ASan/UBSan USB probes pass after implementation. Hardware V18 acceptance is
pending; no keyboard handling moved into the controller.
