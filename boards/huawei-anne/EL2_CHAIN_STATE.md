# EL2 handoff — state of the work

Updated 5 November 2026. Target: ANE-LX1 8.0.0.151(C782), Kirin 659 / Hi6250.
Stock LK image: `fastboot.img`, 4,194,304 bytes, SHA-256 `16e307a5…bee52`.
Addresses are image-relative unless prefixed with the load base `0x10000000`.

## Current DTS result (5 November 2026)

The earlier sections below are dated investigation snapshots; their statements
that no DTS consumer was proven or that DTS had not been written are superseded
by this result. See [the DTS loader vulnerability report](dts-loader-vulnerability.md)
for the full root-cause and evidence write-up.

- The handset board ID is 4970, selecting HSDT entry 262/key `04090700`. The
  earlier entry-0/key-`04000007` candidate targeted board ID 4007 and booted
  with all CPUs at EL1 and `monitor_ready=0`.
- Corrected candidate
  `out/anne-device-private/dts-el2/pinctrl-board4970-v3/dts.img` has SHA-256
  `8a611174e7524a03064799055ddc5201143e34f54dfe41ab64b90e26adc87fd9`. Stock
  loader replay and the focused pinctrl/normalboot handoff harness reached
  verifier return 0, patch word `0x52807921` at `0x10b634`, and SPSR `0x3c9` at
  `0x35e80040`.
- The candidate was flashed to DTS in TWRP and the complete 29,360,128-byte
  partition readback matched that hash. After booting Ame,
  `diag el2` reported `boot_entry_el=2` and `monitor_ready=1` on all eight CPUs.
- The selected row sets `verify_len=0`; loader acceptance is the explicit
  “Verify Not Exist” path, not a valid cryptographic proof result. The stock
  proof bytes and offset remain in the container but are not verified for this
  row.
- One successful boot is recorded. Three consecutive cold boots, direct stock
  BL31/TEE partition hash readback and Dango hello guest execution remain
  unverified. `vm start` and `vm console` report that runtime manager commands
  are not implemented.
- At this update, Ame is running the prior package image
  `out/huawei/packages/20261102T204427130376Z/ame-native-kernel.img`, SHA-256
  `8bbfad072acfa5a61868f2ba1794ab99b4ac734c72bbf48ef81beaf19461af7c`. A
  rebuild from the current working tree, including untracked build inputs, is
  pending.

The stock DTS restore image is
`boards/huawei-anne/stock-backups/anne-dts-backup.img`, SHA-256
`269fd6ddc8892470916761b373f2063aaecd4bdfa8a503946dbea76fce37ccad`. The
previous DTS image and stock kernel were read back before the candidate write;
readbacks and hashes are saved under
`out/anne-device-private/dts-el2/phone-capture-20261105-prewrite/`. The restore
procedure is in [operations](operations.md#dts_partition).

The following §§ 1–9 remain as historical notes, not current state.

LK, XLoader and dts have not been written. Earlier device work restored the single
corrupted boot0 byte (`0x74c`, `ef` → `ee`) and wrote the `kernel` partition (§5), both
read back and verified.

## 1. Summary

| route | state | wall |
| --- | --- | --- |
| LK image on flash (`0x10b634` → `0x3c9`) | patch built, verified | VRL content hash |
| XLoader in boot0, patching LK at load | patch built, verified in Thumb emulation | XLoader signature → BootROM |
| dts inflate overflow | real 35,126,800-byte output reaches `0x3057fe10` | post-inflate `>0x200000` guard rejects the entry; normalboot skips BL31 dispatch |
| dts verify read (`vlen` up to `0x200000` → `0x1000` allocation) | controlled read tail | reaches later arena-A allocations; no handoff consumer proven |
| OPTD memset (`w4+0x8000` vs `w4+8`) | live-object overwrite | zeros only, victim is our own buffer |
| RAM LK via BootROM | **already working** | none — current host-tethered path |

The long inflate and logo overflows do not reach a useful earlier victim. The separate
proof read is a new lead because its overrun lands before later arena-A allocations, but
no indirect-call consumer has been demonstrated.

## 2. LK handoff patch

Exactly one site in 796,272 instructions loads the EL1h request:

```
0x10b634  a1 78 80 52   mov w1,#0x3c5        EL1h
0x10b638  65 04 00 f9   str x5,[x3,#8]
0x10b640  b9 00 10 01   str w1,[x0,#0x10]    x0=0x35e80030 -> 0x35e80040
```

Patched to `21 79 80 52` (`mov w1,#0x3c9`, EL2h): 2 bytes differ, size unchanged, the
following instruction byte-identical. Executed through the real routine, `0x35e80040`
receives `0x000003c9` versus `0x000003c5` stock. Patched sha256 `d236eff2…12b46a`,
identical to the candidate already in `FASTBOOT_EL2.md`.

`out/anne-device-private/lk-el2-patch/fastboot-el2h-hint.img`. Rejected by VRL on boot.

## 3. XLoader patch (patch LK after verification)

XLoader is **Thumb-2**, base `0x20000`, 70,912 bytes, sha `07c2ec41…`. The LK destination
is a modified immediate, not a pool literal:

```
0x211b8  mov.w r0,#0x10000000
0x211bc  bl    0x27438           ; load wrapper
0x211c4  adds  r0,#0xf           ; <- redirected
```

Injection: 4-byte `b.w 0x2ee32` at `0x211c4`; stub in an unreferenced zero cave at
`0x2ee32` (0x4e bytes, zero xrefs) that writes `0x52807921` to `0x1010b634`, replays
`adds r0,#0xf`, then branches to `0x211c8` or `0x21200` on flags.

Verified in Unicorn: `0x211c4 → 0x2ee32 → str → 0x211c8`, no fault, target becomes
`0x52807921`. 30 bytes changed, sha `ac59340a…250ce9`.

Not usable in boot0 (signature → BootROM). Usable only as a RAM upload over BootROM.

## 4. dts route

`dtb_init 0x6f74c` → alloc 4 MiB scratch (`0x11173c`) → `get_dtb_entry 0x6eb44` →
alloc 2 MiB → inflate `0xcfac0` → `dtb_verify 0x6e984`.

Arena B, measured: scratch `0x2e000000`, output `0x2e400000`, window/state `0x2e600000`.

**Input pointer is derivable** — this corrects an earlier claim that it needed a leak:

```
0x6ed3c  ldr x2,[ctx,#0x20]         ; scratch
0x6ed24  add x27,x27,#1,lsl #12
0x6ed28  add x27,x27,#0xb
0x6ed30  and x27,x27,#~0xfff
0x6ed48  add x27,x2,x27             ; input = scratch + aligned(off+0x100b)
```

Captured state on a clean 2 MiB decode confirms it: `+0x00 = 0x2e2020b4`,
`+0x18 = 0x2e600000`, `+0x38/+0x40 = 0x2e600060`.

**The primitive that nearly worked.** On short stored-block streams the decoder calls a copy
routine at `0x111a78`, and the final call is `memcpy(dst=table[0], src=buffer_end+7, len=7)`
— controlled destination, source inside our own overflow payload. `table[0]=0x1010b634`
would write `21 79 80 52 | 65 04 00` over the handoff instruction exactly.

It fires only when the decoder state is self-consistent for the stream being decoded. A
zeroed struct suppresses it; writing the patch bytes at `+7` corrupts field `+0x08` and
suppresses it; at 2 MiB the routine is never called at all (0 calls). The copy exists only
on short streams, and short streams cannot overflow far enough to reach the state.

**Second bug.** `dtb_verify` reads `vlen` (`0x1800` stock, up to `0x200000`) into a `0x1000`
allocation at `0x6e9c4`. In the measured preboot layout the proof buffer is at `0x30004480`;
the next arena-A allocations are the RPMB HMAC and frame buffers at `0x30005480` and
`0x30007800`. The proof tail can therefore prefill later allocations. The downstream
consumer and a surviving control-flow effect remain unproven.

**Third bug.** `0x6f454` reads an 8-byte `OPTD` record, takes length `w4`, checks
`w4+8 <= 0x400000`, then `memset(scratch+8, 0, w4+0x8000)` — up to `0x8000` past the 4 MiB
scratch, into the dtb buffer allocated moments earlier. Live object, but zeros only.

## 5. Device state at end of session

- `kernel` (24 MiB): flashed from `erecovery_kernel`, readback verified `ad00caaa…b55cc268`.
  Now **stock Huawei 4.4.23+**, not Dango. Previous content saved at
  `out/anne-device-private/kernel-before-stock-flash.img` (`8bbfad07…`).
- `recovery_ramdisk` (32 MiB): contains **TWRP** — 571 `TWRP` strings, 13 `teamwin`,
  `twrpback::streamFileForTWRP`. That pairing is why stock 4.4 was needed; Dango 6.12 could
  not boot it.
- `erecovery_kernel`: stock 4.4 boot image, kernel gzip `0xb208ac`, load addr `0x480000`.

## 6. What would unblock

1. Identify a post-proof allocation whose attacker-prefilled field is later consumed as a
   control-flow pointer, while preserving its neighbours and the normal boot path.
2. Confirm that the real verifier accepts the preserved stock proof prefix with an enlarged
   `vlen`; the current replay forces verifier success and does not establish this.
3. Reproduce the candidate consumer and return-to-handoff in emulation with the relevant
   hardware services modeled; do not use the phone before that gate passes.

## 7. REA normalboot trace (3 November 2026)

This supersedes the earlier assumption that a failed `dtb_init` is harmless to the
normalboot handoff. The loader itself tolerates the failure, but normalboot later
requires the dts module's returned FDT pointer.

- `FUN_0006eb44` inflates into the 2 MiB allocation without a destination bound, then
  checks `local_4 > 0x200000`. An overlong output returns zero. `FUN_0006f74c` only stores
  the pointer in `_13896_8` on the successful path, so this leaves the dts service's
  `FUN_0006e080` getter returning zero.
- Normalboot's `FUN_00090f30` calls that getter and returns `0xffffffff` when it gets
  zero. The normalboot callback at `0x911fc` branches through the error block at
  `0x9140c` to `0x91334`, returning before its `bl FUN_000910d0` at `0x91330`.
  `FUN_000910d0` is the BL31 dispatcher; its `trustfirmware + 8` entry is `0x10b5b8`.
- The FDT is otherwise consumed by `FUN_00090f30` as a fixed `0x200000`-byte copy.
  That is the returned buffer, not the overflow tail.
- The previously identified `0x3989c` indirect dispatch is inside
  `FUN_0003985c` (`usbloader_ultraflash`), a command path that requires a host command.
- The real-inflater replay at
  `out/anne-device-private/dts-el2/consumer-marker-v3-actual-normalboot-v3/report.json`
  reproduced the `0x3057fe10` overflow end, recorded dts failure and `dtb_pointer=0`,
  and hit the `0xcbf9c` AVB partition-read callback twice. The only allocation-field
  dispatch candidate was the later 80-byte object at `0x3057fd60`; its `+0x18` field
  was restored to the static AVB callback. The replay hit its instruction limit before
  `0x10b5b8` or `0x10b634`.

Conclusion: the current >2 MiB dts overflow cannot reach the normalboot BL31 handoff.
Do not write the dts partition or try the device path until an alternate consumer or
an input that preserves the dts pointer is proven offline.

## 8. Proof-read overrun and BL31 entry-mode trace (3 November 2026)

The `dtb_verify` overread is a distinct primitive from the rejected long inflate. REA
pseudocode for `FUN_0006e984` shows a `0x1000` allocation followed by a storage read of
the table's `vlen`, with `vlen < 0x200001`. Its verifier callback is
`FUN_001279f8`; that routine copies a fixed `0x1000` bytes from the proof buffer into its
verification workspace. The oversized tail is not passed as a length to that callback.
This makes a preserved stock proof prefix plus an unsigned larger `vlen` structurally
plausible, but the real crypto verifier has not yet been shown to accept the candidate.

Offline candidate `out/anne-device-private/dts-el2/proof-overread-stocktail-v2.img`
uses `vlen=0x10000`, preserves the stock proof's first `0x1800` bytes, and zero-fills the
rest of the read. The forced-verifier replay reports `dtb_pointer=0x2e400000` and the
stock `392205`-byte FDT, so it confirms the read/allocator layout only. In the replay with
real pre-dts initializers, the proof buffer is allocated at `0x30004480`; later allocations
start at `0x30005480` (9088 bytes) and `0x30007800` (16384 bytes). The full preboot replay
stopped in later FDT-dependent hardware initialization at an unmodeled MNTN access before
normalboot. It found no dispatch candidate and did not reach `0x10b5b8`. This is not a
successful exploit replay or a real-verifier result.

The decrypted BL31 artifact used for the separate entry-mode check is
`out/anne-device-private/el2-firmware/trustfirmware-decrypted.bin`, SHA-256
`27c06575c5af056fdbedc9385d4bd3c460459f5075ee79dcb65762f71fa5eb85`, analyzed at
`0x35e00000` with REA/Ghidra. Its `entry` routine writes `SCR_EL3=0x238`, with HCE clear.
`FUN_35e070d0`, called while initializing the next-image context, inspects the supplied
SPSR at entry-info `+0x10`; for AArch64 EL2h (`0x3c9`) it sets bit `0x100` (SCR_EL3.HCE)
in the saved context. `FUN_35e0724c` then follows the HCE-set context path and initializes
EL2 state. Therefore an HCE-only change before BL31 is not preserved; the supplied
EL2h SPSR is the relevant selector. This is static analysis of the decrypted artifact,
not proof of a device transition, secondary-CPU entry, or guest execution.

## 9. Real pre-dts initialization and verifier replay (3 November 2026)

The direct `dtb_init` replay's null secureboot workspace was a harness omission: the real
module dispatcher runs `partition` (priority 20) and `secboot` (priority 53) before `dts`
(priority 54). Keeping `secboot` real allocates its 4 KiB verification workspace before
the proof buffer. Keeping `partition` real also supplies the address range checked by
`FUN_00125064`; with it stubbed, the stock verifier returns `0xffffff01` at that range
check.

The stock-proof replay with both initializers real is
`out/anne-device-private/dts-el2/stock-real-verifier-secboot-partition-v1/report.json`.
It gets through the certificate parser's address-range checks (`0x127abc` and `0x127ad8`
return zero), then times out at `0x7dd40` during the remaining hardware-backed verification.
The replay explicitly stubs `FUN_0006fda4` (efuse word reads), supplies synthetic LCS 5,
and marks observed crypto status waits ready. It is not a real efuse/crypto result and does
not establish proof acceptance. Reads of `vrl` and `vrl_backup` also fail because no local
partition captures were supplied.

The next offline gate remains a completed stock-proof control using captured `vrl` inputs
and a verifier run that does not substitute efuse, lifecycle, or crypto results. The proof
read tail still has no demonstrated downstream control-flow consumer, so the device path is
not confirmed and no dts write has been made.

## 10. Corrected proof-read provenance replay (3 November 2026)

The `vlen=0x200000` replay with the local `dfx`, `trustfirmware`, `kernel`, `ramdisk`, and
`teeos` captures is
`out/anne-device-private/dts-el2/proof-overread-2m-mode1-realpartitions-v3/report.json`.
It uses the forced-success DTB verifier and synthetic hardware mappings, so it is a flow
trace rather than a proof-acceptance or device result. The FDT getter returns the normal
`0x2e400000` pointer and 392205-byte DTB. Normalboot reaches the first
`rescue_recovery_kernel` operation at `0x90968`; it does not reach the later callback at
`0x911fc`, the FDT copy helper, or the BL31 dispatcher before a memory fault at
`0x1400000614000007`.

This replay corrected a false positive in the earlier byte-range taint report. It had
reported reads at `0x10bb54`, `0x10e0d0`, and `0x10e128` as observing dts-overread bytes at
`0x3014caa0`. That address is subsequently overwritten by the normalboot storage read of
the first 256 bytes of `trustfirmware`; the local image begins `a5a55a5a`, matching the
reported sample. The corresponding bytes in the dts candidate are zero. The original
instrumentation tracked CPU stores but missed emulator-host `mem_write` calls.

`emulate-dts-loader.py` now invalidates overread provenance for host-side storage, inflate,
and synthetic-service writes and reports those writes separately. With that correction,
the replay has zero tainted CPU reads; its two provenance-clearing writes are the `dfx`
read (776884 tainted bytes) and the `trustfirmware` header read (256 bytes). The execution
still stops before the later normalboot handoff callback, and the verifier remains forced.
No downstream consumer of the dts proof tail, real proof acceptance, EL2 transition, or
device-safe payload has been demonstrated. Do not use this candidate on the phone.

The stock-dts control with identical captures and emulator settings is
`out/anne-device-private/dts-el2/proof-overread-stock-control-mode1-v1/report.json`.
It stops at the same invalid indirect-call address after the same 12,440,592 basic blocks
and reaches only the same two normalboot PCs (`0x1bd60` and `0x90968`). Its stock proof
read has only a 2 KiB overrun; that tail also has zero observed tainted CPU reads. This
matched control makes the memory fault a harness/model limitation rather than evidence that
the `vlen=0x200000` candidate changed normalboot control flow.

## 11. Normalboot probe with charger callback modeled (3 November 2026)

REA resolves the previous indirect-call fault to `FUN_000bd0b0`, a charger-regulator helper
called while normalboot's first operation runs. For an offline reachability probe only,
`0xbd0b0` was replaced with a zero-return stub; no firmware or device was changed. The
source-attributed replay is
`out/anne-device-private/dts-el2/proof-overread-2m-mode1-source-attributed-v3/report.json`.
Without image aliases, it returns from the normalboot initializer with `0xffffffff` after
the `erecovery_kernel` header read at destination `0x30581840`, length `0x800`, fails.
This did not mean the bytes were absent: the local `kernel.img` and `ramdisk.img` captures
can supply those eRecovery names, as an earlier replay's partition hashes show.

The improved report separates oversized reads by source. Read index 0 is the dts proof
read (`0x200000` bytes into a `0x1000` allocation); it has no tainted CPU reads in this run,
with `0x13e19d` tainted bytes remaining. All 202 tainted read events (788 bytes read) map
to index 1, a separate `kernel` partition header read. The previous aggregate count mixed
those two sources. The report now includes `tracked_sources`, per-partition read counts,
and per-partition remaining-taint totals.

The alias replay, with both eRecovery names mapped to the captured kernel and ramdisk and
the previously observed decompression output regions mapped, is
`out/anne-device-private/dts-el2/proof-overread-2m-mode1-erecovery-alias-mapped-v2/report.json`.
Both gzip streams decompress to the same output lengths and hashes as the earlier control.
Normalboot then fails its `erecovery_vendor` header read (`0x30581d00`, `0x800` bytes), so
`0x911fc`, the FDT helper, and the BL31 dispatcher remain unreached. In this run, tainted
reads map to the `kernel` header (2 events) and `erecovery_ramdisk` read (283 events); the
dts proof-tail source has none. No local `erecovery_vendor` image is available. This remains
a forced-verifier, synthetic-hardware flow trace, not a confirmed device path.

## 12. Normalboot dts consumer ordering (3 November 2026)

REA pseudocode resolves the question of whether normalboot consumes the dts overflow tail.
`FUN_0001bd60` calls the first `rescue_recovery_kernel` operation and returns on any nonzero
result; it calls the second operation only after the first returns zero. The second callback
starts at `0x911fc`. It calls `FUN_00090f30` at `0x91280`, then branches on a nonzero result
to the error block at `0x9140c`, which joins the epilogue at `0x91334`. The BL31 dispatcher
call `FUN_000910d0` is at `0x91330`, so the dts failure branch returns before dispatch.

`FUN_00090f30` obtains the FDT pointer from the dts service. If the getter returns null it
returns `0xffffffff`; otherwise it copies exactly `0x200000` bytes from that pointer into
the callback's destination. This callback never copies bytes after the 2 MiB FDT buffer.
For an inflate output over `0x200000`, `FUN_0006eb44` performs the unbounded inflate, then
rejects the result when its returned length exceeds `0x200000`. `FUN_0006f74c` stores the
FDT pointer only on a non-null return, leaving the getter null in that case. Therefore the
proposed oversized-inflate tail both fails the dts getter and is not consumed by the normalboot
FDT copy.

This does not establish a consumer for the separate `dtb_verify` proof-read tail. In the
forced-verifier alias replay above, the first recovery operation stops at the absent
`erecovery_vendor` capture before the second callback runs; the source-attributed replay
records no CPU reads from the dts proof tail. The proof-read candidate retains a normal FDT
pointer, but its overread bytes are not the FDT source passed to `FUN_00090f30`. A different,
surviving heap object and a normalboot consumer for it still need to be demonstrated offline.
No phone test or dts write is justified by the current evidence.

## 13. Caller-resolved normalboot probe (3 November 2026)

REA resolves the v5 tainted `strlen` callsite `0x90d30` to `FUN_00090c94`, which
builds a kernel command line from the input structure's `+0x40` string. The replay
`out/anne-device-private/dts-el2/normalboot-second-callback-stub-op0-v5/report.json`
records `x30=0x90d34` at that `strlen`, so the helper did execute; REA's direct xref
places its call at `0x91254` in the second callback. The replay also records the
callback entry at `0x911fc`, but does not hit the watched FDT helper at `0x91280` or
the BL31 dispatcher at `0x91330`. It forces the first `rescue_recovery_kernel`
callback at `0x90968` to return zero, so it does not establish the real callback
order or a real boot path.

That report attributes reads at `0x3014caa0` to the dts proof overread, but the
sampled bytes begin `e3 03 1f aa`; the corresponding candidate image bytes at the
mapped file offset `0x16c8e20` are zero. The replay's value and source mapping do not
agree, so these `strlen` reads show that the command-line helper read the destination
buffer, but do not establish that those bytes came from the dts proof tail. The
replay's provenance for this range remains unresolved; no proof-tail control-flow
effect has been confirmed.

## 14. Stock proof-tail neighbor trace (3 November 2026)

The stock dts replay with provenance enabled is
`out/anne-device-private/dts-el2/stock-proof-tail-normalboot-v1/report.json`. It
reads the stock `0x1800`-byte proof from `0x1580800` into the `0x1000` allocation at
`0x30004480`, so the stock tail overwrites exactly `0x800` bytes at
`0x30005480..0x30005c80`. That range is the beginning of the next arena-A allocation
(size `0x2380`, `rpmb_hmac_buff`, allocated by `FUN_00110c88`); the following `0x4000`
allocation is the RPMB frame buffer.

The replay retains all `0x800` bytes as tainted at stop, with zero CPU reads or writes
from the overrun range. It enters the normalboot callbacks, but this synthetic run
stubs `0x90968` to return zero and faults before `0x91280` or the BL31 dispatcher. REA
shows the examined RPMB callers copy freshly read frame data into `rpmb_hmac_buff`
before calling the secboot verifier. This identifies the immediate heap neighbor,
but neither the replay nor the static callers show an EL3 control-flow consumer for
the proof tail. Real proof acceptance is also still unverified.

## 15. FDT-copy replay with the DMA path modeled (3 November 2026)

The stock-dts replay `out/anne-device-private/dts-el2/stock-normalboot-map-dma-v1/report.json`
adds an emulator mapping for the copy routine's DMA registers. With synthetic
zero-returns at `0x90968` and `0x716f0`, it reaches `FUN_00090f30` at `0x91280` with
the stock dts pointer still set to `0x2e400000`. The copy then faults in the CPU
fallback at `0x111b00` because the destination supplied through the stubbed first
recovery operation is address `0x2`. This confirms the fixed-size FDT copy attempt;
it does not validate a real callback setup or reach `FUN_000910d0` at `0x91330`.

The earlier run without the DMA-register mapping,
`out/anne-device-private/dts-el2/stock-normalboot-stub716f0-v1/report.json`,
faults on the unmapped DMA register at `0xfdf30688` before the fallback. Both replays
are synthetic stock-dts flow checks and provide no evidence that the proof-tail
overrun changes this handoff path.

## 16. Base-DTB failure and DTO ordering (3 November 2026)

REA pseudocode for `FUN_0006f74c` confirms the `dtb_init` error handling exactly: when
`FUN_0006eb44` returns a null base FDT, it logs, invokes the 10 ms delay callback, and
returns `0xffffffff` immediately. It does not call the DTO loader at `0x6f454` on this
path. The caller later discards the module-init result, so the boot can continue, but
there is no DTO merge that could consume or repair the rejected oversized FDT.

The DTO loader itself is a separate candidate only after a valid base FDT exists. It
checks the `DTOP` partition header and compressed-size limit, calls the secboot verifier
through the operation table at `+0x58`, then inflates into a 16 MiB allocation. It does
not compare the returned inflate length with 16 MiB before passing the buffer and
reported length to the DTO parser. Section 24 records the verification gate and the
remaining device-state dependency.

## 17. Proof verifier interface and remaining gate (3 November 2026)

REA pseudocode for `FUN_0006e984` shows that `verify_len` controls the storage read into
the `0x1000` proof allocation, but is not passed as an argument to the indirect secboot
verifier. The code copies a fixed `0x1000` bytes from the proof buffer into its working
buffer before calling `FUN_001279f8`; thus a larger read can corrupt following heap
objects without changing the verifier's copied prefix. A zero verify length returns
success without calling secboot, but it causes no proof-buffer overread.

This is a static interface observation, not proof that a modified-length entry passes
the actual verifier. The stock-real-verifier emulator control currently returns failure
(`dtb_verify` result `0xffffffff`) under its modeled preboot/security state. Therefore
the 2 MiB proof-read candidate still fails the offline crypto gate, and no phone test is
justified. The next useful gate is to reproduce the stock verifier's acceptance in the
offline model, then test whether the enlarged read leaves a controlled field that a
real normalboot consumer uses before it is replaced.

## 18. Live REA normalboot recheck (3 November 2026)

Reopened the fastboot dump in REA/Ghidra 12.1.4 and confirmed its SHA-256 is still
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`. Fresh
REA pseudocode for `FUN_0001bd60` (Evidence
`ev_52365c043830316d11fa31e3ac54a7f8e822f1369f54af74792311713da6e374`) shows the
first normalboot callback's nonzero result returns `0xffffffff` before the second
callback is called. Fresh pseudocode for `FUN_0006f74c` (Evidence
`ev_da6792397ba63b73e749b1a7d1d3c3e3712531b996f147a4359f804707fdd533`) shows that
a null result from `FUN_0006eb44` takes the failure log + 10 ms delay return; only a
non-null base FDT reaches `FUN_0006f454` for DTO loading.

This recheck confirms the existing offline traces: the oversized base-DTB path does
not reach the DTO consumer, while normalboot's later FDT/BL31 path remains gated by
the recovery callback. No payload-induced heap object has yet been shown to reach a
normalboot dispatch with controlled contents. This route remains unconfirmed and is
not ready for a device test.

## 19. Arena-A consumer triage (3 November 2026)

The full 33,029,648-byte overflow replay records five Arena-A allocations made
before `dtb_init`: two 0x28-byte BBOX list nodes at `0x30000000` and `0x30000040`,
then OEMINFO buffers at `0x30000080`, `0x30000280`, and `0x30000480`. This narrows the
first possible persistent targets after the 2 MiB DTB buffer. The replay used the
offline inflate shortcut and synthetic pre-DTS callbacks; it is allocation evidence,
not a device-equivalent execution.

REA traces the BBOX nodes to `FUN_000d2c98`, registered by the BFM initializer. The
observed list consumers `FUN_000d2ea4` (module dump) and `FUN_000d3084` (module reset)
are called by `FUN_000d212c` (system-error handling); no normalboot call to that
handler is established. The OEMINFO users clear and reread their 0x200-byte header
buffer from the partition before parsing it (`FUN_000a4100`, `FUN_000a496c`), so the
captured pre-DTS OEMINFO buffers do not retain attacker bytes through those uses.

The previously cited dispatch at `0x3989c` is inside `FUN_0003985c`, whose pseudocode
is the `ultraflash` fastboot command handler. It is not a cold-boot normalboot
consumer. Thus the currently identified early Arena-A objects and that dispatch do
not provide the required automatic handoff. A different normalboot consumer remains
to be demonstrated before payload or device validation can proceed.

## 20. Rebuilt-container format control (3 November 2026)

Built a no-overflow control from the stock backup with `dts-el2-payload.py` and ran
its `verify` command against the original. It passes: the 276-row HSDT table changes
only the selected row's unsigned size/offset fields, the stock proof and original
compressed stream are byte-identical, and the rebuilt stream inflates to the stock
DTB. Control image SHA-256:
`2301ae90d64d7e7812d810b2f8de0efc4bfa3d07c7819605e97567fd46cc4d68`.

This validates the offline container builder's format control. It does not validate
the bootloader's cryptographic verification: the real-verifier emulator still lacks
a passing stock-proof control under modeled preboot/security state.

## 21. Corrected proof-tail provenance trace (3 November 2026)

The earlier ordered trace report
`out/anne-device-private/dts-el2/normalboot-memory-order-forced-v1/report.json`
misattributed bytes at `0x3014caa0` to the dts proof read. Its event dictionary
reached the 4,096-entry cap, and the recorder returned before invalidating taint
for later CPU writes. The tracker now clears overwritten byte intervals before
applying that event-log cap.

The corrected replay is
`out/anne-device-private/dts-el2/normalboot-memory-order-forced-v4/report.json`.
It retains the forced-success verifier, synthetic pre-DTS callbacks and memory
maps, real listed callbacks, and zero-return stubs for post-DTS callbacks. The
replay times out at `0x7d624`; its `normalboot` module is entered but does not
return, so this remains a synthetic flow trace. The provenance report records
zero tainted CPU reads from the dts overread (`tainted_read_count=0`, no dts
entry in `tainted_reads_by_partition`).

The ordered trace for `0x3014caa0..0x3014cae0` shows the dts storage read first
writes zeros there. Normalboot then stores zeros at `0x111e74`, copies other
bytes over the range in `FUN_00113ed4` at `0x113edc`, and scans those bytes in
`FUN_001126cc` at `0x1126d0`/`0x1126dc`. The read bytes are therefore not the
proof-tail data. This corrects the attribution in sections 13 and 19: that
`strlen` is not a consumer of attacker-controlled proof-tail bytes.

Fresh REA pseudocode confirms the copy loop (`FUN_00113ed4`, Evidence
`ev_894a7b13d0d6909917619c7ace3bff8c89811b41409593b4b03a7dd2c32888e5`), the
string scan (`FUN_001126cc`, Evidence
`ev_e226fa8c8de72ca97d65e7d3f5c6d4752288b52e679a6512a83a1e13dcebbce9`), and
the two-callback normalboot order (`FUN_0001bd60`, Evidence
`ev_52365c043830316d11fa31e3ac54a7f8e822f1369f54af74792311713da6e374`). The
fixed-size DTB copy path is also confirmed in `FUN_00090f30` (Evidence
`ev_1fb006b9180bbfc2d685ec3ec17646a5c8dfd00026fee4b010ee7b93db139e8c`). The
corrected replay reports zero allocation-field dispatch candidates and zero
overread dispatch candidates.

REA also confirms that the first callback, `FUN_00090968` (Evidence
`ev_b96fea125528be0c77862eeec1e2d3a57b87ec98e1dfd139954a58111013248d`), can
return `0xffffffff`; `FUN_0001bd60` then returns immediately without calling its
second operation at `0x911fc`. The corrected v4 replay stubs this first callback
to reach the later code, so its `strlen` observation does not establish the
actual normalboot callback path.

This closes the previously suspected consumer lead. The dts path still lacks a
demonstrated surviving overrun object that reaches normalboot control flow, and
the base-DTB overflow is rejected before the getter publishes its pointer. No
payload handoff or device test is supported by the current evidence.

## 22. Normalboot reachability with readiness modeling (3 November 2026)

The forced-verifier replay with modeled crypto readiness and the captured OEMINFO
partition is
`out/anne-device-private/dts-el2/normalboot-oeminfo-forced-v1/report.json`. It
reaches the second normalboot callback only because the first callback at
`0x90968` is stubbed to return zero. It then faults at `0x71708`, an indirect
operation-table call before the watched FDT helper at `0x91280`; the BL31
dispatch at `0x91330` is not reached. The fault target is
`0xaa1f03e2aa1f03e1`, and the corrected provenance counters remain at zero
tainted reads from the dts tail.

REA pseudocode for the adjacent helper `FUN_000716ec` (Evidence
`ev_daf6e0b1a69e109f214b8b7f4926a893da1ffcd72fad998950319958541b992c`) shows
an indirect call through a global operation table at `0x291818 + 0x38`. The
forced replay used synthetic eMMC/storage callbacks, so the invalid table
content is an emulator setup failure, not evidence of a dts-controlled branch.
Supplying the captured OEMINFO fixes its partition reads but does not initialize
the required operation state.

The follow-up with the real eMMC initializer and a zero mapping for its observed
MMIO page is
`out/anne-device-private/dts-el2/normalboot-real-emmc-mapped-v1/report.json`.
That initializer returns `0xffffffff` under the synthetic register values; the
subsequent RPMB initializer faults before its allocations or any reads from the
proof-tail range. The dts tail therefore still has no demonstrated consumer.
The replay also lacks a real-verifier stock-proof pass. The remaining gates are
authentic verifier acceptance and device-equivalent reachability of the
normalboot consumer traced in section 23. No phone test is justified.

## 23. Normalboot DTB consumer traced (3 November 2026)

REA confirms that normalboot does have a DTB consumer, reached from the second
operation in `FUN_0001bd60` only when the first operation returns zero. That
second operation enters the kernel-loading routine at `0x911fc`; its call at
`0x91280` reaches `FUN_00090f30`. REA pseudocode for `FUN_00090f30`
(`ev_1fb006b9180bbfc2d685ec3ec17646a5c8dfd00026fee4b010ee7b93db139e8c`)
looks up the `dtimage` provider, calls its getter, and copies exactly `0x200000`
bytes from the returned pointer to the normalboot destination. The raw code at
`0x911fc..0x91330` was read through REA (`ev_41d9266f9afe91a5245e6223bb789dd2d79b71fecba88b08e3c5326b699dc937`);
Ghidra did not assign that range a procedure, so the caller/callee attribution
comes from the decoded instructions and the emulator PC trace.

Two replays delimit what this proves. The new normalboot replay,
`out/anne-device-private/dts-el2/normalboot-real-storage-forced-v1/report.json`,
uses a stock-size DTB and forces the verifier to succeed. It also stubs the
first normalboot callback (`0x90968`) to return zero; it reaches `0x91280`, then
faults in the DMA copy helper at `0x6dd40`. It records zero dts-tainted reads
and no overflow. This demonstrates the consumer's call path under those
synthetic conditions, not a handoff.

The actual 10,004-byte overrun replay,
`out/anne-device-private/dts-el2/dtb-loader-state-replay-v2/report.json`,
returns from `dtb_init` with `-1` and a null DTB pointer/zero length at
`0x6f804`. Thus the oversized getter does not publish a pointer for the
normalboot copy routine. Even on the successful path, `FUN_00090f30` copies
only the fixed 2 MiB output buffer. The normalboot consumer is real, but these
results do not show it consuming attacker-controlled overflow bytes or reaching
the EL2 handoff. The first-callback gate and a surviving, later-used overwritten
object remain unproven.

## 24. DTO overflow candidate and verification gate (3 November 2026)

REA pseudocode shows a stronger consumer than the base-DTB route. `FUN_0006f454`
reads the `dto` partition, checks the `DTOP` header and compressed-size limit,
then calls the secboot operation at `+0x58` before inflation
(`ev_9e7ed4974f6df73ff712821e093b0c7e9cb721aeb0aad3b55f9e91123bfbcc3b`).
If that call succeeds, it allocates `0x1000000` bytes and calls the shared
inflater without an output-capacity argument or a post-inflate `<= 0x1000000`
check. It passes the returned length to `FUN_0006f16c`, whose checks validate
the DTO magic, declared total size, entry-table bounds, selected board ID, and
blob bounds (`ev_fd2342cef221c889f7388d842f8306d3b4306a9fc83b0b1dad2103365b3fed53`).
`FUN_0006f74c` then passes the selected blob to the FDT overlay parser and
applier. A structurally valid oversized DTO can therefore reach an automatic
consumer after the base DTB succeeds.

The verifier is the hard gate. The `secboot` table in the analyzed image points
at `FUN_00127cf4` for slot `+0x58`. That routine tries `FUN_00126cd4` in two
verification modes and accepts success or the verified-success sentinel
`-0x100` (`ev_ebde11aa4a72798e45bf3080103895f6258f8ccf60a88017a7b6e612bd1d9939`,
`ev_c84e1bf19d964e0ef7fc15423344468c4eb2a61602848f4ebae1980574f82e0d`).
`FUN_00126114` selects the mode from lifecycle state and the eFuse bit at
offset `0x49c + 28`: secure LCS 5/7 requires image verification; non-secure
LCS 0/1/3 with that bit clear selects mode 2, which returns success without
signature verification; with the bit set it selects a verifying mode
(`ev_cb0f0de8aecc91f35ab329a58c8ef43f3b85e04c975c7ab72f98b0ee3f0353ea`).
The captured eFuse context has word 36=`0x00000401` and word 37=`0x20b6d000`.
REA's `FUN_000704e4` bit reader confirms the 32-bit read at `0x49c` yields
`0x0b6d0000`, so bit 28 is clear (`ev_e318692554ace929cb6611996c52b3d3398bfb5b8f73cefc57d42c32b486a9fc`).
The lifecycle getter reads `0xff011000`; its actual handset value is not in the
local captures. The unlocked fastboot flag and eFuse bit alone do not establish
whether this device takes the non-secure bypass or secure verification path.

The local `dto-before.img` capture is 4 MiB of zeros, so it supplies no signed
DTO template. No local VRL captures are available for the verifying modes.
The DTO parser path is identified, but acceptance of a modified DTO is not
established. Do not build a flash candidate or write the phone until the
actual verifier mode is known and the exact DTO payload passes its header,
board-ID, bounds, overlay, and verification checks offline.

## 25. DTS validation and verifier-input comparison (3 November 2026)

REA recheck of `FUN_0006eb44` shows the loader zeroes a 2 MiB DTB buffer, inflates
into it, rejects a returned length greater than `0x200000`, then calls
`FUN_0006e984`. The original planned stream (stock DTB + 2 MiB padding + overflow
payload) necessarily exceeds that cap and returns a null DTB before verification.
It cannot be a passing DTS candidate.

The separate offline proof-overread image
`out/anne-device-private/dts-el2/proof-overread-stocktail-v2.img` keeps the stock
gzip stream and therefore the normalized 2 MiB image identical to stock
(392,205 inflated bytes followed by the same zero padding). It changes only row
0's proof length/offset from `0x1800`/`0x1580800` to `0x10000`/`0x1bf0000`.
That proof read ends exactly at the 28 MiB partition boundary, overlaps no other
indexed row extent, and its first `0x1000` bytes match the stock proof exactly
(SHA-256 `fe057141dbe5d0810bb4557c42b12291454b65d709975ae1b6ba663231dc1443`).
REA shows `FUN_001279f8` copies exactly `0x1000` bytes of proof into its verifier
workspace. These comparisons preserve the verifier's image and proof-prefix
inputs, subject to the larger read's heap side effects.

The helper now has a `preflight` command that checks the HSDT table, selected
extents, the loader's 2 MiB length gate, and whether the normalized image and
first 4 KiB proof prefix match stock. It explicitly reports that cryptographic
verification was not performed and that the image is not device-ready. It
passes for the stock image, the rebuilt no-overflow control, and the
proof-overread candidate. It rejects `container-v1.img`, whose 2,097,476-byte
inflate result exceeds the loader's 2,097,152-byte cap. The ordinary `verify`
command now enforces the same cap; its existing control manifest also exposed
an omitted zero-overflow field, which is now handled compatibly.

This is still not a real verifier pass. With pre-DTS security initializers,
synthetic secure LCS 5, and crypto readiness modeled, the candidate replay
`out/anne-device-private/dts-el2/proof-overread-verifier-preboot-v2/report.json`
reaches the stock proof verifier, attempts `vrl` and `vrl_backup` reads, then
times out at `0x7e004`; both partition captures are absent. The proof-tail trace
records zero CPU reads from the 60 KiB overread before that stop. The matching
stock control
`out/anne-device-private/dts-el2/stock-real-verifier-secboot-partition-v1/report.json`
also fails its VRL reads and times out in crypto. This is a matched flow check,
not proof acceptance: neither run completes the verifier, and the actual
handset lifecycle value remains unknown. No DTS write is supported until a
stock verifier control passes with the real lifecycle, VRL inputs, and
initialized security services, and the proof-tail side effects are shown not to
disrupt that path.
