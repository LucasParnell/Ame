# Exact-device fastboot authentication evidence

14 October 2026, ANE-LX1 8.0.0.151(C782), Kirin 659/Hi6250.
This updates the previously unresolved authentication gate in
[FASTBOOT_EL2.md](FASTBOOT_EL2.md).

## Result

**The existing EL2 instruction patch fails the captured fastboot content hash.
Replacing that stored hash also fails the existing certificate signature.**
This is now reproduced from this handset's own signature metadata, rather
than inferred only from Huawei secure-boot descriptions. It rules out using
the unchanged authentication metadata with the patched persistent image, or
repairing it by merely recalculating a checksum.

No modified fastboot, xloader, VRL or factory loader was executed or written
to the phone. Runtime rejection of a modified image was not deliberately
tested. An authentication bypass remains a separate research question.

## Read-only capture

The explicitly requested original shared-kernel restoration completed and its
full partition SHA-256 was read back from installed TWRP. The subsequent
capture checked the original device serial, `ro.product.device=anne`,
TWRP `3.7.0_9-0`, and an existing root shell.

This handset has no `by-name/xloader` link. Xloader is in the eMMC boot area;
the matching vendor [partition definitions](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/include/linux/hisi/partition.h)
also place it in `EMMC_BOOT_MAJOR_PART`. Both boot areas were captured without
changing their `force_ro=1` state. Reads of VRL, its backup and fastboot were
also captured. Every artifact's size and SHA-256 matched a fresh phone-side
read. The private directory is `out/anne-device-private/xloader-validation-v1/`.

| Artifact | Bytes | SHA-256 |
| --- | --- | --- |
| eMMC boot0 | 4,194,304 | `72b7d1ad7407bb2f56389b1bb7c2a5611a83474c454b1e20970197f7bc00eed1` |
| eMMC boot1, all zero | 4,194,304 | `bb9f8df61474d25e71fa00722318cd387396ca1736605e1248821cc0de3d3af8` |
| VRL and identical VRL backup | 262,144 each | `ce5d2889ae815309ad5d37102073ee64b2f0a56282dfdb938274cba244f9696e` |
| Current fastboot, unchanged from the earlier dump | 4,194,304 | `16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52` |

## Fastboot's authenticated component

The fastboot record is in the VRL slot at `0x6000`. The component descriptor
after its content certificate records flash offset `0x200000`, consistent
with the fastboot partition's location, and a length of `0xa4950` words.
Xloader's length extraction multiplies the word count by four.

That is **`0x292540` bytes (2,696,512)** from the start of the raw fastboot
partition image. The stored 32-byte digest at VRL offset `0x674c` matches
that exact prefix. Bytes beyond this prefix are not established as
authenticated by this particular component record.

| Input | SHA-256 of the authenticated component |
| --- | --- |
| Stored digest and original fastboot | `c5fd7fe49b45e5fd8fa254f1ca1295824ee00a6db765ef314334de823796a39d` |
| EL2h candidate | `365c9589afa5fd8304be4bfc9f5e5dec17ef3129859393c704790eda45281119` |

The changed instruction at raw fastboot offset `0x10b634` lies inside this
authenticated component. Moving a payload into padding alone would not make
the signed bootloader call it; no such entry mechanism has been demonstrated.

## Independent certificate checks

The verification script uses Python's integer modular exponentiation and
`hashlib`; it needs no external cryptographic package. For this captured
CryptoCell format, the modulus is stored big-endian and the RSA signature
little-endian. The signatures validate as RSA-2048/PSS with exponent 65537,
SHA-256, and a 32-byte salt.

All three certificate signatures in the fastboot slot independently validate:

| Certificate | Signed-data start in VRL | Signature start in VRL |
| --- | --- | --- |
| Root key | `0x6000` | `0x61cc` |
| Intermediate key | `0x62cc` | `0x6498` |
| Fastboot content | `0x6598` | `0x6774` |

The root-to-intermediate and intermediate-to-content key hash links also
match. Each link hashes the next certificate's 256-byte public modulus plus
its 20-byte CryptoCell auxiliary key data.

The content certificate's signed bytes include the image digest. In an
offline copy, replacing that digest with the patched image's digest causes
the original content signature to fail. Truncated signatures are also rejected.
This is a negative authentication test, not a write to either phone VRL copy.

The root key's modulus-plus-auxiliary-data SHA-256 is
`37937554356c88f8d2a25dadbd1d46137a11af131b26f669bff19b6281f339a1`.
Its identity is pinned to the captured stock artifacts. The hardware's fused
root was not read, so this does not independently measure the OTP trust anchor.

Reproduce from the repository root:

```sh
python3 out/anne-device-private/xloader-validation-v1/verify-authentication.py
```

Evidence files: `manifest.json`, `authentication-proof.json`, the private
`xloader-disassembly.txt`, and `factory-loader-authentication.json` in the
same directory. No proprietary firmware is added to Git.

## What xloader's code adds

Xloader is readable Thumb code in boot0 after its 4096-byte certificate header.
The analysis ELF assigns an address base solely to make disassembly readable;
the following offsets are **raw boot0 image offsets**:

- The normal fastboot loading routine at `0x122c` references the `fastboot`
  name and VRL slot `0x6000`. The Hi6250 chip branch skips some direct reads,
  relying on the platform's preloaded buffers; the generic read sequence alone
  is not the whole Hi6250 boot path.
- At `0x12d4`, it invokes the verification wrapper at `0x7248`, passing a
  lifecycle-derived mode. A nonzero verification return takes its failure
  branch. The upstream loading routine can retry the backup VRL.
- The wrapper's modes 0 and 1 call the certificate/component verification
  path at `0x70fc`. Mode 2 returns success without that call. The mode producer
  at `0x6ffc` depends on hardware lifecycle/security information and a bit read
  through the crypto/OTP access helpers. It does not consume fastboot's
  USERLOCK or FBLOCK variables.

The existence of a mode-2 branch does not establish that this production
handset can select it through a supported setting. The existing boot logs
report a secure product. No OTP write, lifecycle change, unknown SMC, or
attempt to trigger a verification failure was performed.

## Single-byte flag changes: fresh verification, 17 October 2026

Xloader in eMMC boot0, both VRL copies, and fastboot were read again from
the connected phone in TWRP. Each host artifact matches the phone-side
SHA-256 before and after transfer. The hashes match the artifacts documented
above. Boot0 remained `force_ro=1`; this verification made no device writes.

Fresh captures, a standalone verifier, results, and newly generated Thumb
disassembly are in
`out/anne-device-private/fastboot-write-verification-20261017/`.

The fresh VRL confirms that raw fastboot offsets **`0x000000` through
`0x29253f`, inclusive**, are covered by the content certificate's image hash.
The original image hash, all three RSA-2048/PSS/SHA-256 signatures, and both
certificate key-hash links validate. The xloader payload's certificate chain
and content hash validate separately, with the same recorded root key material.

The offline verifier changes exactly one bit in one byte, independently at
each of three offsets:

| Changed offset | Resource | Original image digest still matches | Replacing digest preserves content signature |
| --- | --- | --- | --- |
| `0x1702b0` | Unlocked-warning text | No | No |
| `0x236930` | Unlocked-warning JPEG | No | No |
| `0x292110` | Pointer to the unlocked-warning JPEG | No | No |

An outside-range control at `0x300000` leaves this component's digest and
signature intact. This establishes only the bounds of this particular signed
component; it does not establish that the control byte is a useful flag or
that modifying other components is accepted.

The current xloader bytes independently confirm these raw boot0 offsets:

- `0x12d4` calls verification wrapper `0x7248` before fastboot is executed.
  The result check at `0x12da` sends a nonzero result to the load-failure path.
- `0x7272` calls certificate/component verifier `0x70fc` for verification
  modes zero and one. Mode two skips that call.
- Mode producer `0x6ffc` gets security/lifecycle information through
  `0x798c`, then conditionally reads a hardware word through `0x7a80`.
  This path does not read fastboot's ordinary USERLOCK/FBLOCK state or an
  OEMINFO field to decide whether verification is skipped.

Consequently, a deliberate flag change inside the covered prefix fails the
existing authentication in the enforcing path even if the edited image is
otherwise structurally valid. An ordinary bootloader-unlocked flag does not
select the hardware verification-skip branch shown here.

At this stage the hardware lifecycle/OTP mode had not been measured. A fresh
TWRP dmesg query yielded no matching security-mode boot-log lines. The lifecycle
follow-up below subsequently identified the bootloader-reported mode through
the current boot arguments. No modified firmware
was flashed or booted, so these results do not claim that permanent failure or
a specific emergency recovery outcome was demonstrated on the handset.

Reproduce the cryptographic verification without touching the phone:

```sh
python3 out/anne-device-private/fastboot-write-verification-20261017/verify.py
```

`verification.json` records the original signatures and each single-byte test;
`xloader-verification.dis.txt` records the newly disassembled verification and
mode-selection paths.

## Lifecycle follow-up: current boot reports enforcing mode 0

The current TWRP boot reports **`efuse_status=0`** in both `/proc/cmdline` and
the device tree's `chosen/bootargs`, with exactly one occurrence in each.
The same arguments report `userlock=unlocked`, `bootlock=unlocked`, and
`androidboot.verifiedbootstate=ORANGE`. These are different state variables:
normal bootloader unlock and Android's orange verified-boot state coexist
with the earlier bootloader-authentication mode.

Tracing the fresh fastboot image establishes the meaning of that argument:

1. Formatter `0x42bf4` looks up the `secboot` module and invokes operation
   `+8` at `0x42c1c`, passing an output-word address.
2. The descriptor at `0x28ff88` has operations at `0x28ffa8`; its `+8` entry
   is `0x12752c`, a branch to security-configuration selector `0x126114`.
3. After a successful return, the formatter loads that output word at
   `0x42c34` and formats it with `efuse_status=%d` at `0x42c44`.
4. Selector `0x126114` obtains lifecycle state through `0x80160`. Its signed
   byte jump table at `0x13d7b0` directs **only lifecycle values 5 and 7** to
   `0x1261ac`, which stores output mode zero at `0x1261e0`.

This is evidence for the **current bootloader-reported mode**, rather than a
direct read or attestation of the hardware lifecycle register. It establishes
the candidate set `{5, 7}` under the captured firmware's selector; it does not
distinguish which of those lifecycle values the hardware returned.

### What xloader actually reads

Xloader's lifecycle path is `0x6ffc` → `0x798c` → `0x89a4`.
The final helper polls controller offset `+0xabc`, then reads the low byte of
the lifecycle/status register at **controller offset `+0xad4`**.
The xloader controller base is `0xbf011000` in its execution address space.
The corresponding fastboot getter uses base `0xff011000`, including the
lifecycle helper at `0x7d60c`. These addresses are from the respective firmware
code; no live raw-MMIO read was used to establish the reported mode.

For lifecycle values 0, 1, or 3, xloader additionally calls `0x7a80` with OTP
word index **`0x24`**. The helper at `0x8f4c` submits a read command through
offset `+0xaa4` and retrieves the word through `+0xaac`. The selector tests
**bit 28**, mask `0x10000000`, of that word.

| Hardware lifecycle value | OTP word `0x24`, bit 28 | Selected mode | Wrapper behavior |
| --- | --- | --- | --- |
| `5` or `7` | Not consulted by mode selector | `0` | Calls verifier `0x70fc` |
| `0`, `1`, or `3` | Set | `1` | Calls verifier `0x70fc` |
| `0`, `1`, or `3` | Clear | `2` | Skips verifier in wrapper `0x7248` |
| Other accepted input, or lifecycle-read/validation error | — | `3` / error | Loading aborts or wrapper rejects |

The lifecycle getter performs additional hardware-status checks. For value 5
it checks status bit 8 and validates part of OTP word `0x0a`; for values 1 and
5 it checks status bit 9. Those failures do not enter the verification-skip
path. Polling and validation occur before the selector returns its mode.

Arm's published lifecycle terminology maps `0` to Chip Manufacture, `1` to
Device Manufacture, `5` to Secure Enable, and `7` to Return to Manufacturer.
See the [Arm CryptoCell lifecycle indication table](https://documentation-service.arm.com/static/5e85992bc10d1d3c5b7052d7),
appendix A.7.1. This is naming context from Arm's published CryptoCell
documentation; the numeric behavior above comes from this handset's code.
The older value `3` is accepted by this firmware, but its exact lifecycle
name has not been established here.

### Offline execution and live evidence

`lifecycle/emulate.py` executes the captured Thumb lifecycle getter, hardware
validation, OTP-read helper, and selector with synthetic MMIO. All 18
combinations of lifecycle values 0–8 and the two OTP-bit settings match the
table. Three additional hardware-validation error cases select error mode.
Eight verifier-wrapper runs cover modes 0–3 with a synthetic successful or
failing verifier; modes 0/1 call it and propagate failure, mode 2 skips it,
and mode 3 fails. Logging and clock calls are stubbed; no hardware is exercised.

`lifecycle/emulate_fastboot.py` independently executes fastboot's actual
selector with synthetic lifecycle/OTP helpers. Its 18 cases agree with the
xloader mode matrix. In particular, only input lifecycle 5 or 7 produces the
zero seen in the live boot arguments.

TWRP exposes `/dev/efuse`, but inspection of the pinned Huawei driver/header
found chip-ID, authentication-key, die-ID, and debug-mode operations rather
than a direct lifecycle-register ioctl. Debug-mode readout is not the same
measurement as lifecycle state. No unknown ioctl or fuse-programming
operation was issued. The mode finding comes from the existing boot arguments
and their independently traced producer.

**Conclusion:** the current boot reports the enforcing mode, not mode 2.
For the lifecycle candidates consistent with that report, clearing the tested
OTP bit would not select mode 2: lifecycle 5/7 takes mode zero without
consulting that bit. Nothing in this path makes the ordinary bootloader-unlock
flag a lifecycle-mode switch.

Evidence is in
`out/anne-device-private/fastboot-write-verification-20261017/lifecycle/`:
`live-mode.json`, `emulation.json`, `fastboot-mode-emulation.json`,
`fastboot-live-mode.dis.txt`, and the two emulator scripts. The downloaded
reference driver/header and their source revision and hashes are retained
beside them in `kernel-source.json`.

## Factory-loader lead and its limits

The ANE-compatible factory images from PotatoNV's
[HiSiBootloaders repository at commit 4755428](https://github.com/kitsuned/HiSiBootloaders/tree/4755428/hisi65x_a)
were downloaded for offline analysis. Their certificate signatures, key hash
links and payload hashes all validate. Both use the same root public-key
material as the captured stock xloader and fastboot VRL record.

| Factory image | SHA-256 | Authenticated payload bytes |
| --- | --- | --- |
| xloader | `07c2ec41717b532b5f8d27968111539901854afacb0c0f35ac8b85dc08be0e81` | 66,816 |
| fastboot | `c019e3e9acebe2a7e835d83efd170f8867973c5f7dba808958c2e02e1054a145` | 3,371,008 |

This makes them a concrete signed-loader research lead, consistent with
[PotatoNV's documented ANE support and RAM-loading method](https://github.com/kitsuned/PotatoNV).
It does not prove this phone accepts every factory certificate's lifecycle,
rollback and platform parameters in the current boot mode.

The factory fastboot also requests EL1h: the corresponding instruction is at
payload offset `0xf9f34` (file offset `0xfaf34`, including its header).
Its authenticated payload covers that instruction, so simply applying the
EL2 immediate change to the signed factory image has the same signature
problem. Static inspection of its `oem boot` handler shows module-based boot
operations, not a demonstrated arbitrary-address execution interface. Commands
named `rw` and `wr` operate through storage/partition modules in the examined
handlers; their names are not proof of unrestricted memory access.

The next concrete research target is what the **unchanged, authenticated
factory loader can do after authentication**, or an exact-device earlier-stage
verification defect. A loader must demonstrate a usable EL2 handoff before
persistent replacement is considered. No working EL2/VM route is claimed.
The phone remains in TWRP with its verified original kernel.
