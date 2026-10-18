# Native RPMB error-completion diagnostic

The new [eMMC mode](EMMC.md) uses `diag el3-rpmb arm-emmc` and `serve <cpu>`,
derives configuration from EXT_CSD, and services successive requests through
real card transport. `key-status` and the explicit `provision-key` operation
follow the pinned Huawei key-status and FF05 provisioning paths.
It is host-tested and hardware-unverified. The default `arm` mode and the
historical hardware observations below retain their error-only behavior.

See [16 October verification and eMMC feasibility](RPMB_VERIFICATION.md) for
the latest live result: metadata works, but this boot's nonzero request-status
word prevented arming before FF04. The successful registration and unsupported
counter results below are earlier captures.

`diag el3-rpmb` runs in Ame. It registers the shared request buffer, observes
Huawei's real secure-request notification, and can complete that request with
`FF06(RPMB_ERR_BLKDEV, 0, 0)`. The error is `0xff05`: Ame has no Anne eMMC
backend. This follows the stock driver's block-device-open failure path and
requires no card I/O or synthetic RPMB response.

The notification comes from `IPI_SECURE_RPMB`, SGI **5**, in the pinned
[Huawei SMP dispatcher](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/arch/arm64/kernel/smp.c#L902-L915).
The native GIC handler records it after EOI. No SMC is made in interrupt context.
The regular syscall path enables IRQs only around the probe's firmware calls,
preserving FIQ, SError and debug masks. No spinlock spans an SMC.

Build the native kernel and shell, then use the serial shell:

```text
diag el3-rpmb arm
diag el3-rpmb counter 1
diag el3-rpmb status
```

`arm` copies the same 40-byte assumed stock-log configuration as `el3-canary`
and calls `FF04(address, BOOT_DEVICE_EMMC, 0)`. It refuses to overwrite a buffer
whose stock request-status word is already nonzero. Existing EL3 research
operations must finish before arming; while armed they return busy, including
the raw operations that would otherwise overwrite the request or HMAC buffer.

`counter 1` queues a helper on CPU 1 that calls the stock debug counter service,
`0xc600fff3`. CPU arguments are hex. Choose an online nonzero CPU different
from the shell's current CPU. The shell returns immediately; `status` reports
whether the worker and the firmware call have returned.

The debug counter service may be disabled on retail firmware. If it returns
without an SGI 5 notification, the diagnostic has no pending request to complete
and will issue **no FF06**. An inflight trigger alone is not evidence of a
request either. No timeout, guessed status, or caller-supplied frame authorizes
completion.

Once `status` reports `rpmb.sgi5_events=1`, complete on another available CPU:

```text
diag el3-rpmb complete 2
diag el3-rpmb status
```

`complete` queues a separate helper and issues FF06 only if registration
succeeded, exactly one notification arrived after arming, and no earlier FF06
was issued for that arm. It passes `x1=0xff05, x2=0, x3=0`. It writes no frames,
status markers, callbacks, or HMAC breadcrumbs. Multiple notifications are
reported as ambiguous and completion is refused.

An acknowledged error completion reports:

```text
rpmb.ff06_started=1
rpmb.ff06_inflight=0
rpmb.ff06_returned=1
rpmb.ff06_x0=0x0
rpmb.ff06_storage_result=0xff05
```

This validates handling of an error completion. A successful counter read still
requires the eMMC backend and secure response validation. The decoded frame and
request fields in the report are captured before FF06; they are observations,
not instructions for synthesizing a request.

After the trigger and completion calls return:

```text
diag el3-rpmb release
```

Release also works when the debug trigger returned without an event, or when
registration failed without an event. It refuses while a call is inflight, a
request is uncompleted, or FF06 returned an error. It releases Ame's local
research gate; it does not unregister the firmware buffer. A blocked secure call
cannot be cancelled by a software timeout. Worker state 1 means queued/running;
it does not identify the firmware PC or prove that execution is currently at EL3.

Validation: the production lifecycle runs under ASan/UBSan with fake firmware,
including a trigger blocked until a concurrent completion, stale and duplicate
events, registration failure, unsupported debug service, and completion errors.
The source oracle checks SGI 5, the SMC IDs, error code and request offsets
against the pinned Huawei definitions. Native builds and synthetic eight-core
EL1/EL2 regressions pass. Package `anne-usb-native-package-v37-rpmb-lifecycle`
was flashed to the identified ANE-LX1 after model, firmware and unlock checks.
The phone rebooted into Ame and `diag el3-rpmb status` returned successfully,
with the probe unarmed and no FF06 issued. The new notification/completion path
has not yet been exercised on the handset. Flash and serial records are under
`out/anne-device-private/usb-native-v37-rpmb-lifecycle/`.

The subsequent hardware probe accepted FF04 registration (`x0=0`). The counter
trigger on CPU 1 returned `0xffffffff`, with `sgi5_events=0` and the worker
returned. Thus this attempt produced no firmware RPMB request. The completion
command refused to queue FF06, and `ff06_started` stayed zero. The probe was
released successfully (`armed=0`), leaving Ame's shell responsive. This is
consistent with the retail firmware disabling the counter debug service; it
does not establish the behavior of a notification-backed FF06 completion.
The five `probe-*.txt` captures and `state.json` record the sequence.

The [production request search](RPMB_REQUEST_PATHS.md) identifies a HISEE
producer in a verified HiKey960 reference binary. Its presence and command
interface are unverified on Anne; there is still no verified production
trigger for this handset. The TrustedCore RPMB agent uses a separate completion
protocol and does not supply FF06 requests through its normal kernel path.
