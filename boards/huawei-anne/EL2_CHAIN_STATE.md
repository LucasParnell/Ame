# EL2 handoff chain — state of the work

17 October 2026. Target: ANE-LX1, 8.0.0.151(C782), Kirin 659 / Hi6250.
Image under analysis: `out/anne-device-private/el2-firmware-inspection/fastboot.img`,
4,194,304 bytes, SHA-256 `16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
796,272 instructions decoded.

**Two address spaces are in play.** Everything below is written as *image-relative*
(`+0x10b634`) with the runtime address in brackets, because the image is loaded at
`0x10000000` — see §2.5. Quoting one without the other is how the `0x110b634`
slip below happened.

No device command was issued and nothing was flashed.

## Where it stands

| link | state |
| --- | --- |
| BMP overflow writes `w·h·bpp` bytes into the heap, unbounded | **measured** (earlier work) |
| write-what-where gadget `+0x8d290` | **measured** |
| EL2h request site `+0x10b634` and its patch value | **measured** (`+0x10b634` = runtime `0x1010b634`) |
| corrupted object field redirects a real dispatch at `+0x3989c` | **measured** |
| entry + dispatch + write run as one chain at the real load base | **measured in Unicorn** |
| load base `0x10000000` | **unsupported** — the manifest field is a flash offset, not a RAM address (§7.3) |
| `usb_init` precedes the logo, `usbloader` follows it | **measured** (log line order) |
| logo records come from oeminfo | **established** (source of the images) |
| relocation of the image's absolute pointers at load | **none in the image**; an external relocator would be required (§7.2) |
| `.text` writable at the running EL | **unverified** |
| BL31 honours the supplied SPSR | **unverified** |
| trigger: two centring calls, small then large, one boot | **not demonstrated** |
| victim object inside the span | **not demonstrated** |

## 1. The defect, as measured

`hisi_display_logo` (`+0x4fac8`, display ops[0] at `+0x25f270`) takes an index in
`w21` and, for indices 0–2, resolves the `logo` module by name (`+0x15bf10`), calls
its prepare op, then calls `+0x4ef74` (also published as display ops[3] at
`+0x25f288`), which calls the centring routine `+0x4ebec` at `+0x4f020`. The range
check is the string `logo index is out supported` at `+0x15be88`.

```
4ec78: add  x28, x24, #0x600      ; x28 = 0x293600
4ec7c: mul  w26, w20, w21         ; w26 = width * height * bpp
4ec80: ldr  x0, [x28, #0x8]       ; cached buffer pointer (0x293608)
4ec88: cbz  x0, 0x4ef14           ; NULL -> allocate
4ec98: bl   0x119c4c              ; memset(buf, 0, w26)
4ecb0: bl   0x1176ac              ; memcpy(buf, src, w26)
4ef14: mov  w0, w26
4ef18: bl   0x111680              ; malloc(w26)
4ef1c: str  x0, [x28, #0x8]       ; the only store to 0x293608
```

The cache holds a pointer and nothing else — no capacity field, so a second call
can never learn the buffer is too small. The firmware names the buffer itself: the
malloc-failure path at `+0x4ef38`/`+0x4ef40` references `+0x15b588`,
`alloc logo_temp_buf failed,error`.

Allocator `+0x111680` is a monotonic bump with 32-byte alignment and no header, so
a 4-byte request yields 32 usable bytes. The overrun is exactly `w·h·bpp − 32`.
The entry clamp at `+0x4ec5c`/`+0x4ec70` compares `width+1`/`height+1` against the
panel and bails to a log-only path at `+0x4ee38`.

`+0x4ebec` has one caller (`+0x4f020`); `+0x4ef74` has one direct caller
(`+0x4fd18`) and is otherwise reachable only through its ops-table entry.

## 2. What is proven

### 2.1 Write-what-where at `+0x8d290`

```
8d290: ldr  w1, [x0, #0x18]   ; destination address (32-bit, zero-extended)
8d294: cbz  w1, 0x8d2a4
8d298: mov  w1, w1
8d29c: ldr  w0, [x0, #0x10]   ; value
8d2a0: str  w0, [x1]
8d2a4: ldp  x29, x30, [sp], #0x10
8d2a8: ret
```

Both operands come from the same buffer, the write is 32-bit (enough for any
address below 4 GiB), and it returns cleanly. Regex scans report this as absent;
the operand-based model finds it.

### 2.2 The EL2 request and its patch site

From `EL2_PATH_AUDIT.md` and `FASTBOOT_EL2.md`, re-read at instruction level:

```
10b634: mov  w1, #0x3c5       ; EL1h, D/A/I/F masked
10b640: str  w1, [x0, #0x10]  ; x0 = 0x35e80030 -> writes 0x35e80040
```

The handoff routine `+0x10b5b8` builds a TF-A-shaped block at physical `0x35e80000`
with the non-secure entry record at `0x35e80030`: PC at `+8`, SPSR at `+16`
(`0x35e80040`), args at `+24`. It then branches to BL31 at `0x35e00000` through the
`br x2` stub at image offset `0x348`.

`0x528078a1` → `0x52807921` changes the immediate to `0x3c9`, i.e. EL2h. For an EL3
request the value is `0x3cd`, but BL31 is still resident at EL3 in that case, so it
buys coexistence, not removal of the monitor.

### 2.3 Dispatch through a corrupted object field

```
3989c: ldr  x22, [x0]
398a0: cbz  x22, ...
398a4: ldr  x1, [x22]
398a8: mov  x0, x20
398ac: blr  x1
```

`+0x398a8` is the trap in this shape: the callee receives `x20`, not the object that
was corrupted. A victim is only usable if its field is the one forwarded as `x0`.

### 2.4 The joined chain, at the real load base

```
0x10021544 → 0x10021548 → 0x1002154c   (op slot +0x25c3a8)
           → 0x1003989c                (corrupted field → fake table)
           → 0x1008d290                (write gadget)
           → 0x1010b634: 0x528078a1 → 0x52807921
err: None
```

The session object and the corrupted object are the same block in this run, because
`+0x21548` reads the command buffer from `[x20+0x5b8]` while `+0x398a8` forwards
`x20`. It proves the mechanics and the address arithmetic, not a real path.

### 2.5 Load base

`out/anne-device-private/xloader-validation-v1/reference/factory-manifest.xml`:

```xml
<image path="xloader.img"  role="xloader"  address="0x00020000" />
<image path="fastboot.img" role="fastboot" address="0x10000000" />
```

Image-relative `+x` is runtime `0x10000000 + x`. The patch site is therefore
`0x1010b634`. The image begins with a `MAGICNUMBER!` header at `+0x28` and contains
9,893 absolute qwords pointing inside its own 4 MB, which means it is linked at 0
and either relocated at load or only ever referenced PC-relatively.

Caveat: this is the flashing tool's manifest — the address the boot chain expects —
not a value read back from a running handset.

## 3. Negative results worth keeping

### 3.1 No usable stack pivot, therefore no return-oriented chain

| mechanism | sites |
| --- | --- |
| `mov sp, x0` | `+0x1a0`, `+0x407c` — both in the EL3/QEMU entry stub |
| `mov sp, x1` | `+0x4110`, same stub |
| `msr SP_EL0/1/2, x0` | `+0x4080`, `+0x4084`, `+0x4088`, same 24 bytes |
| `mov sp, x29` | 4 ordinary epilogues |
| `mov x29, <reg>` from a data register | 0 |
| load `x30` from non-`sp` memory then `ret` | 0 of 4,679 `x30` loads |

Executing `+0x1a0` in Unicorn gives `SP = 0x90000000` (full stack control) and then
`UC_ERR_EXCEPTION` at `+0x1a8`, `msr VBAR_EL3`. At EL1 it faults earlier, at
`+0x4088`. It only runs to completion at EL3, which is not where this code runs.
`ret` is `br x30`, and every `x30` in the image is restored from the stack, so a
stackless dispatch chain does not exist either.

This corrects the earlier chain map, which marked "stage 6 — pivot source" as
proven. No pivot source had been identified.

### 3.2 Fastboot state is BSS, not heap

```
214c8: adrp x20, 0x292000
214d8: ldr  x21, [x20, #0x5b8]    ; command buffer pointer at 0x2925b8
```

The session object is the global area at `0x292000`, outside the arena, so the
overflow cannot reach it. "Corrupt the fastboot session" is not available.

### 3.3 No allocation on the command path

169 allocator call sites exist; the reachability pass from the command dispatch
finds none of them on that path. Everything is allocated during module init.

### 3.4 What the tooling got wrong

Four separate scans returned confident zeros because of bugs in the scanner, not in
the binary:

- capstone prints `bl #0x111680`; parsing `'#0x…'` as hex silently failed, so every
  `bl` target was missed.
- treating `sp` as register index 0 made `ldp x29, x30, [sp]` look attacker-fed.
- treating "loaded from a pointer I chose" as "value I chose" turned
  read-modify-write counters into false write-what-where hits.
- the xloader is **ARM32**, and its ELF maps `.text` to VMA `0x21000` from file
  offset `0x100`; searching for file offsets as addresses found nothing.

Current numbers come from capstone operands with a two-level taint model
(`ATK_PTR` = points at the buffer, so content is chosen; `ATK_VAL` = value chosen,
so a load through it is unknown).

## 4. Boot ordering

From the bootloader log buffer in `recovery-dmesg-private.txt`. Line order is
execution order; the millisecond stamps are **not** usable as wall-clock — the
kernel log shows "Booting Linux" at 0.000 s and its next line at 20.15 s with
nothing between, and the logo plainly does not take five seconds.

```
1847  usb: -- usb_init -- done
1903  module --- usb_switch init end
2188  display: hisi_display_logo: +
2287  preboot: display_logo end
2325  usbloader: hwdog_certify...
```

The USB controller is initialised **before** the logo is drawn and `usbloader`
runs **after** it, so controller objects sit below the scratch buffer and are
unreachable by an upward overflow.

The logo record comes from **oeminfo**, which is what makes the trigger
controllable: the dimensions are read from a partition that can be written, not
fixed by firmware.

## 5. Open work, in order

1. **Relocation.** Decide whether the image relocates its absolute pointers at
   load. It governs whether any address planted in a buffer needs `+0x10000000`,
   and therefore whether the `+0x8d290` destination operand is written correctly.
2. **Trigger.** Establish which oeminfo record each boot mode draws — `prepare`
   takes an index, range-checked to 0–2 — and whether one boot can draw two.
3. **Victim.** With a deterministic bump allocator and an attacker-chosen second
   image size, the span is chosen rather than found. Candidates are the
   `usbloader`-era allocations (`+0x213dc`, `+0x23074`, `+0x239f4`, `+0x26908`,
   `+0x26c2c`, `+0x2a1e0`, `+0x2a228`, `+0x2a75c`, `+0x2aab4`, `+0x2b…`), all after
   the logo and mostly command buffers with list linkage at `+0x10`.
4. **`x0` forwarding.** The victim's corrupted field must be the one passed as
   `x0`; `+0x398a8` shows this cannot be assumed.
5. **Writability.** `.text` must be writable at the EL the chain runs at.
6. **BL31.** Whether the supplied SPSR is honoured at all. BL31 is encrypted and
   cannot be checked from this dump.

## 6. Artefacts

- `out/anne-device-private/gadgetdb/gadgetdb.py` — gadget database, operand-based,
  two-level taint.
- `out/anne-device-private/el2-chain/base1000.py` — chain at base `0x10000000`,
  patches `0x1010b634`.
- `out/anne-device-private/el2-chain/fullchain.py` — same chain at base 0.
- `out/anne-device-private/findentry/` — dispatch, ops-store and reachability scans.
- Scheduled continuation: task `46f0fe7d-ba96-4493-a5df-573f719cc574`.

## 7. Catch-up pass, 17 October evening

### 7.1 The joined chain still reproduces, unchanged

`.venv/bin/python out/anne-device-private/el2-chain/base1000.py`:

```
base 0x10000000   target 0x1010b634
before 0x528078a1   after 0x52807921   patched=True   err=None
```

Trace `0x10021544 → 0x10021548 → 0x1002154c → 0x1003989c → 0x100398ac → 0x1008d290 → 0x1008d2a0`.
No drift from §2.4.

What the trace also shows is that the model is still the synthetic one: the object the
payload corrupts and the fastboot session are the same block, and `x0` arrives at
`+0x21544` as `0x25c398` because the script put it there. §2.4's caveat stands.

### 7.2 Relocation — the image contains no relocator

Three measurements, all on the 4 MB file:

1. **The dispatch table is static.** In the file, `0x25c398` holds `0x2b86c`, `0x25c3a0`
   holds `0x2cfec`, `0x25c3a8` holds `0x2abb8`. A scan of all 796,272 decoded
   instructions for stores whose computed address lands in `0x25c398..0x25c3c8` finds
   **none**; the same scan over the display ops table `0x25f250..0x25f2a0` also finds
   none. Nothing patches these words at runtime — they are file values.
2. **`0x25c398` is the object the dispatcher dereferences.** `+0x110f44` is the module
   registry lookup: it walks the array at `[0x291a30]`, `strcmp`s `[entry]` against the
   supplied name (`0x144cb8` = `"fastboot_ctrl"`) via `+0x112680`, and returns `[entry+0x18]`
   when `[entry+0xc]` is set. `+0x21544` then does `ldr x1,[x0,#0x10]; blr x1` → the word at
   `0x25c3a8`. So the gate really is reached through a static, file-resident pointer.
3. **There is no pointer fix-up loop.** A scan for the only pattern that could relocate a
   range in place — `ldr xD,[xB]` … `add xD,xD,<reg>` … `str xD,[xB]` — returns 29 hits,
   every one of them array-index arithmetic inside normal functions, none a boot-time pass.

Consequence: at any non-zero base, `blr 0x2abb8` faults on the first fastboot command.
Since the firmware demonstrably serves commands, one of these must be true — and only the
first is consistent with what is in the file: **the image runs at base 0**, an external
agent relocates it, or the table is copied to RAM and fixed outside these 4 MB. Nothing
in the file supports the second or third.

### 7.3 The manifest field is a flash offset

`factory-manifest.xml` gives `xloader.img address="0x00020000"`. That is the canonical
Kirin BootROM offset — LBA `0x100` × 512 bytes — i.e. a byte offset in the boot area of the
eMMC, not a RAM address. `fastboot.img address="0x10000000"` is the same field, so it is
the fastboot partition's offset in flash (256 MiB in).

So §2.5's "image-relative `+x` is runtime `0x10000000 + x`" has no support: the value was
never a load address. `base1000.py` is a mechanics demonstration, not a model of the device;
`fullchain.py` (base 0) is the only model consistent with the image's own tables.

Practical effect on the chain: the destination operand of `+0x8d290` is 32-bit either way,
so `0x10b634` and `0x1010b634` are both writable by the same gadget — but the *fake table
entry* and any planted code pointer are not, and those must match the real base. Until a
base is read back from the loader or the device, the payload's pointer values are unfounded.

### 7.4 A symbol listing is embedded in the image, and it does not fit it

`0x292a40..0x374b06` (925,894 bytes) is an `objdump -t` text dump sitting in the file's
slack and in the region the boot clears: 11,055 symbols, 3,266 `.text` functions, including
`hisi_display_logo`, `hisi_prepare_logo`, `prepare_bl31`, `load_encrypted_bl31_img`,
`cmd_hwdog_certify_relock`, `usr_fastboot_relock`.

It is **not this build's table**. At the listing's own values only 1.5% of function starts
decode as prologues; the best shift over ±0x3000 bytes reaches 3.6%, against a random
baseline of 0.8% in the same window. The listing puts `strcmp_asm` at `0x111680`, which is
the bump allocator. Treat it as a different build's table: useful vocabulary, useless
addresses. It is extracted to `out/anne-device-private/symbols/fastboot.symtab.txt`.

### 7.5 What this does to §5

Item 1 is now bounded from the image side: no relocator exists inside the 4 MB, so the base
is either 0 or supplied externally. That is a smaller unknown than before, and it cannot be
closed without the loader or the device.

Items 2–4 are unchanged and still gate everything. The previous session's finding — one
`hisi_display_logo` per boot, cache NULL at first use because the BSS clear covers
`0x293608`, therefore no second larger centring call — remains the sharpest result in this
file, and it is the one that decides whether a chain exists at all. A boot mode that draws
twice (charging boot, or a fastboot redraw after preboot) is the single experiment that
moves it.

## 8. Entry map (S0), same evening

### 8.1 One route in

A scan of every module-op invocation in the image (`bl 0x110f44` → `ldr xN,[x0,#off]` →
`blr xN`, 533 sites, `out/anne-device-private/findentry/ops-dispatch.txt`) gives the display
object at `0x25f270`:

| off | fn | role |
| --- | --- | --- |
| `+0x00` | `0x4fac8` | `hisi_display_logo` — the only route to the centring routine |
| `+0x08` | `0x4df8c` | called by `0x4fd00` right after the logo record is fetched |
| `+0x10` | `0x4e508` | called from `0x4fe0c` on one panel branch |
| `+0x18` | `0x4ef74` | the logo draw → `0x4f020` → `0x4ebec` |
| `+0x20` | `0x4e16c` | backlight: 4 call sites pass `0`, `0x32`, `0x96`, `0xfa` |
| `+0x28` | `0x4e320` | used at `0x4fcbc` to init the geometry block |

No dispatch site in the whole image calls `+0x18`. `0x4ef74` is reached only from `0x4fd18`
inside `0x4fac8`. The entry is `display op[0]` or nothing.

### 8.2 Three callers, all index 0

| dispatch | function | identified by |
| --- | --- | --- |
| `0x291a0` | `0x29174` | handler for the oem command `boot`: table entry at `0x25bc10`, name `0x14a470` = `"boot"`, len 5 |
| `0x8a218` | `0x89be8` | rescue → recovery; logs `rescue: prepare to goto recovery!` (`0x171d48`) |
| `0xcae50` | `0xcae24` | sd update; logs `no_module: \n enter update_from_sd mode` (`0x198500`) |

All three pass `w0 = 0`, so the `cmp w21,#2` in `0x4fac8` never selects anything but
record 0 from these paths.

### 8.3 Where the size comes from

`0x4fac8` looks up the `logo` module (`0x15bf10`), then calls its op[0] with
`(index, &src, &geom)`; `0x4fcb0`–`0x4fcfc`. `geom` is `{+8: w, +0xc: h, +0x10: bpp}` and
the byte count is `(w+1)·(h+1)·bpp` (`0x4ec40`–`0x4ec84`), clamped against the panel width
and height at `[[0x291a88]+0x48]+4` / `+8`.

### 8.4 A second draw in one boot does exist

`0x29174` — the `boot` oem command — draws the logo and only then proceeds. That is a
second centring call in the same boot, on the same cache word, with the same index. §7.5's
"one draw per boot" holds only for boots with no oem command; it came from logs of boots
with no fastboot session in them.

### 8.5 What is left, precisely

The overflow needs draw 2 larger than draw 1. The stock record is 1080 wide and the logged
logo payload is 4,924,880 bytes ≈ `1080 × 1140 × 4` — i.e. shorter than the panel, and the
clamp admits up to full panel height. A second draw from a taller record would run past a
buffer sized for the shorter one by up to `(panel_h − 1140) · 1080 · 4` bytes.

The next measurement, then, is narrow: does the `logo` module re-read the oeminfo record on
each op[0] call, and does the record it returns depend on a mode word that differs between
the preboot draw and the `oem boot` draw? If yes, the trigger is one boot that draws twice
with a taller second record. If no, the two draws are size-identical and the trigger needs a
panel-mode change between them instead — which is measurable in the same harness.

## 9. The size is the BMP header's (S1, same evening)

### 9.1 `logo` op[0] = `0x55adc` parses the bitmap at draw time

The module's ops object is `0x25f520`; op[0] is the routine `hisi_display_logo` calls. It:

- reads the record table from `[0x291870]` — base at `+0`, capacity at `+4`, loaded-flag at `+8`;
- accepts indices `0..0x13` (the display side already clamps to `0..2`);
- for index *i*, takes the record offset `[table + i*4]`, checks the `'B','M'` signature
  (`0x55d30`–`0x55d44`), reads width at `+0x12`, height at `+0x16`, bpp at `+0x1c`, and the
  pixel offset at `+0x0a`;
- rejects bpp other than `0x10` (`0x55d88`);
- writes the geometry back at `0x56064`–`0x56084` as `{+8: w−1, +0xc: h−1, +0x10: 2}` and
  returns the pixel pointer as the source.

So the draw size is exactly **`bmp_w · bmp_h · 2`**, read out of the record on every call. The
only bound is the panel clamp in `0x4ec5c`–`0x4ec70`. The record lives in oeminfo, which is
writable, so the size is attacker-chosen — this is the part §1 already called controllable,
now with the exact field offsets.

### 9.2 The fallback is a second, independent size source

When the table's loaded-flag is clear (`0x55b48`), op[0] takes the branch at `0x55e14` and
fetches a **built-in resource** instead: module lookup `0x14f778`, then `blr x3` with
`w0` ∈ {`0x15f`,`0x160`,`0x161`} (one per index 0/1/2), `x1 = 0x800000` (an 8 MB cap) and
`x2` = the same buffer. Those three IDs are the firmware's own artwork — the boot logo and the
locked-state screens.

### 9.3 Where the bitmap lives

The boot log puts it plainly:

```
1985 ms  no_module: [LOGO] get_logo_buffer_from_dtsi: logo_data->fb_base  = 0x5d000000
1985 ms  no_module: [LOGO] get_logo_buffer_from_dtsi: logo_data->fb_size  = 0x03000000
1985 ms  main: module --- logo init end
2188 ms  display: hisi_display_logo: +
```

48 MB starting at `0x5d000000`, initialised 200 ms before the first draw. The bitmap op[0]
parses and the surface it ends up on are the same memory.

### 9.4 The unlocked warning is not a second centring call — but it touches the same buffer

The warning strings are `0x1702d8` (`"Your device has been unlocked and"`) and `0x170270`
(`"Your device has loaded a different operating system."`), referenced at `0x82c20` and
`0x82d3c`; `"Your device is booting now..."` at `0x16f938`. They belong to the GUI screens.
The shared draw helper `0x88480` (five callers, `0x81b64`, `0x821f8`, `0x82ae8`, `0x830c0`,
`0x83634`) goes through display `op+0x08` and `logo op+1` (`0x552d0`), never through `0x4fac8`
— and `0x4fac8` has no direct callers at all, so no GUI screen can enter the centring routine.

The warning screen therefore cannot populate `0x293608`. What it can do is repaint the surface
that `logo op[0]` reads its bitmap from, which is the same 48 MB buffer — that is the bridge
between the warning screen and the overflow, and it is a candidate for the size change rather
than for the call.

### 9.5 What S1 now is, exactly

One question with three candidates:

| candidate | what would have to be true |
| --- | --- |
| record re-read per draw | a write to the logo record between the preboot draw and `oem boot` changes `bmp_w·bmp_h` |
| lazy table load | the first draw takes the `0x15e14` fallback (built-in art), the second takes the record |
| surface repaint | a GUI screen between two draws rewrites the header bytes op[0] parses |

All three are decidable in the existing harness by calling the real `0x55adc` and `0x4ebec`
twice and printing the geometry each time — no device needed. That is the next run.

## 10. The bitmap is read once, so the size can only move by a parse failure

### 10.1 `logo init` = `0x5533c`, and it runs before the first draw

It allocates the 12-byte descriptor, checks its own loaded byte (`[x19+8]`), and only then
reads: `blr x3` with `x1` = a handle from the `0x147e30`-named module, `w2 = 0x200000`
(a 2 MB cap), followed by three property lookups (`0x15feb0`, `0x151c68`, `0x151c70`).
The boot log puts this at 1985 ms, the first draw at 2188 ms.

So the record bytes are fetched once per boot. `logo op[0]` never re-reads them: candidate 1
in §9.5 (record re-read per draw) is dead, and candidate 2 (lazy load at draw time) only
survives when init itself failed and the loaded byte stayed clear.

### 10.2 Therefore the only lever left is the header bytes

`0x55adc` derives the geometry from whatever bytes sit at the record offset — and every
failure path converges on `0x55b90`, which sets `{w−1, h−1, 2}` from the **panel**, not from
the record. So the draw has two possible sizes:

| parse | geometry | bytes copied |
| --- | --- | --- |
| `'B','M'`, 16 bpp, w/h > 0 | the BMP's own w, h | `bmp_w · bmp_h · 2` |
| anything else | panel w, h | `panel_w · panel_h · 2` |

The second row is the whole-panel frame — 1080 · 2280 · 2 = 4,924,800 bytes on this panel,
which is the number in the boot log. So a second draw whose parse fails is automatically the
largest possible draw, while the first draw's allocation is whatever the record declared.

**That is the trigger, stated exactly:** get the first draw to succeed with a small record and
the second draw to fail its parse in the same boot. Candidate 3 in §9.5 is the only one still
standing, and it has a natural mechanism — the bitmap lives in the framebuffer region
(`0x5d000000`, 48 MB, §9.3), and §9.4 shows the GUI painting screens (including the unlocked
warning) through `0x88480`. A screen painted between the two draws rewrites the header bytes.

### 10.3 The experiment, run

`out/anne-device-private/el2-chain/twodraw.py` drives the real `0x55adc` and `0x4ebec` twice
in one Unicorn run, against a 64×64 panel and a 32×24 record:

```
draw 1: geom w=31 h=23 bpp=2 src=0x20004840  -> 1536 bytes
  centring 1 ok  scratch=0x20060000  peak write=0x20060600  span=1536
draw 2: geom w=63 h=63 bpp=2 src=0x20004840  -> 8192 bytes
  centring 2 ok  scratch=0x20060000  peak write=0x20062000  span=8192  (alloc was 1536)
```

Between the two calls nothing changes except the first two bytes of the record. The first
draw parses, allocates 1536 and writes exactly 1536. The second fails the `'B','M'` check,
takes the panel branch, and writes **8192 bytes into the same 1536-byte allocation — 6656
bytes past its end**, with no fault, through the firmware's own memset and memcpy.

So the size jump is no longer an inference: it is what the shipped code does, measured. The
ratio is the BMP size against the panel, which on the real 1080-wide panel means anything
from a few kilobytes up to a 4.9 MB frame.

What is still not demonstrated is the real-world cause of the parse failure — the GUI
repaint in §10.2 candidate 3. That is the last piece of S1. Everything downstream (S2 span,
S3 victim) can now be built against a measured overflow end address instead of a hypothetical
one.


