# Fastboot RD mode — 18 October 2026

RD mode bypasses the shared fastboot command-permission gate, but its OEMINFO
representation is an RSA authorization token. Setting a payload byte to one
does not enable it. This is independent of xloader's lifecycle-based image
authentication.

This analysis uses the handset's captured fastboot image with SHA-256
`16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52`.
Addresses are raw offsets with the disassembler based at zero. No phone commands
or writes were issued for this investigation.

## OEMINFO authorization path

1. Getter `0x2bce0` reads 256 bytes from OEMINFO field `0x82` at `0x2bd30`.
2. It constructs the four-byte message `00 00 00 01` at `0x2bd50`–`0x2bd5c`.
3. At `0x2bd8c` it calls `0xd54ec` with hash options `2` and RSA key selector `2`.
4. Hash helper `0xd4f54` sees option bit 1 set and appends four device-derived
   bytes obtained through `0xd488c`. It hashes the resulting eight bytes using
   the SHA-256 path. The suffix comes from the helper named `get_hun`;
   `0xd60b4` derives it from characters `[20:28]` (zero-based, end exclusive) of
   the 32-character storage CID representation supplied by `get_cid` at `0xd5f3c`.
   The parsed 32-bit word is stored little-endian. These are storage identity
   bytes, not the hardware lifecycle value.
5. `0xd3fe8` performs an RSA public operation on the 256-byte OEMINFO token,
   checks type-1 padding (`00 01 FF…FF 00`), and extracts the remaining bytes.
   Wrapper `0xd54ec` requires exactly 32 recovered bytes at `0xd5774`–`0xd577c`
   and compares them with the calculated hash at `0xd5978`–`0xd5984`.
   The recovered value is the raw digest, without an ASN.1 DigestInfo prefix.
6. Only verifier success returns RD state `1` at `0x2bd9c`. A missing OEMINFO
   record, failed read, or failed check follows a return-zero path.

The key table is at `0x2903c0`, referenced through pointer `0x2917a0`. Selector
2 points to the public key at `0x258170`: a 256-byte modulus, length at `+0x100`,
exponent bytes at `+0x102`, and exponent length at `+0x202`. The modulus is
2048 bits and the public exponent is 65537. SHA-256 of the stored modulus bytes:

`ab561e9c512d9f2c8091ec34150ed2673884497bbae2952402f1b411e64fd0e9`

The public key permits verification, not generating a token. Constructing a
normally accepted token requires the corresponding private key, an existing
valid token for the same derived input, or a separately established bypass.
No such token or bypass was established in this investigation.

## Effect and other state paths

Initializer `0x2cebc` stores the getter's result into cached state `+0x214` at
`0x2cf00`. The command gate `0x2abb8` checks it first: value `1` branches at
`0x2abd8` directly to the allow result at `0x2ac74`, bypassing its user-lock,
FRP and allowlist checks. Individual command handlers can impose further checks;
this does not create handlers for unsupported commands.

A separate runtime path at `0x2b2ac` accepts a 256-byte RSA response using key
selector 1, requires 16 recovered bytes, compares them with cached challenge
data, and then sets RD state at `0x2b518`. This is another authorization path,
not evidence of an unsigned enable flag. The clear path writes cached RD state
zero at `0x2ba58`.

An initialization fallback at `0x2cfc0` calls `0x2bbf8`, which reads NVME item
`0x138` named `FBLOCK` and interprets a zero data byte as RD enabled. This is a
distinct non-OEMINFO source, selected by initialization conditions. Its presence
does not mean writing OEMINFO field `0x82` can select that path.

## Saved handset records

Field `0x82` uses the 4 KiB slot at `(0x82 - 1) << 12 = 0x81000`; the backup
slot is at `0x2081000`. Both complete slots are zero in both of these images:

- Original handset backup: `out/anne-device-private/oeminfo-dump-20261017/oeminfo.img`.
- Supplied and flashed replacement: `out/anne-device-private/flash-oeminfo-el2-20261018/oeminfo-el2.img`.

Neither saved image contains an OEMINFO RD token. These observations concern
the saved files, not a new read of the currently running phone.

Evidence and reproducible extraction are in
`out/anne-device-private/rd-mode-20261018/`: `analyse.py`, `analysis.json`, and
`evidence.dis.txt`. Run `.venv/bin/python out/anne-device-private/rd-mode-20261018/analyse.py`.

## Focused verifier audit

One concrete parser defect was reproduced offline: the type-1 padding scanner
reads one byte beyond its 256-byte RSA output allocation when the recovered
block starts `00 01` and every remaining byte is `FF` (no zero delimiter).
At `0xd4318`, the pre-incrementing byte load reaches offset `0x100`; the end
comparison at `0xd4310` is reached after that load when the loaded byte is `FF`.
If the byte beyond the allocation is zero instead, subsequent length checks
reject the resulting invalid remaining length. The offline test observed this
one-byte overread and a nonzero wrapper result (`19`), with no digest comparison.

This is an out-of-bounds read in the parser, not a demonstrated RD authorization
bypass. The test injects a synthetic output at the RSA-operation boundary; it
does not provide an attacker-generated signature producing that output under
the embedded public key. No device execution, crash, disclosure or signature
forgery was demonstrated.

The audit executed the captured AArch64 verifier, key-selection, parser, bounded
copy and comparison instructions in Unicorn across 32 cases. RSA-operation
outputs, hash generation and crypto return statuses were modeled; hardware and
signature arithmetic were not exercised. A matching synthetic digest was the
sole accepting control. Changed header/padding/digest bytes, recovered lengths
of 0, 1, 16, 31, 33, 64, 245 and 246 bytes, injected hash/RSA/key-import errors,
and failure of each of the eight baseline allocations all rejected. The key
allocation failure case models the crypto importer rejecting a null destination;
this does not establish how the real importer handles that argument.

The four public moduli in the key table have pairwise greatest common divisor
one, so this set does not expose a shared-prime factorization. That check is not
a general proof of RSA key quality or strength.

Two further limitations are visible without constituting a demonstrated keyless
bypass:

- The persistent authorization has no fresh challenge or expiry in this path.
  Its device-derived input contains at most 32 bits, so an issued token would
  also match another device with the same derived value and embedded key.
  This observation does not supply a valid token.
- The digest comparison at `0x111b8c` exits on the first unequal byte. This is
  not constant-time. A useful hardware timing oracle or signature forgery based
  on it has not been established; RSA output and expected-digest comparison
  are different operations.

Reproduce the audit with
`.venv/bin/python out/anne-device-private/rd-mode-20261018/audit.py`.
`audit.json` records each case, return status, allocation event, comparison
length and padding-scanner boundary read.
