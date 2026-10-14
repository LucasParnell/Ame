# Native power commands

Updated 14 October 2026. `shutdown`, `reboot`, and `reboot fastboot` (also
`reboot bootloader`) go through init's service-control channel and its existing
`system.control` capability. A read-only kernel check rejects unsupported
operations before any service stops. Accepted requests receive a reply and a
100 ms terminal grace period before planned service teardown. A returned error
restores interrupt state and lets supervision restore the previously desired
services. A failed reset is not reported as a completed userland reboot.

The earlier path assumed that a working PSCI CPU_ON implied usable system power
calls and then waited forever if those calls returned. The captured firmware
advertises legacy PSCI, including explicit system-power function IDs. That is
not proof those functions perform the native board sequence. Generic boards now
use power IDs declared by their DT or implied by PSCI 0.2/1.0 compatibility;
an invalid conduit or unknown power function is rejected.

## Verified mechanism and register bounds

The captured phone DT has `hisi,powerhold` at **0xfff0e000**, offset **4** and no
powerhold-protect property. GPIO25's PL061 direction register is at 0x400 and its
masked bit-4 data register at 0x40. The kernel maps this one Device page in each
process address space. It preserves other direction/data bits and does not
invent a protection-register write.

Normal reboot writes **0x10** and fastboot writes **0x01** to PMIC HRST_REG0,
register index **0x18b**, at byte address **0xfff3462c**. The PMUSSI bus uses
byte-wide accesses with a four-byte stride; `libk::Mmio::write8/read8` verifies
the reason before reset. Reset pulses bit 2 at **0xfff0a510**, preserving other
bits and ordering the two edges. This is the LPM3 path already used by native
boot diagnostics. The normal command makes one pulse and returns an error if
firmware has not reset after three seconds.

Shutdown writes reason **0x00** and lowers power-hold. If external power keeps
the PMIC alive for a second, the vendor sequence requests charger boot with
reason **0x06** and resets. Thus USB-powered shutdown may enter charging boot
rather than remain off; Ame has no separate Android charging userspace.

The primary vendor references are the
[power driver](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/mntn/hisi_poweroff.c),
[reboot reason and NMI implementation](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/mntn/blackbox/platform_ap/rdr_hisi_ap_adapter.c),
[Hi6250 reboot reason values](https://github.com/LineageOS/android_kernel_huawei_hi6250/blob/97a757c4464e687a83ccc2de122b5cd960aa0e3a/drivers/hisi/ap/platform/hi6250/mntn_public_interface.h)
and PMIC byte-access implementation inspected during earlier board bring-up.
Pinned downloads and SHA-256 values are retained in
`out/anne-power-research/sources.json`; the captured phone DT is private under
`out/anne-device-private/recovery-inspection/observations/`.

## Validation

ASan/UBSan probes check PMIC byte lanes, GPIO masks, NMI bit preservation,
register-view bounds, shell syntax, request routing, and returned errors. Real
QEMU power calls check capability ownership, unsupported fastboot handling,
shutdown, and reset. Eight-core synthetic Anne workloads at EL1 and EL2 retain
working diagnostics. The native USB build and existing USB/package checks pass.

Phone deployment and command observations are recorded after the physical test;
host/QEMU tests alone do not establish successful native shutdown.
