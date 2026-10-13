# Handoff — evening session (2026-10-13, 19:10 → 22:20): Anne USB native bring-up

Self-contained summary of everything this session touched. The day handoff
(`docs/HANDOFF_2026-10-13.md`, sections V7–V14) has the blow-by-blow detail;
this file is the one to read first.

## Continuation: V18 Mac acceptance and V19 cleanup

Current phone state: **V18 native image is installed. Stock is not restored.**
The Mac enumerated `Ame USB diagnostics`, bound CDC ACM, and the standard-library
sender acknowledged `help` plus Enter. The user confirmed the command list
appeared on the phone. The Mac required its accessory approval. A physical
USB-C keyboard has not been checked; the user could not connect it then.
Private evidence is in `out/anne-device-private/usb-native-v18-test/`.
The V18 image SHA256 is
`f92a53a336514247ad3ad8ecd40a4c8ab48dff9a4cee9209671e2f8f2d0b4666`.

The user now requested readable, commented USB cleanup with DOP, cache/IPC
and scheduling attention, and debug text disabled. V19 changes are written;
validation/package preparation is underway, and it has not been flashed.
The existing direct native-to-native flash authorization persists. Restore
stock only before returning to TWRP/Android. Never use `reboot recovery`:
it enters eRecovery; use the established bootloader/button route.

## Earlier continuation: V15–V17

- V14 was restored, and TWRP verified the live stock checksum. Recovery clock
  readout showed HCLK 240 MHz and USB/ABB reference 19.2 MHz. FUSB301 dump showed
  ID `12`, mode `04`, status `2d`, type `08`: attached Mac with VBUS. Recovery
  has no `/dev/mem` and the USB debug directory only exposes device inventory;
  attempts to read PHY/core snapshots there did not succeed.
- V15 added full board-PHY readout without an electrical change. Live state:
  `ctrl0=44 ctrl1=0 ctrl2=bd ctrl3=6b866db ctrl4=202 ctrl5=c22`,
  `bc3=0 bc4=c01 bc5=0 bc=c20`. No Mac USB device or CDC node appeared.
- V16 selects the vendor's PHY IDDIG source for device mode (`ctrl0=14`);
  host mode retains software ID=0 for Type-C. Fresh user readings confirmed
  `ctrl0=14` and the same other controls, with `bc=20`. No Mac enumeration.
  The user's first V16 paste repeated V15 exactly; it was excluded from V16
  evidence and a fresh readout was obtained. Private run kits contain both
  Mac observation and accepted live-screen evidence.
- The user asked why an equivalent Linux driver was not being used. A stricter
  startup comparison found omissions, documented in
  [ANNE_USB_REFERENCE_AUDIT.md](ANNE_USB_REFERENCE_AUDIT.md).
- V17 corrects post-layout FIFO flush, a bounded asynchronous disconnect/settle
  interval, post-reset and ENUMDONE UTMI timing, ENUMDONE global IN NAK clear,
  and revision-3 SETUP receive credits without rewriting an enabled OUT receive.
  It adds controller-local reset/ENUMDONE/SETUP/command/completion counters and
  EP0 register readbacks. Board/HID/input/terminal responsibilities stay separate.
- After implementation, native build and all four ASan/UBSan controller/HID/
  debug/device probes in `test_dwc2_and_hid_report_state_machines` passed.
  A model verifies disconnect through flush/settle, quarantine on stuck flush,
  revision-dependent timing, and cumulative event counters. Diff check clean.
- Prepared package: `out/anne-usb-native-package-v17`, run tag `anne-usb-v17`,
  native image SHA256
  `52e3e7cb9d4e63a1cbb30569b41b7dd113d9eaacd33a6108831c0e480173c0f9`.
  Hash-pinned controller and observer:
  `out/anne-device-private/usb-native-v17-test/{control.py,observe.py}`.
  Observer checks the correct IOService USB device query and CDC nodes for
  65 seconds; if an Ame device and one CDC node appear, it sends `help` + Enter
  and saves the ACK result. The V17 package is locally AVB-verified. FASTBOOT target/unlocked preflight,
  flash and reboot were acknowledged. The 65-second Mac observation window
  finished with no enumerated Ame device or CDC node, so injection did not run.
  Fresh V17 readings subsequently showed 36 reset/ENUMDONE events, eight SETUP
  requests and eight IN completions. This localized the fault to enumeration.
  V18 stages ADDRESS before the status SEND and handles PIO FIFO OUTDONE plus
  early EP0 OUT retention. Mac command input succeeded as recorded above.

The historical V14 state below is superseded by this continuation.

## TL;DR / where this stands

- Goal of the arc: get the full native USB stack booting on the phone and the
  **USB device (CDC/ACM) attach** working, for the Mac-side acceptance tests.
- **Done tonight (V7–V10): the boot chain went from "fusb301 dies at startup"
  to "full service graph up, role=device, stable runs."**
- **Open: host-side attach.** macOS never registers the phone as a USB device
  (verified live across v10–v14). Targeted fixes + live diagnostics are in
  place; the next session starts from the precise open questions below.
- **Phone state first (IMPORTANT):** the last flashed image is **v14**
  (no auto-reset — it may still be sitting in the Ame console), and **stock is
  NOT restored for that cycle.** Recovery: hold Power ~15 s → VolDn+Power →
  FASTBOOT → `cd out/anne-device-private/usb-native-v14-test &&
  python3 control.py restore` → `control.py bootloader_reboot` → button route
  to TWRP.

## Commit chain (master, oldest → newest)

- `759983c` WIP: controller/bus/HID split + Anne native stack, platform control
  (the day's uncommitted tree — first action tonight)
- `09fe9b7` V7: i2c4 pad mux (GPIO_037/038) + named startup stages —
  **fixed the fusb301 blocker**
- `7c0672b` V8: named role-transition stages + kernel log `%x` fix
- `34f0401` V9: REG_TYPE mapping fix (bit3→Device, bit4→Host); ABB reference
  best-effort with raw lock logging
- `e8a3ef4` V10: bound the PHY idle wake (zero-deadline sleep bug) —
  **unblocked the whole role chain**
- `cbc1eb9` docs: V10 capture result (device mode end-to-end, ABB acquired)
- `a519f00` V11: disable the diagnostic auto-reset (dev runs stay up)
- `e9dab2a` V12: PCGCCTL "restart PHY clock" + post-connect bus readback
- `41176bb` V13: live bus-state readout every 10 s for no-reset runs
- `45d5a1d` V14: force GOTGCTL VBValid/BValid overrides + PHY connector-view
  readout (`bc_sts2`)

## What each iteration found (condensed)

- **V7 (pads):** the fusb301 startup failure was the never-programmed I2C4
  pads. Vendor DT: mux e896c088/8c ← 1; cfg 0x894/0x898 ← 0. New kernel op
  `MX_PLATFORM_I2C_PINS` (9). Capture showed `controller comp=0x44570140` and
  full service readiness.
- **V8 (names):** role-transition stall named: `phy-set` "no reply";
  kernel formatter has no `%08x` (use `%x`).
- **V9 (REG_TYPE):** vendor `fusb301_detect_port_mode()` mapping is bit3
  ("UFP") = DFP host attached → **our port is the device**; bit4 ("DFP") =
  UFP partner → our port is host. We had it inverted (took Host on the Mac).
- **V10 (the real stall):** `drv.usb.anne-phy` passed `r->next` (absolute,
  0 right after accept) to `driver::idle()` → `wait_doorbell_until(..., 0)`
  = unbounded sleep; the PHY never ran a single step, faulted at the expired
  deadline with stale replies. Fix: bound the wake (`next` only when future;
  else +1 ms busy / +1 s idle). First ever full run: role=device, zero
  faults, ABB lock acquired first try (`usb ref status=0/30 acquired=1`).
- **V11 + V13 (retention discovery):** trace retention is driven BY the
  capture reset ("retain trace by resetting"). With capture off, runs write
  zones to RAM only; a power cycle loses them (v11/v12 recorded nothing).
  No-reset builds now use live on-screen readouts instead.
- **V12 (PHY clock):** vendor `dwc_otg_core_dev_init()` starts with
  `PCGCCTL = 0` ("Restart the Phy Clock"); PCGCCTL survives core soft reset.
  Wrote it in `device.activate()`.
- **V14 (session force):** core read `GOTGCTL.bsesvld=0`; forced the
  VBValid/BValid override bits (2/3, 6/7) → `bsesvld` flips to 1 on-device.

## Hardware truth established tonight (device-verified)

- FUSB301 @ `fdf0d000` ("i2c4" in DT naming, sysfs i2c-2), addr 0x21;
  Hi6523 @ 0x6B. Both live under recovery.
- I2C4 pads GPIO_037/038 as above; clock gate PEREN2 bit27 / reset release
  PERRSTDIS2 bit27 (matches vendor `reset-controller-reg`).
- hwspinlock (lock 9): pctrl 0xe8a09000 + 0x40c (request) / 0x410 (unlock) /
  0x414 (status), command `3<<4`; verified against `hisi_hwspinlock.c` + DT
  `hwlock,offset`. Acquires first try once actually called.
- PHY AHB window registers: ctrl0=0x0, ctrl1=0x4, ctrl2=0x8, ctrl3=0xc
  (eye pattern: host 0x073f679f / device 0x06b866db), ctrl4=0x10,
  ctrl5=0x14, bc_ctrl0..8=0x18..0x38, bc_sts0..4=0x3c..0x4c
  (**bc_sts2=0x44**: [0] vbus_valid, [1] sess_valid, [2] dp, [3] dm,
  [4] chg_det, [5] iddig, [12-13] linestate).
- Core register facts: GUSBCFG[29]=force_host / [30]=force_dev;
  GOTGCTL bits 2/3 = VBValid override, 6/7 = BValid override, [16] conidsts,
  [19] bsesvld; PCGCCTL low bits = gating; CRG USB bits (CLK_EN4 0x40 /
  RSTDIS4 0x94): HCLK=1, REF=2, PHY_N=8, USB2OTG=9, MUX=10, AHBIF=11, PHY=12,
  PHYPOR=13, 32K=14, ADP=27.
- **Known difference vs vendor**: our ctrl0 uses `id_sel=0` (id forced from
  register, id=1); the vendor default writes `id_sel=1` (id from PHY iddig).
  Candidate A/B experiment.

## Open problem (attach) — all evidence

- Mac never registers the device across v10–v14 (live macOS log stream,
  `ioreg -p IOUSB`, `/dev/cu.*`, both ports; macOS USB works — fastboot
  attaches normally).
- Phone side (v13/v14 live readouts): `dctl=0` (soft-connect on);
  `pcgcctl` gating clear; `gusbcfg=40001400` (force-device + turnaround);
  `otgctl=4c900cc` after V14 (overrides + bsesvld=1); `dsts` shows an
  advancing counter (≈8 kHz if HS µframes, mod 16384 — possibly a free-run
  artifact; unresolved); PHY view: `ctrl0=44 ctrl2=bd ctrl3=06b866db`,
  and **`bc=20` → vbus_valid=0, sess_valid=0, iddig=1**.
- **Unresolved questions (ask/verify first):**
  1. Was the phone actually plugged into the Mac during these runs? Several
     replugs happened; `bc` showed no VBUS whenever sampled. Do one clean,
     confirmed test: phone on a Mac port, read `bc=` on-screen within 10 s,
     cross-check the Mac watchers. (Last exchange: user said "replugged it";
     no follow-up reading collected.)
  2. If `bc` still reads vbus=0 on a live host port → PHY VBUS-sense path
     work (candidates: id_sel=1 to match vendor; bc detection enables;
     ctrl1 `scaledown_mode`; ctrl4 opmode/xcvrselect/termselect area).
  3. If `bc` reads vbus=1 but the Mac sees nothing → pull-up/physical attach
     path (SFTDISCON connect pulse; termselect; id_sel A/B; compare full
     ctrl state vs vendor's normal-mode set).

## Next diagnostics (v15 candidates)

- Extend the PHY live line to dump ctrl0/1/2/3/4/5 + bc_sts2 + bc_ctrl4/5.
- A/B `id_sel=1` (vendor default) vs current forced-register id.
- Consider a connect pulse (SFTDISCON 1→0) after the session force.
- Keep the live-screen protocol; only re-enable capture (60 s) when a
  retained trace is truly needed.

## Tooling & procedures (workspace)

- Run kits: `out/anne-device-private/usb-native-vN-test/control.py` —
  actions: `preflight` / `flash` / `reboot` / `restore` / `bootloader_reboot` /
  `recovery_reboot`. Hash-pinned; state.json tracks acknowledgements.
  Standing note: capture/restore approval is already given — do NOT re-ask;
  do verify target, hashes, and actual transport. TWRP route from fastboot:
  `fastboot reboot bootloader` then the on-phone button route (never
  `reboot recovery`).
- Collection (TWRP): `python3 scripts/anne/collect-native-log.py
  --package out/anne-usb-native-package-vN --output
  out/anne-device-private/usb-native-vN-test --dfx-fallback` (also verifies
  the restored stock kernel hash).
- Mac watchers (currently RUNNING; kill via the pid files if needed):
  `out/anne-device-private/usb-watch.sh` (basic; pid `usb-watch.pid`),
  `usb-watch-v2.sh` (full IOUSB snapshots; pid `usb-watch-v2.pid`).
- macOS live USB log command that works:
  `log stream --style compact --predicate 'eventMessage CONTAINS[c] "USB" OR
  eventMessage CONTAINS[c] "AppleUSB"' > out/anne-device-private/mac-usb-live.log`
- Mac sender (not yet exercised): `python3 scripts/anne/usb-input.py
  --serial /dev/cu.usbmodemNN text 'help' --enter` (stdlib only; the phone's
  ACK is the acceptance signal).
- git identity: commits are auto-derived (`lucasparnell@Lucias-MacBook-Air.local`;
  no user.name/email set). Mention before "fixing" — it's cosmetic.

## Artifacts produced tonight

- Packages `out/anne-usb-native-package-v7 … v14` (hash-pinned; each has
  SHA256SUMS + AVB metadata).
- Run kits + collected traces:
  v7 (4 records), v8 (4), v9 (2), **v10 (4, two runs — the "device mode
  end-to-end" capture)**, v11–v14 (no retention — screen dumps pasted in
  chat; the v13/v14 dumps are the current evidence).
- Research: `out/anne-usb-research/` — new fetches tonight include
  `soc_crgperiph_interface.h`, `hisi_6250_pinctrl.dtsi`,
  `hisi_i2c_designware.c`, `hisi_6250_i2c.dtsi`, `hisi_6250_clock.dtsi`,
  `soc_pmctrl_interface.h`, `soc_pctrl_interface.h`, `soc_sctrl_interface.h`,
  `dwc_otg_cil_intr.c`, `otg_dual_role.c`, `hisi_usb_vbus.c`,
  `dwc_otg_pcd.c`, `dwc_otg_pcd_intr.c`; plus `dt/` (full device-tree dump
  from the phone). `i2cprobe.c` was staged but unused (recovery kernel has
  no i2c-dev).
- Docs updated: `ANNE_USB.md`, `USB_PLAN.md`, `FEATURE_STATUS.md`,
  `PROGRAMMER_ABI_V0.md`, `HANDOFF_2026-10-13.md` (V7–V14 sections), and
  this file.

## Immediate next steps (ordered)

1. Confirm phone state (restore stock if the v14 run is still up).
2. Controlled attach test: phone on a **confirmed** Mac port → read the
   on-screen `bc=` line within 10 s → cross-check the Mac watchers. Record
   both sides.
3. If vbus still 0 on a live port → v15: full PHY register readout +
   id_sel=1 A/B + VBUS-sense investigation.
4. If vbus ok but still no host attach → v15: pull-up path experiments
   (connect pulse, termselect, id_sel).
5. When attach works: exercise the Mac sender (`usb-input.py`), then the
   physical-keyboard acceptance, then continue per `docs/USB_PLAN.md`.
