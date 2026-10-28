# Huawei Anne eMMC, AmeFS and RPMB

The later full-volume ADMA2 sections include native filesystem write/reboot persistence evidence. Earlier PIO-only and read-only descriptions are historical. AmeFS user-area durability is independent of secure-firmware RPMB request production and FF06 completion, which retain separate limitations. See [filesystem specification](../../docs/FS_SPEC.md) for disk and wire layouts.

- [Anne eMMC and RPMB transport](#emmc)
- [RPMB SMC verification and FF06 feasibility](#rpmb_verification)
- [Native RPMB error-completion diagnostic](#rpmb_diagnostic)
- [RPMB / BL31 source audit](#rpmb_audit)
- [Production RPMB request producer search](#rpmb_request_paths)

<a id="emmc"></a>
## Anne eMMC and RPMB transport

<a id="emmc-anne-emmc-and-rpmb-transport"></a>

The driver is implemented and host-tested, with native and synthetic board
builds. User-area reads and AmeFS mounting have also been verified on the
physical handset; see the userdata installation below. This filesystem check
does not establish a working Anne secure-firmware request producer or a
returning FF06, and does not validate native eMMC writes.

`kernel/drivers/emmc/sdhci.cc` implements synchronous PIO for the Arasan SDHCI
controller at `0xff390000`. The captured running DTB identifies this controller
as `arasan,sdhci`; the separate DesignWare controllers serve SD/SDIO.

The register definitions, legacy PHY settings, 200 MHz input clock, EXT_CSD
offsets and RPMB command sequences were checked against Huawei's pinned
[SDHCI driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/mmc/host/sdhci-of-arasan.c)
and [RPMB driver](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/mmc/card/hisi_rpmb.c).
These sources are references; no Linux implementation is linked into Ame.

<a id="emmc-interface"></a>
### Interface

`anko/emmc.h` exposes `drivers::emmc::Driver`, with an injected register/time
interface for host tests and other SDHCI boards. Calls require exclusive access
and a working monotonic microsecond clock. The Anne singleton in `emmc.h`
serializes hardware access with a nonblocking gate:

```cpp
anne::rpmb::ConfigInfo config{};
if (anne::emmc::prepare(config)) {
    alignas(4) u8 sector[512];
    auto result = anne::emmc::read(0, 1, sector);
    // result == drivers::emmc::Error::Ok means the transfer completed.
}
```

`read`/`write` address only the user area, in 512-byte sectors, with 1–32 sectors
per call. Bounds use the card's EXT_CSD sector count. Boot partitions are not
exposed. `status` copies EXT_CSD and the last failure (error, command index,
SDHCI interrupt bits and R1 status) without card I/O. APIs belong in kernel
thread/syscall context, with IRQs enabled; they are not a filesystem/block IPC
service. There is no automatic initialization or card access during boot.

Initialization takes over a bootloader-powered controller, retains the power
and signal-voltage selection, disables DMA/host IRQ signals, configures the
legacy Arasan PHY, and re-enumerates with CMD0/1/2/3/7/8 at 400 kHz and one bit.
It then runs at at most 12.5 MHz. The native wrapper assumes the reference's
200 MHz input; a different bootloader source rate must be measured and supplied
to `Driver::initialize`. SoC clock gates, pinmux and regulators must already be
enabled. It does not perform full cold power-up, voltage switching, high-speed
tuning, DMA or command queueing. Byte-addressed cards and native 4K sectors are
rejected.

Each transfer checks controller errors before completion, checks R1 errors,
and polls CMD13 until READY/TRAN. User-area writes flush an enabled card cache.
Failed transfers reset CMD/DAT and attempt
CMD12 recovery. Failed writes are never replayed: a timeout can mean data
reached the card. A failed partition restore disables further I/O until card
initialization succeeds again.

<a id="emmc-rpmb"></a>
### RPMB

`Driver::rpmb` exchanges opaque 512-byte frames, whose payloads contain 256-byte
RPMB data blocks. It selects hardware partition 3 while preserving boot-enable
bits, sends CMD23 + CMD25 requests, sends a result-read request for authenticated
writes, reads responses with CMD23 + CMD18, and restores the previous partition
on both success and failure. Authenticated writes set CMD23 bit 31. Reliable
frame counts follow the existing firmware profile: 1/2 frames, or 32 when
EXT_CSD WR_REL_PARAM bit 4 supports it.

Supported request types are key programming, counter, authenticated read and
authenticated write. Normal SGI servicing excludes key programming; the
explicit provisioning operation below owns that path. Region bits are
preserved for operational frames. Response result, MAC and nonce come from
the card; transport success does not authenticate them. Huawei's normal-world
driver delegates authentication and key generation to secure firmware, and
Ame follows that boundary. Ame does not synthesize HMACs, extract the key or
replace the firmware's prepared frames.

`anne::emmc::Backend` translates the pinned Huawei shared-window states RD,
CNT, WR_CNT and WR_DATA. It validates frame/chunk bounds and retains a logical
writer reservation across WR_CNT and successive WR_DATA chunks, matching the
owner/base/length/offset. Other RPMB operations cannot interleave with that
write. Copy buffers are private to this serialized backend. Transport failures
return `0xff04`, unavailable storage returns `0xff05`, malformed requests return
`0xff03`, and unsolicited key programming returns `0xff09`. Raw card response
types and results remain in the actual returned frames for firmware validation,
including malformed response types. A malformed/failed counter response does
not retain the writer reservation. Before serving ordinary requests, the
backend checks key readiness with one counter frame, like Huawei's
`mmc_rpmb_key_status`. Missing/unknown key readiness completes requests with
`RPMB_ERR_KEY=7`; this avoids large reads on unprovisioned Hynix cards.

The key query is a presence check. A successful counter query alone does not
prove that secure firmware has the matching key; secure-world authentication
must still succeed for an operational transaction.

<a id="emmc-native-diagnostic"></a>
### Native diagnostic

```text
diag el3-rpmb arm-emmc
diag el3-rpmb serve 1
diag el3-rpmb status
# After the workload finishes:
diag el3-rpmb stop
diag el3-rpmb status
# Once worker 1 has returned and no write/request is pending:
diag el3-rpmb release
```

Select an online worker CPU different from the shell's CPU. `arm-emmc` first
reads real EXT_CSD and registers its derived RPMB configuration with FF04.
Initialization failure returns `storage_unavailable` without registering or
overwriting the firmware request buffer. `complete` runs PIO before FF06 and
passes the real transport result. `rpmb.sgi5_events - rpmb.completed_events`
must equal one; duplicate or absent notifications do not authorize a transfer.
The completion worker leaves IRQs enabled and holds no state lock over PIO or
SMCs. `release` refuses an outstanding write reservation or unfinished request.

`serve` runs a background thread pinned to the selected CPU. It checks the
notification counter, completes each new request, and immediately checks again
after FF06 for another chunk. A private timed doorbell sleeps for up to 1 ms
while idle; the worker consumes no terminal input. It stops on an ambiguous
notification count, failed FF06, or explicit `stop`. Stop takes effect after
the current card/firmware operation returns and does not discard pending work.
The service runs inside the shell process and must be started explicitly; it
is not yet a separately supervised boot service.

`complete <cpu>` remains available for one request at a time. `arm` retains the
previous error-only behavior and stock-log configuration. `counter` remains a
firmware debug trigger and can still be unsupported on retail firmware. Adding
card transport does not enable that trigger; see
[firmware findings](storage.md#rpmb_verification).

<a id="emmc-provisioning"></a>
### Provisioning

```text
diag el3-rpmb key-status
# Only for an unprogrammed card to be paired with the current secure firmware:
diag el3-rpmb provision-key
diag el3-rpmb status
```

`key-status` initializes/probes storage without registering a firmware buffer,
changing its contents or issuing an SMC. `key_state` is 0 for unknown, 1 when
the card explicitly reports key-not-programmed, and 2 when the counter query
succeeds. It cannot run concurrently with an armed service.

Provisioning is a one-time, permanent card operation. Ame first initializes
storage and registers the measured EXT_CSD profile with FF04, then verifies
that the card explicitly reports no programmed key. An existing key returns
`already_provisioned`, and uncertain/query-error states refuse programming.
It clears the frame and `key_frame_status`, calls Huawei's FF05 with zero
arguments, and polls `key_frame_status` at 5 ms intervals for up to 300 polls.
This matches `mmc_rpmb_key_store` in the pinned source.

Once the firmware publishes a PROGRAM_KEY frame, Ame sends that exact frame
once with reliable CMD23/CMD25, sends the result-read request, and reads the
real response. Success requires response `0x0100`, result zero, and a subsequent
successful counter query. No FF06 is issued for this sysfs-style provisioning
sequence. The diagnostic reports the raw SMC result, transport result,
response/result pair, final key state, counter and provisioning stage; it never
prints the key. Key bytes in the shared frame and private provisioning copies
are wiped on exit.

There is no automatic retry of PROGRAM_KEY. A transport timeout or failed
postcheck reports `provision_uncertain`; inspect key state before any later
explicit attempt. If FF05 never publishes its key frame, the request window
remains exclusively owned (`provision_pending=1`) until reboot, because that
producer can still finish late. Research operations, release, re-provisioning
and ordinary servicing remain blocked during that ownership. Ame does not
guess a firmware cancellation operation.

The existing-key refusal, post-program counter check, frame-type check, secret
erasure and conservative timeout ownership are additional checks beyond the
OSS sysfs routine. Malformed chunks and transport failures also release the
local write reservation instead of preserving Huawei's potentially stranded
counter lock. The secure caller receives the actual error or response.

<a id="emmc-validation"></a>
### Validation

```sh
python3 -m unittest discover -s tests -p test_emmc.py -v
AME_HI6250_LINUX_ORACLE=/tmp/hi6250-src \
  python3 -m unittest discover -s tests -p 'test_rpmb*.py' -v
python3 -m unittest discover -s tests -p test_identity_map.py -v
python3 -m unittest discover -s tests -p test_msh_diag.py -v
cmake --build out/anne-usb-native-dev --target anko mochi_system
```

The executable model tests the production PIO and bridge with ASan/UBSan:
sector round trips and unaligned buffers, counter/read/write sequences,
32-frame transfers, reliable-write limits, real card-result propagation,
partition restoration, command/data/clock/reset timeouts, CRC/R1 errors,
malformed frames, chunk ownership, and explicit provisioning. Lifecycle
tests cover actual storage-result forwarding, successive notifications (also
during FF06), failed initialization, duplicate notifications and write leases.
Provisioning tests cover stale status, delayed/absent frames, existing keys,
unsupported FF05, malformed frames, card errors, transport uncertainty, and
postcheck failure. The service loop tests successive, delayed and ambiguous
notifications and cancellation.

The oracle test compiles the actual hash-pinned `mmc_rpmb_*` builders and
`mmc_rpmb_key_store` with tiny host shims. It compares command directions,
counts, reliable-write bits, exact outgoing bytes (including result-request
region bits) and provisioning SMC/wait/program order. Read sizes 1–32, reliable
sizes 1/2/32, multiple regions, and the 299/300-poll boundary are covered.
Reference sources stay outside the repository. To obtain/verify them:

```sh
python3 scripts/test/fetch-rpmb-oracle.py --output /tmp/hi6250-src
# Existing cache, without network access:
python3 scripts/test/fetch-rpmb-oracle.py --output /tmp/hi6250-src --offline
```

The model cannot establish physical timing, bootloader handoff, PHY calibration
or secure-world authentication on Anne.

<a id="emmc-amefs-on-userdata-physical-verification-2026-10-16"></a>
### AmeFS on userdata: physical verification, 2026-10-16

On the ANE-LX1 running firmware `8.0.0.151(C782)`, the user authorized replacing
Android userdata with AmeFS. TWRP confirmed the existing GPT entry and sysfs
geometry before any filesystem write:

| Field | Value |
| --- | --- |
| Partition | `userdata`, `/dev/block/mmcblk0p56` |
| First LBA | 16,056,320 |
| Sectors | 106,086,400, each 512 bytes |
| Capacity | 54,316,236,800 bytes, about 50.6 GiB |
| AmeFS geometry | 13,260,800 blocks, each 4,096 bytes |

The installer unmounted both `/sdcard` and `/data`, wrote the initialized AmeFS
metadata and packaged files through TWRP, flushed, and verified all 13,025,280
initialized bytes by SHA-256 readback. The GPT header and entry-array CRCs were
checked, and the captured primary GPT remained byte-for-byte unchanged. This
was a filesystem format of the existing partition, not a GPT repartition or a
secure erase of every old data sector.

The `emmc.partition` binding now selects partition 56. Native package
`out/anne-amefs-userdata-package-v40`, run tag `anne-amefs-userdata-v40`, booted
successfully and logged:

```text
drv.blk.emmc: selected lba=16056320 sectors=106086400
drv.blk.emmc: AmeFS partition ready
io.fs: mounted midix-fs-v1 disk
io.fs: read-only root, volatile /tmp, durable /data empty
```

The USB shell listed `/ame` and read `/ame/meta/install.manifest` from disk.
The native kernel image SHA-256 is
`51b7f253a2b7226d4cae76995844eb45ea0d7a76acfa0fde736d22d7f3763ce7`.
Private installer state, partition-table backup, checksums and transcripts are
in `out/anne-device-private/amefs-userdata-20261016/`.

At v40, the volume spanned userdata's full capacity, but its writable `/data`
implementation still uses a 256-block (1 MiB) checkpoint region and fixed-size
in-memory tables. Full-capacity writable allocation was not implemented in v40. That
installation verified native mounting and reads only. The replacement below
removes those writable-capacity limits.


<a id="emmc-full-volume-storage-and-adma2-2026-10-16"></a>
### Full-volume storage and ADMA2, 2026-10-16

The v41/v42 native stack replaces the 1 MiB snapshot with disk-resident keyed
B+trees and full-volume bitmaps. `/data` can allocate across all 13,260,800
userdata blocks, less packaged files, filesystem metadata, the journal and a
32-block reclamation reserve. It does not change the GPT or consume boot,
firmware or recovery partitions. The in-memory table limits apply to `/tmp`
and legacy compatibility, not disk file count or disk size.

The SDHCI driver now uses ADMA2 for user-area transfers, retaining PIO for card
initialization and RPMB. Native logs confirm eight data lines after immutable
EXT_CSD verification, and 32-bit ADMA2. The bus remains at the verified legacy
12.5 MHz setting; HS200/HS400 tuning is not implemented. An unsupported ADMA2
capability is reported explicitly and falls back to PIO before any transfer.

The kernel pins a contiguous 68 KiB arena below 4 GiB and registers every CPU
alias as Normal Non-Cacheable before granting it to the driver. One page holds
ADMA descriptors, followed by 64 KiB of payload. Descriptors split at 32 KiB
with explicit nonzero lengths and a final END flag. DMA publish/consume barriers
order buffer ownership around command completion. Filesystem and client shared
arenas retain normal CPU cache coherence; copies cross the DMA boundary.
The controller is reset before driver ownership can transfer; a failed reset
retains board ownership, and DMA pages are never recycled during the boot.

Successful writes wait for READY/TRAN, while explicit BLK_FLUSH drains the
card's optional volatile cache. Metadata commits use three ordered flush
boundaries. Adjacent data and journal pages batch up to 64 KiB. Failed DMA
reads do not publish partial bytes; failed writes are not retried. Any ambiguous
filesystem write or timed-out block connection prevents further arena reuse
until recovery.

The register layout and descriptor sizes were checked against the pinned
Huawei source above and the [Linux SDHCI definitions](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci.h).
See [FS_SPEC §6–7](../../docs/FS_SPEC.md) for arena lifetimes, checkpoint
extensions and the physical metadata redo format.

Validation includes sanitizer tests for ADMA descriptor bounds, 1–128-sector
unaligned transfers, RPMB PIO isolation, DMA errors/timeouts and failed recovery;
2,790 power-cut cases for overwrite, truncate, unlink, rename and promotion;
physical-tail allocation, sparse 50 GiB offsets, shared-block CoW isolation and
reclamation at ENOSPC. QEMU's actual IPC/capability path writes a 3 MiB file,
checks read-only arena rights and immutable mapped views, and verifies the file
on a second boot.

Physical acceptance: v41 wrote, flushed and verified `/data/storage-check.bin`
(3,145,728 bytes), then read and verified every byte after a complete phone
reboot. Boot logs confirmed:

```text
drv.blk.emmc: bus width lba=8 sectors=0
drv.blk.emmc: ADMA2 32-bit, uncached 64 KiB arena
drv.blk.emmc: selected lba=16056320 sectors=106086400
io.fs: full-volume /data allocator and journal ready
io.fs: read-only root, volatile /tmp, full-volume durable /data
```

The final v42 build adds batched data writes and the tested equal-LSN redo fix:
a valid new checkpoint can reach media before metadata home blocks during a
shared final flush, so matching-LSN redo must also replay on mount. Its native
kernel image SHA-256 is
`aca1fa49714e8f285fb5ae4260543897af196b0a4a6213ca7581d3c11e251de9`.
Package: `out/anne-amefs-userdata-package-v42`. `fscheck write` on v42 verified
3 MiB with sync and exact readback in 7,564 ms; this includes metadata commits
and the read phase, and is not a raw eMMC throughput measurement. A further complete reboot
passed `fscheck read` on v42, verifying all 3,145,728 bytes again.

Optional disk snapshots, direct client DMA and high-speed PHY tuning remain
unimplemented. A populated DUR1 legacy volume is preserved for export rather
than silently reformatted. Private flash acknowledgements and console
transcripts live in `out/anne-device-private/amefs-userdata-20261016/`.


The full host gate also ran (226 tests) and is not green: boot/display diagnostic
fixtures still fail their workload/bounds expectations, and minimal framebuffer
fixtures still expect a three-second LED hold while `kLedHoldTicks` is zero.
The duplicate host `sys_cap_revoke` shim exposed by that run was corrected and
`test_msh_diag.py` passes on rerun. Storage, ADMA2, capability, ring and system
manifest checks pass; the remaining gate failures are not counted as storage
acceptance. The raw gate log is `out/verify/Testing/Temporary/LastTest.log`.

<a id="rpmb_verification"></a>
## RPMB SMC verification and FF06 feasibility

<a id="rpmb_verification-rpmb-smc-verification-and-ff06-feasibility"></a>

Implementation follow-up: the [eMMC/RPMB transport](storage.md#emmc) now exists with
host tests and an explicit `diag el3-rpmb arm-emmc` mode. It is not yet verified
on hardware. The captures and missing firmware-producer findings below remain
historical evidence; a completed handset transaction is still unproven.

16 October 2026. Live Ame serial observations, focused host regressions, and
review of the pinned Huawei-derived Linux driver. Service reachability and
completed RPMB transactions are separate findings.

<a id="rpmb_verification-follow-up-returning-an-ff06-error-is-still-blocked"></a>
### Follow-up: returning an FF06 error is still blocked

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

<a id="rpmb_verification-result"></a>
### Result

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

<a id="rpmb_verification-current-hardware-observations"></a>
### Current hardware observations

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
result reproduced in this run. See [diagnostic history](storage.md#rpmb_diagnostic).

<a id="rpmb_verification-without-emmc"></a>
### Without eMMC

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
[request-path research](storage.md#rpmb_request_paths) for the reference's limits.

The previous unsupported FFF3 return is consistent with the vendor driver's
statement that secure-world debug SMCs are disabled in user firmware. eMMC
initialization does not establish an enable mechanism. The HiKey960 HISEE
producer is unverified on Anne; the saved Anne stock kernel disables
`CONFIG_HISI_HISEE`. TrustedCore's RPMB agent completes through its own protocol,
not FF06. No verified Anne production trigger has been identified.

<a id="rpmb_verification-with-emmc"></a>
### With eMMC

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

<a id="rpmb_verification-validation-and-next-decisive-evidence"></a>
### Validation and next decisive evidence

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

<a id="rpmb_diagnostic"></a>
## Native RPMB error-completion diagnostic

<a id="rpmb_diagnostic-native-rpmb-error-completion-diagnostic"></a>

The new [eMMC mode](storage.md#emmc) uses `diag el3-rpmb arm-emmc` and `serve <cpu>`,
derives configuration from EXT_CSD, and services successive requests through
real card transport. `key-status` and the explicit `provision-key` operation
follow the pinned Huawei key-status and FF05 provisioning paths.
It is host-tested and hardware-unverified. The default `arm` mode and the
historical hardware observations below retain their error-only behavior.

See [16 October verification and eMMC feasibility](storage.md#rpmb_verification) for
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

The [production request search](storage.md#rpmb_request_paths) identifies a HISEE
producer in a verified HiKey960 reference binary. Its presence and command
interface are unverified on Anne; there is still no verified production
trigger for this handset. The TrustedCore RPMB agent uses a separate completion
protocol and does not supply FF06 requests through its normal kernel path.

<a id="rpmb_audit"></a>
## RPMB / BL31 source audit

<a id="rpmb_audit-rpmb-bl31-source-audit"></a>

15 October 2026. This audits Ame's EL3 diagnostics against Hi6250 Linux,
not the encrypted BL31 implementation. No phone was flashed or probed.

The oracle is `hisi-oss/android_kernel_huawei_hi6250-8` revision
`65e545cbaa11d132e243287b150146bc2a51c45e`. The cached source files were
verified against the Git blob hashes in that revision's tree.

Provenance rechecked against GitHub: both RPMB implementation files last
changed in [Huawei import b426912](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/commit/b426912ab189f556fceb39ad476b0294e2472c45),
whose source is `Agassi2-AL00_OO_EMUI8.0.0_Opensource`. This is Huawei's
vendor driver in a community-maintained Hi6250 tree, not an independently
verified source archive for ANE-LX1 `8.0.0.151(C782)`. Matching this driver
does not prove identical private BL31 behavior on that exact firmware.

- [emmc-rpmb.c](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/mmc/core/emmc-rpmb.c)
  computes the configuration from EXT_CSD. A multiplier of `0x20` gives
  **16384 data blocks** with shift 8. MMC frames are separately 512 bytes.
- [bootdevice.h](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/include/linux/bootdevice.h)
  defines the 40-byte configuration layout. Ame checks its field offsets and
  constructs the reliable-write, alignment and region fields using Linux's rules.
- [hisi_rpmb.c](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/mmc/card/hisi_rpmb.c)
  maps the request window with `ioremap_wc()`, copies the configuration, and
  registers `(physical_address, BOOT_DEVICE_EMMC=0, 0)` with `0xc600ff04`.
  Ame's Normal-NC identity mapping and store barrier match that memory policy.
  Linux calls `0xc600ff06` after servicing a storage request, passing the result
  in x1. It defines no callback pointer at request offset `0x6c38`.
- [hisi_rpmb.h](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/mmc/card/hisi_rpmb.h)
  defines the actual request layout below. The metadata IDs are defined in
  this header; the stock initialization routine does not invoke those queries.

Offsets relative to the request address, compiled from that header:

| Offset | Stock field |
| --- | --- |
| `0x0000` | 32 data frames, 512 bytes each |
| `0x4000` | Status frame, 512 bytes |
| `0x4200..0x69ff` | 10 KiB HMAC buffer |
| `0x6a00` | 32-byte `request_info` |
| `0x6a20` | 16-bit `key_frame_status` |
| `0x6a22` | 16-bit `rpmb_request_status` |
| `0x6a24` | 16-bit `rpmb_exception_status` |
| `0x6a28` | Debug record; complete request ends at `0x6a70` |

Thus `+0x6c38` and `+0x6c42` are outside the stock request structure. The shell
labels the latter `raw_6c42`, and reports that Linux defines no callback.
Existing research breadcrumbs at `+0x6900..+0x691f` overlap the HMAC buffer.
Research `el3-poke`, arbitrary SMCs, window writes and sacrificial calls remain
explicit experimental operations; they do not reproduce the stock lifecycle.
No unused firmware scratch area is established by the Linux structure alone.

The standard metadata and registration paths remain available on native Anne.
The [native error-completion diagnostic](storage.md#rpmb_diagnostic) now observes the
stock SGI 5 notification and uses FF06 with the block-device-unavailable result;
it never reports a fabricated successful storage operation.
The standalone `el3-done` query now returns `NOT_IMPLEMENTED`: zeroing a
Kirin970-specific presumed callback field does not establish a Hi6250 guard.
An operational RPMB completion path still requires a storage backend and a
pending-request lifecycle. Other explicitly selected research probes are not
validated as Linux-compatible operations by this audit.

The regular EL3 window operations are serialized across CPUs, and output buffers
are checked before firmware calls or writes. Shared-memory writes check their full width;
scan bounds use subtraction to reject address wraparound. Window reads and
scans clear caller-supplied output fields before collecting results.
Raw operations 25..28 deliberately bypass that transaction lock for research;
they can race regular operations and are not a coherent stock transaction.
Synthetic Huawei and other boards return unavailable metadata and reject native
research operations without accessing the phone's addresses or vendor SMCs.
This includes all raw operations: unsupported boards return `NOT_IMPLEMENTED`
before processing a caller-supplied result record. The valid index range is
0..30 (`MX_DIAG_EL3_MAX_INDEX`); larger indices return `INVALID`. Indices 29
and 30 use the notification-backed lifecycle, which excludes the legacy
research operations while armed.
The shell reports unsupported services, rejected registration and failed legacy
completion records as failures.

The registration profile's 4 MiB size and 8K reliable-write support come from
saved stock logs. Vendor-feature and region bytes are still assumptions, not
a captured EXT_CSD. Registration success also does not prove that storage
requests can be processed or that EL2 is available.
The shell identifies the assumed profile when registration is requested.
Saved handset logs confirm `4.00 MiB` and `RPMB 8K Bytes read/write`; the saved
kernel configuration also disables `CONFIG_HISI_MMC_RPMB_AUTO_RESIZE`, matching
the pinned base defconfig and the differential test's calculation path.

Validation:

- ASan/UBSan configuration wire-byte, legacy/multi-region and boundary tests.
- An optional differential test compiles the actual pinned Linux function and
  structure with host shims, comparing 786432 input combinations byte for byte.
  Run it with `AME_HI6250_LINUX_ORACLE=/tmp/hi6250-src python3 -m unittest
  discover -s tests -p test_rpmb.py -v`. That directory must contain the
  hash-checked `emmc-rpmb.c`, `emmc-rpmb.h`, `bootdevice.h`, `hisi_rpmb.h`,
  `global_ddr_map.h`, `share.dtsi`, `merge_hi6250_defconfig` and
  `arch_arm64_kernel_smp.c` files. The latter is `arch/arm64/kernel/smp.c`. Here
  `share.dtsi` is the source tree's
  `arch/arm64/boot/dts/auto-generate/config/hi6250/hisi_6250_bl31_kernel_share_mem.dtsi`.
- A second source test compiles the pinned request structures, service IDs,
  boot-device enum and DDR map, and checks the device-tree request subrange.
- Shell diagnostic error handling and identity-mapping regressions.
- Native Anne, synthetic Huawei and generic QEMU kernel/userland builds.
- Eight-core synthetic QEMU workloads entering at EL1 and EL2 check metadata
  unavailability and rejection of all 29 native-only operation indices.

Revalidation on 15 October: all five RPMB tests, shell diagnostics,
three-board identity-mapping checks, native Anne kernel/shell build, and the
eight-core synthetic EL1/EL2 workloads passed. These checks used no phone access.
The later v37 deployment verified flashing, boot and the new status command;
the SGI 5 / FF06 transaction remains untested on the handset. See
[RPMB_DIAGNOSTIC.md](storage.md#rpmb_diagnostic) for deployment evidence.

<a id="rpmb_request_paths"></a>
## Production RPMB request producer search

<a id="rpmb_request_paths-production-rpmb-request-producer-search"></a>

<a id="rpmb_request_paths-live-hisee-production-read-attempt"></a>
### Live HISEE production-read attempt

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

<a id="rpmb_request_paths-original-static-search"></a>
### Original static search

Search date: 2026-10-16. A production request producer was identified in the
public **HiKey960 / Hi3660 reference firmware**. It has not been established
that this producer exists or has the same interface in the ANE-LX1's Hi6250
firmware. No additional handset SMCs or flashes were performed for this search.

There is still no verified production trigger for Ame on this handset. The
existing notification-gated FF06 diagnostic is ready to consume a request;
its FFF3 counter trigger returned `0xffffffff` without an SGI 5 event.

<a id="rpmb_request_paths-two-kernel-paths"></a>
### Two kernel paths

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

<a id="rpmb_request_paths-concrete-producer-in-the-hikey960-reference"></a>
### Concrete producer in the HiKey960 reference

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

<a id="rpmb_request_paths-reconstructing-the-consumed-state"></a>
#### Reconstructing the consumed state

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

<a id="rpmb_request_paths-what-the-actual-stock-kernel-establishes"></a>
### What the actual stock kernel establishes

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

<a id="rpmb_request_paths-published-p20-lite-research"></a>
### Published P20 Lite research

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

<a id="rpmb_request_paths-remaining-work"></a>
### Remaining work

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
