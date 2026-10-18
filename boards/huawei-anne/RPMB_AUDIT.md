# RPMB / BL31 source audit

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
The [native error-completion diagnostic](RPMB_DIAGNOSTIC.md) now observes the
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
[RPMB_DIAGNOSTIC.md](RPMB_DIAGNOSTIC.md) for deployment evidence.
