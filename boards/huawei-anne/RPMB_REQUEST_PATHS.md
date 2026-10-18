# Production RPMB request producer search

## Live HISEE production-read attempt

Later on 16 October, the user explicitly requested testing the HISEE lead.
The existing native diagnostic registered the shared window with FF04
successfully (`x0=0`); request bytes `+0x6a00..+0x6a3f` then read back as zero.
The existing worker runner issued this reference-derived call once on CPU 1:

```text
diag sac 1 c5000020 13 0 0
```

The worker returned (`sac.state=2`, `worker_cpu=1`, diagnostic record size 112)
with **`x0=0xffffffff`**, x1/x2/x3 zero. Immediate and subsequent reads found
the request-info/status area still zero. This command did not prepare an
observable request for FF06. The result is consistent with an unsupported
service or command; it does not prove every HISEE entry point is absent.

The raw runner requires the lifecycle diagnostic to be unarmed. Consequently
its `sgi5_events=0` field is not a measurement of total interrupts, and its
trigger fields still describe the earlier FFF3 test. The event CPU stayed at
zero. No FF06 call was issued. A subsequent RPMB metadata SMC on **the same
CPU 1** returned call count `0xf`, establishing that the tested core remained
usable. The shell remained responsive. No kernel flash, reboot, or card I/O
was performed.

Command 0x13 and its zero buffer/length arguments were checked against the
hashed HiKey960 reference: that command bypasses the caller-buffer check and
builds its own COS read descriptor. This is an experimental cross-SoC probe,
not a demonstrated Anne command ABI. No key-provisioning, lifecycle-setting,
upgrade, or format commands were tried. The raw runner writes its existing
research breadcrumbs in the HMAC scratch area; request-info fields were not
fabricated.

The private captures, bounded command helper, and JSON summary are under
`out/anne-device-private/hisee-production-20261016-v1/`.

## Original static search

Search date: 2026-10-16. A production request producer was identified in the
public **HiKey960 / Hi3660 reference firmware**. It has not been established
that this producer exists or has the same interface in the ANE-LX1's Hi6250
firmware. No additional handset SMCs or flashes were performed for this search.

There is still no verified production trigger for Ame on this handset. The
existing notification-gated FF06 diagnostic is ready to consume a request;
its FFF3 counter trigger returned `0xffffffff` without an SGI 5 event.

## Two kernel paths

The pinned Huawei-derived kernel tree is
[`65e545cbaa11d132e243287b150146bc2a51c45e`](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/tree/65e545cbaa11d132e243287b150146bc2a51c45e).
It is not established as the exact source archive for this handset's firmware.

| Request path | Kernel entry | Completion |
| --- | --- | --- |
| Secure firmware notification | SGI 5, `IPI_SECURE_RPMB` → `hisi_rpmb_active()` → `rpmb_work_routine()` | `FF06(storage_result, 0, 0)` |
| TrustedCore RPMB agent | `rpmb_agent_work()` → `send_ioccmd()` → `hisi_rpmb_ioctl_cmd(RPMB_FUNC_ID_SECURE_OS, ...)` | Agent response through `TC_NS_send_event_response()` → `TC_NS_POST_SMC()` |

The agent does not call FF06. Implementing a TrustedCore secure-storage client
does not, by itself, implement the first path. These paths share storage code
but have distinct request and completion protocols. Relevant sources:
[SMP dispatcher](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/arch/arm64/kernel/smp.c),
[RPMB driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/mmc/card/hisi_rpmb.c),
[RPMB agent](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/tzdriver/agent_rpmb.c),
[agent framework](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/tzdriver/agent.c).
The newly fetched agent framework was checked against Git blob
`b4ee1d3410a283b83ed1dfa7624963bd7ee8c71a`.

## Concrete producer in the HiKey960 reference

Inspected upstream
[`hisi-bl31.bin`](https://github.com/96boards-hikey/tools-images-hikey960/blob/ca37a212d548eafb3fc501c01372ba52c3a06a46/hisi-bl31.bin),
148,968 bytes, SHA-256
`f2b8becf0c3467956ec50cd58cea040dfd83af4126b47a088614e6187af22478`.
A fresh download at that revision exactly matched the local binary and the
payload of the ELF used for disassembly. Addresses below are **binary offsets**,
not addresses to use on Anne. The names describe the observed instructions;
they are not recovered debug symbols.

1. The service dispatcher matches HISEE service `0xc5000020` and calls the
   handler at `0x5774` (call at `0xff74`). Its command is passed in `x1`.
2. Command `0x13` selects `0x59f0` in the jump table at `0x1ad70`, then calls
   `0x5554`. That routine builds an internal RPMB descriptor, sets a callback
   to `0x5500` via the pointer at `0x24530`, and calls `0x10de0` at `0x55c4`.
   This appears to be the COS storage-read path: it builds a read frame, and
   its callback logs a COS partition read failure on error.
3. `0x10de0` copies the descriptor and callback into the registered request.
   The operation-4 branch stores the callback at request offset `0x6a30`
   (`0x10f54`–`0x10f58`), prepares a read request, sets its internal state to
   2 (`0x10ff8`–`0x10ffc`), and branches to `0x1125c`.
4. `0x1125c`–`0x11260` calls the interrupt helper with interrupt 5. The helper
   at `0xb88` builds the SGIR value and writes the distributor register at
   base + `0xf00` (`0xbd4`–`0xbe8`), targeting the calling CPU and setting the
   non-secure SGI attribute.
5. The FF06 handler at `0x115c4` handles a nonzero storage result by loading
   the saved callback at request + `0x6a30` and calling it with that result
   (`0x115d0`–`0x115e8`, `0x11f8c`). Its zero-result path processes the saved
   state and storage response first.

This is a concrete chain from a production HISEE command to the SGI 5 / FF06
transaction. The reference also has HISEE mailbox handlers that reach the same
request builder: calls at `0x4948`, `0x4b08`, `0x4c08`, and `0x51e4`.

### Reconstructing the consumed state

A production producer is one way to initialize the transaction. It is not
established as the only way to exercise FF06. In particular, the inspected
reference's FF06 error branch does not test whether SGI 5 was sent or delivered.
The notification requirement in Ame's lifecycle diagnostic is a guard for
stock-style completions, not a demonstrated firmware ABI precondition.

For nonzero `x1`, the reference branch is effectively:

```text
request = registered_request_pointer
callback = load64(request + 0x6a30)
callback(x1)
return 0
```

This branch does not validate RPMB frames, counters or HMACs before calling the
callback. FF04 supplies the registered pointer and clears the shared request;
it does not install that callback. Reconstructing this error path therefore
requires a valid callback executable at EL3, its expected context, and whatever
the surrounding firmware entry/return code consumes. Reconstructing a successful
storage operation additionally requires the state-specific response data.

This makes synthetic state reconstruction a valid research direction. The
unresolved part is the matching Anne handler and callback contract, rather
than an established requirement to enable HISEE or perform eMMC I/O first.
The HiKey960 offset and callback address cannot be assumed to match Anne.

The reference shows why an unsolicited error completion can fail: its callback
is supplied by the request producer, before the notification. It does **not**
prove that a null callback explains the observed hang in Anne's different BL31.
Neither the reference callback offset nor command numbers establish the Anne
firmware's private layout or supported commands. This is a lead for matching
firmware analysis, not a validated Ame command to execute on the phone.

## What the actual stock kernel establishes

The saved stock `kernel.img` has SHA-256
`ad00caaa84c53d3ce478e562a6a1116ab720b2f0fce565c518928476b55cc268`.
Decompressing its Android boot payload produces an Image with SHA-256
`0365fea672f464956b86232fdaec67cdebe5a5a838d5194ed92be0b2f0d22562`.
This exactly matches the existing stock disassembly input.

Its embedded IKCONFIG, extracted directly from that Image, has:

```text
CONFIG_HISI_MMC_SECURE_RPMB=y
CONFIG_TZDRIVER=y
# CONFIG_HISI_HISEE is not set
# CONFIG_HISI_HISEE_MNTN is not set
# CONFIG_HISI_HISEE_NVMFORMAT_TEST is not set
# CONFIG_HISI_DEBUG_FS is not set
```

The pinned Hi6250 source tree contains HISEE Kconfig and Makefile entries, but
not the implementation files named by that Makefile. Its Kconfig option
controls the Linux client and depends on the mailbox driver. Enabling this
option in a kernel would not prove or enable the corresponding BL31 service.
Likewise, enabling Huawei debugfs adds debug callers, while the RPMB driver's
comment explicitly says the secure side disables debug SMCs in user firmware.

`FF05` is key provisioning, not a debug-enable operation. The undefined `FFF5`
slot is also not evidence of an enable switch: in the inspected HiKey960
binary it only writes 1 to MMIO `0xff3b00cc` and returns zero; it does not
install a pending request or callback. Its meaning on Anne remains unknown.

## Published P20 Lite research

[EL3XIR](https://github.com/HexHive/EL3XIR) includes a
[P20 Lite rehosting harness](https://github.com/HexHive/EL3XIR/tree/af0e3573a3f4317d089dd6fbad25bfadc7ba0c54/src/rehosting-framework/secmonRehosting/rehostingEnvironments/huaweiP20).
The original cached recursive repository listing was truncated; the focused
framework subtree was fetched separately before drawing conclusions.
The harness maps BL31 at `0x35e00000`, substitutes a minimal secure-world stub,
models MMIO with buffers, and patches the boot-time counter-frequency value.
Its call-strategy methods are empty. These files supply no production RPMB
request trigger and no retail debug-enable sequence. The proprietary firmware
artifacts are distributed encrypted, so this repository is not a public copy
of this handset's plaintext BL31.

## Remaining work

The missing evidence is a matching Anne BL31 request producer and its supported
normal-world entry point. The HISEE main-service dispatcher and COS storage
request path are now specific places to examine if matching plaintext firmware
becomes available. The saved Anne trustfirmware image is encrypted, so it does
not currently permit that static comparison. An alternative is a trace of an
actual SGI 5 transaction under the matching stock stack, identifying its
producer and call parameters.

Keep the Ame completion gated on an actual fresh notification. A local SGI 5,
a guessed request, or a callback copied from another SoC would not establish
that BL31 has created a legitimate request.

Reproducible evidence is saved under
`out/anne-device-private/rpmb-request-search-v1/`: reference provenance and
disassembly excerpts, stock-kernel hashes and extracted config, hash-verified
agent/HISEE build sources, and the relevant public research harness files.
