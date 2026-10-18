# Anne eMMC and RPMB transport

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

## Interface

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

## RPMB

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

## Native diagnostic

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
[firmware findings](RPMB_VERIFICATION.md).

## Provisioning

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

## Validation

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

## AmeFS on userdata: physical verification, 2026-10-16

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


## Full-volume storage and ADMA2, 2026-10-16

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
