# RPMB SMC verification and FF06 feasibility

Implementation follow-up: the [eMMC/RPMB transport](EMMC.md) now exists with
host tests and an explicit `diag el3-rpmb arm-emmc` mode. It is not yet verified
on hardware. The captures and missing firmware-producer findings below remain
historical evidence; a completed handset transaction is still unproven.

16 October 2026. Live Ame serial observations, focused host regressions, and
review of the pinned Huawei-derived Linux driver. Service reachability and
completed RPMB transactions are separate findings.

## Follow-up: returning an FF06 error is still blocked

The user clarified the required result: FF06 must actually execute with a
storage error and return, leaving its calling core available. A locally
rejected completion, a returned unsupported trigger, or a helper running on a
sacrificial core does not meet that requirement.

A subsequent live test used the existing `diag el3-canary` initialization.
FF04 returned zero and a readback showed `+0x6a00..+0x6a3f` cleared. The later
`diag el3-rpmb arm` then registered successfully. Thus the nonzero initial
buffer can be resolved by registration; it was a pre-registration diagnostic
obstacle, not evidence that the firmware registration operation was broken.

After this initialization, `counter 1` returned on CPU 1 with
`trigger_x0=0xffffffff`, `trigger_inflight=0`, and `sgi5_events=0`. The probe
was released successfully and the shell remained responsive. This attempt
still supplied no firmware transaction for the gated FF06 completion.

The pinned HiKey960 reference was downloaded again and its SHA-256 verified.
Its common RPMB handler dispatches FF04, FF05, FF06, FF10, and FFF5, but FFF3
falls through to the unsupported result. Its nonzero FF06 branch loads the
saved callback at request `+0x6a30` and calls it without a null check. These
are reference-binary findings, not Anne's private ABI. Anne's corresponding
callback and entry/return context remain unknown. Sending an unsolicited
error completion with an invented callback would not establish a returning
error path.

No FF06 call, firmware replacement, or card I/O was performed in this
follow-up. The requested outcome is **not fixed**: a verified Anne producer or
a matching reconstruction of the state consumed by its handler is still
missing. Captures, the reference disassembly, and a machine-readable summary
are in `out/anne-device-private/ff06-error-return-20261016/`.

## Result

The handset's RPMB metadata service works. FF04 registration succeeded in an
earlier captured probe, but the current boot refuses to arm because its shared
buffer contains a nonzero request-status word. Neither capture establishes a
completed FF06 transaction on Anne.

FF06 is a **storage completion**. Its intended error path can operate without
eMMC I/O once a valid firmware transaction exists. Authentic successful storage
operations need an RPMB backend or an equivalent backend that satisfies secure
firmware's response checks. No such replacement is established here.

Ordinary eMMC block support alone would not make Ame's FF06 path work. Separate
missing pieces are a verified Anne request producer, RPMB transport, and a
service connecting notifications and responses to FF06. There is no demonstrated
firmware prohibition against production completion, but successful operation on
this handset remains unproven.

## Current hardware observations

The existing diagnostics on `/dev/cu.usbmodemanne_native_011` returned:

| Operation | Observed result | Establishes |
| --- | --- | --- |
| `diag el3` | Call count `0xf`; UID `72ef2c14:11e4f22a:6e288894:0f5789d4`; version x0/x1 `0/1` | Metadata SMCs return normally. The count does not prove every advertised operation is enabled. |
| `diag el3-rpmb arm` | Result 5, `stale_request`; `armed=0`, `registered=0` | Local preflight rejected the buffer before copying config or invoking FF04. `registration_x0=0` here is an initial value, not a successful SMC result. |
| `diag el3-win 6a00` | Nonzero, mostly `aa`-patterned bytes; stock-layout status at `+0x6a22` is `0xa2b8` | Explains the refusal; does not establish an outstanding firmware transaction or its owner. |
| Final `diag el3-rpmb status` | Unarmed; trigger and FF06 not started | Shell responsive; no helper created by this verification. |

The status report decodes request fields only after registration and a
notification, so its zero fields do not contradict the raw buffer snapshot.
The guard may reject residual or uninitialized memory as well as a pending
request. This run preserved the buffer. No flash, reboot, card I/O, or FF06 call
was performed.

Captures, command records, and the bounded helper are under
`out/anne-device-private/rpmb-verification-20261016/`. The helper stopped at its
registration assertion. Subsequent commands only read the request and final
status; its counter/completion/release steps were not executed.

The earlier v37 captures show `FF04 x0=0`, then `FFF3 x0=0xffffffff`, no SGI 5,
and refusal to queue FF06. This is historical registration evidence, not a
result reproduced in this run. See [diagnostic history](RPMB_DIAGNOSTIC.md).

## Without eMMC

The pinned [Huawei driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/mmc/card/hisi_rpmb.c)
registers the shared window with FF04, receives a firmware request through SGI 5,
services its storage operation, then calls `FF06(storage_result, 0, 0)`.

`rpmb_work_routine()` calls FF06 on failures too. Missing key readiness yields
`RPMB_ERR_KEY`; failure to open the RPMB device yields `RPMB_ERR_BLKDEV=0xff05`
before any card command. Thus eMMC access is not intrinsically required for
an intended **failure completion**. Conversely, zero transport status alone
does not establish a successful authenticated RPMB operation: the returned
frame has its own result and secure validation.

The inspected HiKey960 reference corroborates a nonzero-result branch calling
the saved firmware callback without first validating storage frames. FF04 does
not install that callback. Reconstructing this state might exercise an error
branch without a producer or SGI, but requires the matching handler, callback,
and context. Those are unknown on Anne. The diagnostic's SGI guard is not proof
that notification delivery is a firmware ABI prerequisite. See
[request-path research](RPMB_REQUEST_PATHS.md) for the reference's limits.

The previous unsupported FFF3 return is consistent with the vendor driver's
statement that secure-world debug SMCs are disabled in user firmware. eMMC
initialization does not establish an enable mechanism. The HiKey960 HISEE
producer is unverified on Anne; the saved Anne stock kernel disables
`CONFIG_HISI_HISEE`. TrustedCore's RPMB agent completes through its own protocol,
not FF06. No verified Anne production trigger has been identified.

## With eMMC

A useful backend needs more than sector reads and writes:

- RPMB hardware-partition selection and exclusive access coordinated with
  other card clients.
- CMD23 block counts, CMD25 request frames, CMD18 responses, CMD13 completion
  polling, and reliable writes for authenticated data writes. Counter/read
  operations also send a request frame; that does not change stored RPMB data.
- 512-byte transport frames with 256-byte data units, bounded frame counts,
  endian handling, timeouts, and actual EXT_CSD capabilities. Ame's current
  registration profile still assumes vendor and region bytes.
- Real returned responses/errors, serialization across counter/write phases,
  and support for successive notifications and transfer chunks.

The normal-world transport can relay secure firmware's prepared frames to the
already provisioned card; it need not extract the RPMB secret. Card/firmware key
agreement and secure validation must still succeed. An empty RAM disk or an
invented success frame does not establish that agreement.

Ame has no Anne MMC backend; its Switch SDMMC1 code targets other hardware.
Also, `rpmb_diagnostics.cc` always completes with `0xff05` and allows only one
completion per arm. A new storage driver would not change this. A production
service must dispatch actual requests, supply responses, pass the actual
transport result to FF06, and handle subsequent requests.

Even that would not enable the disabled debug trigger or supply a missing
production request. A matching Anne producer or verified reconstruction of
its consumed state remains necessary. Stock logs establish a 4 MiB RPMB device
and an initialized RPMB bootloader module, not an end-to-end FF06 transaction.

## Validation and next decisive evidence

Seven focused host tests passed: five RPMB tests with the hash-checked Linux
oracle, shell diagnostics, and identity mapping. These include 786432
differential configuration cases, IDs/layout/SGI checks, and the production
diagnostic lifecycle with fake firmware under ASan/UBSan. They establish Ame's
software behavior, not the handset's private FF06 behavior.

```sh
AME_HI6250_LINUX_ORACLE=/tmp/hi6250-src python3 -m unittest discover -s tests -p test_rpmb.py -v
python3 -m unittest discover -s tests -p test_msh_diag.py -v
python3 -m unittest discover -s tests -p test_identity_map.py -v
```

The smallest decisive device experiment is a valid Anne request followed by
the existing `FF06(0xff05, 0, 0)` error completion. It needs no new eMMC driver.
First establish ownership/initialization of the nonzero buffer, then identify
a producer from matching firmware analysis or a stock transaction trace. After
error completion works, a real counter/read response through an RPMB backend
can establish successful completion.
