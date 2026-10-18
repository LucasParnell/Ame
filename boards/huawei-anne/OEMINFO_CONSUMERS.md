# Fastboot OEMINFO consumers — fresh handset analysis, 2026-10-17

Fresh TWRP read of `/dev/block/platform/hi_mci.0/by-name/fastboot`: 4,194,304 bytes. Device SHA-256 before and after transfer and the host hash all match:

`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`

The inventory contains **37 direct OEMINFO operation sites in 29 functions** (29 reads, 6 writes, 2 erases), resolving to **33 distinct field IDs**. It also records 29 reads and 9 writes of cached OEMINFO-derived lock state.

All addresses below are offsets in this freshly captured binary, disassembled at base zero. Function boundaries are inferred from code and in-image function pointers. Names come from this binary’s diagnostics, command-registration tables, or the described behavior. No previous disassembly, previous consumer list, or external symbol table was used for this inventory. The phone was only read during this analysis.

## OEMINFO entry points

| Item | Address | Meaning |
| --- | --- | --- |
| Module descriptor | `0x28d860` | Name pointer `0x14f778` contains `oeminfo`; ops pointer is `0x28d880`. |
| Module registry lookup | `0x110f44` | Returns the named module’s operations. |
| Initializer | `0xa400c` | Acquires storage operations and allocates scratch buffers. |
| Read | `0xa496c` | Ops `+0`: `(field_id, maximum_bytes, output_buffer)`. |
| Write | `0xa45f0` | Ops `+8`: `(field_id, bytes, input_buffer)`. |
| Erase | `0xa48d8` | Ops `+0x10`: `(field_id, bytes, scratch_buffer)`. |

## Every direct operation site

Lengths below are requested limits, not proof of the stored record’s length. The read implementation copies the smaller of the request and the validated stored length. `arg2` in the generic command handler is its caller-supplied length. The `writeOemInfo` wrapper row resolves its argument through its sole direct caller, as described.

| Call site | Function entry | Operation | Field IDs | Bytes | Consumer and use |
| --- | --- | --- | --- | --- | --- |
| `0x21ca4` | `0x21c58` | erase | `0xa7` | `0x80` | **Crypt-info reset** — erase/flash cleanup. Erase the 128-byte crypt-info record (0xa7); direct callers at 0x22960, 0x22eb0, 0x3a1e0. |
| `0x2714c` | `0x270c4` | read | `0x4e` | `0x80` | **Build-number response** — get-build-number. Append the 128-byte build-number string to the command response. |
| `0x2bd30` | `0x2bce0` | read | `0x82` | `0x100` | **RD-mode state** — fastboot_ctrl initialization. Read 256 bytes of RD-lock material and pass it to helper 0xd54ec with a fixed four-byte input; use the result as a boolean state. |
| `0x2bebc` | `0x2be8c` | read | `0x43` | `0xc` | **Root-state getter** — fastboot_ctrl initialization; relock/lock. Interpret little-endian words at payload +0 and +4; either equal to 1 produces a true root-state result. |
| `0x2c2b4` | `0x2c274` | read | `0x5d` | `0x20` | **User-lock state getter** — fastboot_ctrl initialization. Compare all 32 bytes with helper-generated digests of the two fixed lock-state strings. |
| `0x2c440` | `0x2c3d0` | write | `0x5d` | `0x20` | **User-lock state writer** — oem unlock/relock; flashing unlock/lock. Generate the selected 32-byte lock-state digest and write the whole record. |
| `0x2d3cc` | `0x2d38c` | read | `0x43` | `0xc` | **Root-information response** — oem check-rootinfo. Read three words: payload +0 and +4 select root-state text; +8 goes to helper 0x2d0b4 for the additional reason/status text. |
| `0x32e70` | `0x32e3c` | read | `0x62` | `0x4` | **FRP marker getter** — FRP-state getter; fastboot_ctrl ops. Compare the four-byte payload with 0x00465250; return equality. |
| `0x32ef8` | `0x32ebc` | read | `0x5f` | `0x4` | **FRP state resolver** — fastboot command permissions; fastboot_ctrl initialization. Read the four-byte flag; nonzero returns state 2. Zero or read failure consults the 0x62 marker, then the FRP partition's final byte. |
| `0x332e8` | `0x33210` | read | `0x63` | `0x134` | **FRP-password verifier** — oem frp-unlock. Read a 308-byte password/authentication record and feed it into the crypto/password comparison path. |
| `0x34138` | `0x340ac` | read | `0x5b` | `0x80` | **Product-model response** — get-product-model. Copy and return the model string from the 128-byte record. |
| `0x34388` | `0x3432c` | read | `0xa7` | `0x80` | **Crypt-info response** — get_crypt_info. Copy the first four bytes of 0xa7 as a word and format crypt info is <%d>. |
| `0x345c8` | `0x34574` | read | `0xa7` | `0x80` | **Crypt-enable response** — check_crypt. Read payload byte +8 and format crypt enable is <%d>. |
| `0x34b98` | `0x34b28` | erase | `0x12`, `0x61`, `0x8`, `0xa7`, `0x51`, `0x86`, `0x91` | `0x80` | **Table-driven OEMINFO eraser** — oeminfoerase. Field ID is an argument from the seven-row erase table. Erase 128 bytes; table-derived IDs are recorded in tables.json. |
| `0x352e0` | `0x3507c` | read | `0xa7`, `0x6f`, `0xac`, `0xad`, `0xae` | `arg2` | **Table-driven OEMINFO reader** — oeminforead. Select the field ID from row +0xc in the five-row table. Length and output buffer come from the wrapper; all five rows permit reading. |
| `0x35504` | `0x3507c` | write | `0xa7` | `arg2` | **Table-driven OEMINFO writer** — oeminfowrite. Select the field ID from row +0xc. The access-mode check permits writing only cryptinfo (0xa7) in this table; the four version rows are read-only. |
| `0x35b14` | `0x35a90` | read | `0x8c` | `0x800` | **HOTA response builder** — oeminforead HOTA special case. Read the 2048-byte HOTA information record; check/combine it with device model and initial HOTA digest before returning it. |
| `0x35b84` | `0x35a90` | read | `0x61` | `0x20` | **HOTA device-model input** — oeminforead HOTA special case. Read a 32-byte model string and append it to the HOTA response through hotainfo_check_and_combi. |
| `0x35c1c` | `0x35a90` | read | `0x8b` | `0x40` | **HOTA initial-digest input** — oeminforead HOTA special case. Read 64 bytes identified by the diagnostic as hotainfo_init, compare with the calculated digest in the HOTA combination path. |
| `0x36798` | `0x36748` | read | `0xa7` | `0x80` | **Crypt-enable update: read** — enable_crypt; disable_crypt. Read the whole 128-byte 0xa7 record before updating byte +8. |
| `0x367bc` | `0x36748` | write | `0xa7` | `0x80` | **Crypt-enable update: write** — enable_crypt; disable_crypt. Write the same 128-byte record after setting payload byte +8 to 1 or 0. Wrappers are 0x3686c and 0x36874. |
| `0x36dac` | `0x36d30` | read | `0x96` | `0x42` | **Encrypted-UDID response** — getencryptudid. Require payload byte +0 to equal ASCII '1'; pass the following 64 bytes to helper 0x3687c and format its output. |
| `0x3708c` | `0x37050` | read | `0x7c` | `0x20` | **NFF/test-mode flag** — get-psid helper. Compare the 32-byte string with TRUE; callers at 0x37434 and 0x378e8 use the result to choose the PSID path. Name is inferred from this use. |
| `0x37670` | `0x372e4` | read | `0x57` | `0x111` | **PSID response** — get-psid. Read the 273-byte PSID record, use its leading mode byte and the remaining structured data in the PSID response path. |
| `0x37bec` | `0x37b74` | read | `0x12` | `0x40` | **Vendor/country response** — vendorcountry. Copy the 64-byte vendor/country string into the response. |
| `0x37d70` | `0x37cf8` | read | `0x61` | `0x40` | **Device-model response** — devicemodel. Copy the 64-byte device-model string into the response. |
| `0x37fdc` | `0x37f54` | read | `0xb3` | `0x40` | **System-update-state response** — system_update_state. Copy the 64-byte system-update-state string into the response. |
| `0x42f68` | `0x42f20` | read | `0x5c` | `0x1` | **One-shot initrc-tracer flag: read** — kernel command-line construction. If the byte is nonzero, append initrc_tracer=1 to the kernel command line. |
| `0x42fe0` | `0x42f20` | write | `0x5c` | `0x1` | **One-shot initrc-tracer flag: clear** — kernel command-line construction. Clear the one-byte flag after consuming it. |
| `0x55e60` | `0x55adc` | read | `0x15f`, `0x160`, `0x161` | `0x800000` | **Logo record loader** — logo module prepare operation. Read up to 8 MiB: logo type 0 selects 0x15f, type 1 selects 0x160, type 2 selects 0x161. Parse the returned container/header and BMP, then provide pixels to the display path. |
| `0x55fc0` | `0x55adc` | read | `0x15f` | `0x800000` | **Logo fallback loader** — logo module prepare operation. If the selected new-logo read fails, retry the legacy resource in field 0x15f and continue the logo parser. |
| `0x89368` | `0x892c4` | read | `0x65` | `0x5c` | **Rescue main-version response** — rescue module operation. Read and copy the 92-byte main-version string into the response; emit the missing-version message on failure. |
| `0x8baa8` | `0x8ba6c` | write | `0xa6` | `0x5d` | **Generic sysupdateauth writer** — saveTokenInfo chain. Signature is writeOemInfo(id, buffer, length). The sole direct caller at 0x8c2b4 supplies ID 0xa6 and requires length 0x5d (93 bytes); record purpose is update-authentication token storage. |
| `0x8ef54` | `0x8eefc` | write | `0x85` | `0xd` | **Third-party-recovery marker** — load_kernel module. When its input mode equals 2, write the 13-byte NUL-terminated literal 3rd_recovery to field 0x85. |
| `0xc1d00` | `0xc1bd4` | read | `0x61` | `0x20` | **Modem-ID model input** — modemid module initialization. Read the 32-byte device-model string, map it through the device-model table, and combine the result into modem/product selection. |
| `0xc2174` | `0xc1bd4` | read | `0x5e` | `0x20` | **Modem-ID preferred vendor input** — modemid module initialization. Read the preferred vendor/country string from 0x5e. The adjacent diagnostic names it OEMINFO_VENDER_AND_COUNTRY_NAME_COTA. Empty data or read failure falls back to 0x12. |
| `0xc21a8` | `0xc1bd4` | read | `0x12` | `0x20` | **Modem-ID fallback vendor input** — modemid module initialization. Read the 32-byte vendor/country fallback and use the vendor mapping in modem/product selection. |

The two vendor-related IDs must remain distinct: the modem path reads `0x5e` and calls it COTA in its diagnostic, while the erase table names `0x91` as `vendorcountrycota`. This report preserves both observed IDs without treating them as aliases.

## Table-driven field consumers

The generic read/write handler at `0x3507c` loads the table pointer from `0x291bc0`; it points to `0x25c4b8`. There are five 32-byte rows. Row `+8` selects backend, `+0xc` is the field ID, and `+0x18` is the access mode. Backend zero selects OEMINFO. Access mode zero permits reading, and mode two permits reading and writing.

| Table row | Command item | Field ID | Permitted OEMINFO operations |
| --- | --- | --- | --- |
| `0x25c4b8` | `cryptinfo` | `0xa7` | read/write |
| `0x25c4d8` | `SYSTEM_VERSION` | `0x6f` | read |
| `0x25c4f8` | `SYSTEM_VERSION_B` | `0xac` | read |
| `0x25c518` | `ANDROID_VERSION` | `0xad` | read |
| `0x25c538` | `ANDROID_VERSION_B` | `0xae` | read |

The erase handler follows `0x34f14` → one-item `0x34db0` or all-items `0x34ea8` → `0x34d40` → tail call `0x34b28`. It loads its table pointer from `0x2914e0`, pointing to `0x25c558`. There are seven 24-byte rows; backend and field ID are again at `+8` and `+0xc`. Every current row selects OEMINFO.

| Table row | Erase item | Field ID |
| --- | --- | --- |
| `0x25c558` | `vendorcountry` | `0x12` |
| `0x25c570` | `devicemodel` | `0x61` |
| `0x25c588` | `ver` | `0x8` |
| `0x25c5a0` | `cryptinfo` | `0xa7` |
| `0x25c5b8` | `single_sim` | `0x51` |
| `0x25c5d0` | `dm-verity` | `0x86` |
| `0x25c5e8` | `vendorcountrycota` | `0x91` |

`oeminforead` enters wrapper `0x363c8`; `oeminfowrite` enters `0x3567c`. The read wrapper also calls `0x35a90` for its HOTA special case. Command registration slots are `0x25c718`, `0x25c730`, and erase slot `0x25c700`.

## Consumers of cached lock fields

Initializer `0x2cebc` populates the state object addressed by the pointer at `0x292000`. It stores RD mode at `+0x214`, user-lock state at `+0x218`, root state at `+0x21c`, and resolved FRP state at `+0x224`. These values can subsequently be overridden by initialization/error or command paths; the source IDs below describe initial provenance, not an immutable runtime value.

| Initial field sources | State offset | All identified cached read sites | All identified cached write sites |
| --- | --- | --- | --- |
| `0x82` | `0x214` | `0x2abd0`, `0x2ad48`, `0x2afc0`, `0x2b858`, `0x2ba10`, `0x2cf28`, `0x2cfd8`, `0x2d098`, `0x2d764`, `0x2d7dc`, `0x2d7ec`, `0x33138`, `0x34958` | `0x2b518`, `0x2ba58`, `0x2cf00`, `0x2cfc8` |
| `0x5d` | `0x218` | `0x21b08`, `0x2abe0`, `0x2b02c`, `0x2b864`, `0x2c638`, `0x2c944`, `0x2cf44`, `0x2cfe4`, `0x2d0a4`, `0x2d828` | `0x2c6d4`, `0x2ca84`, `0x2cf0c` |
| `0x43` | `0x21c` | `0x2b84c`, `0x2cf5c`, `0x2cf9c`, `0x2d080` | `0x2cf18` |
| `0x5f`, `0x62` | `0x224` | `0x2ac88`, `0x2cf8c` | `0x2cf20` |

The command-permission function at `0x2abb8` reads RD mode (`0x2abd0`), user-lock state (`0x2abe0`), and FRP state (`0x2ac88`). Additional readers serve lock/root status, command dispatch, and unlock/relock paths. `cached-state.json` records each access with its containing function; `cached-state.dis.txt` contains assembly evidence windows around their function entries. This table covers reads of the four derived state fields, not every later use of values copied out of them.

## OEMINFO header-field consumers

The common layer consumes record metadata independently of the payload-specific callers above. Each record has a 512-byte header, and the backup region is at primary offset `+0x02000000`.

| Header field | Consumer | Evidence and use |
| --- | --- | --- |
| `+0`, `+4`: magic words | `0xa4100`, `0xa496c` | Compare with `0x5f4d454f` and `0x4f464e49` (`OEM_INFO`). Read checks at `0xa49f0` and `0xa4a04`; age-reader checks at `0xa41a8` and `0xa41bc`. |
| `+8`: version | `0xa45f0` | Writer stores version 6 at `0xa46ec`. No version comparison was found in the read/age helpers. |
| `+0xc`: field ID | `0xa496c`, `0xa45f0` | Reader loads it at `0xa4a18` and compares with the requested ID. Writer stores it at `0xa46f4`. |
| `+0x14`: payload byte count | `0xa496c`, `0xa45f0` | Reader loads at `0xa4a24` and `0xa4a30`, validates the size class through `0xa3e24`, then clamps transfer length. Writer stores it at `0xa46f8`. |
| `+0x18`: age | `0xa4100`, `0xa4240`, `0xa45f0` | Age is read at `0xa4160`, validated against 999,999,999, and compared between primary/backup records by `0xa4240`. Writer increments modulo 1,000,000,000 and stores at `0xa4700`. |
| Field ID and size parameters | `0xa3edc`, `0xa3e24` | Compute slot offsets and enforce ID-dependent payload limits. Logo IDs `0x15f`–`0x161` use 8 MiB slots with a maximum payload of `0x7ffe00`. |
| Erase bookkeeping | `0xa48d8` → `0xa4804` | Common erase path operates on both record regions; it is shared by the two erase call sites above. |

## Evidence and coverage

All 31 OEMINFO module lookups resolved by the control-flow pass are accounted for by operation sites. An independent PC-relative string-reference pass also checks diagnostics mentioning OEMINFO; its extra hits are common storage helpers or wrapper/caller paths, retained in `coverage.json`. There are no direct `bl` calls to the three OEMINFO API functions, and their only aligned data-pointer references in the captured image are the three operation-table slots. The inventory follows those indirect operations, including reuse of a returned operations pointer for multiple reads/writes.

The reverse direct-call/tail-call closure and in-image function-pointer registrations for each consumer are retained in `inventory.json` under `consumer_caller_routes`. This includes command wrappers and module initialization routes without relying on an older symbol map.

This is a static inventory of the captured firmware. Runtime execution of every command was not attempted. Arbitrary unresolved indirect calls and data copied into unrelated structures prevent a proof that every possible downstream use has been identified. Five non-OEMINFO analysis functions reached the worklist bound; none appears in the independent OEMINFO string-reference sweep. Their addresses are retained in the JSON warnings.

Fresh evidence directory: `out/anne-device-private/oeminfo-consumers-20261017/`.

- `capture.json`, `fastboot.img`: fresh handset capture and checksum evidence.
- `fastboot.dis.txt`: freshly generated AArch64 disassembly.
- `consumers.csv`, `consumers.json`: every operation site, IDs, lengths, command route, and usage notes.
- `fields.csv`: all 33 resolved IDs mapped back to consumer sites.
- `tables.json`: all table-derived IDs, backend selectors, and access modes.
- `inventory.json`, `coverage.json`: data-flow results, caller closure, string references, and analysis limits.
- `contexts.dis.txt`, `cached-state.dis.txt`, `support.dis.txt`: fresh assembly evidence for payload readers, cached readers, wrappers, and header helpers.
- `disassemble.py`, `analyse.py`, `annotations.json`, `write_report.py`: local analysis and report sources.

Rebuild the fresh disassembly, inventory, and report from this captured binary:

```sh
.venv/bin/python out/anne-device-private/oeminfo-consumers-20261017/disassemble.py
.venv/bin/python out/anne-device-private/oeminfo-consumers-20261017/analyse.py
.venv/bin/python out/anne-device-private/oeminfo-consumers-20261017/write_report.py
```
