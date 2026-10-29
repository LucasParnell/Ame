#pragma once
// ANE-LX1 (Hi6250) Mali-T830 mechanisms, each traced to a vendor or upstream
// source rather than transcribed from a register dump.
//
// Sources (pinned; see docs/ANNE_GPU_APPLIANCE.md for the full list):
//   * hisi-oss/android_kernel_huawei_hi6250-8 @ 65e545cbaa11e...
//       - drivers/.../hisi_6250_gpu.dtsi      register window + three IRQs
//       - drivers/.../hisi_6250_ipc.dtsi      mailbox-14 allocation + src/des bit mask
//       - drivers/.../hisi_6250_powerip.dtsi  g3d@14 enable/disable command words
//       - drivers/gpu/arm/r20p0-01rel0/mali_kbase_gpu_id.h  GPU_ID_PI_T83X
//   * Linux v6.12 drivers/gpu/drm/panfrost/panfrost_regs.h
//       Register names and bit positions below follow this file, so the host
//       mediator and the guest Panfrost driver cannot disagree about a name.
//
// This header is board knowledge only. Guest-visible numbers (the synthetic
// MMIO window and virtual IRQs) live in shared/include/midix/abi/anne_gpu.h,
// because the guest device tree and the manager must agree on those.
#include "anko/types.h"

namespace anne::gpu {
// Pinned Hi6250 soc_pctrl_interface.h, read-only routing research inputs.
constexpr u64 kPctrlBase = 0xe8a09000;
constexpr u32 kPctrlGpuMasterIds = 0xc0c; // PERI_CTRL8, four 5-bit bus master IDs
constexpr u32 kPctrlIntegration = 0x68;   // PERI_CTRL25, SMMU security override bit 11

// clk-hi3xxx.c + hisi_6250_clock.dtsi: firmware DVFS, MHz payload, GPU
// frequency index in SCBAKDATA bits 12..15. Voltage stays firmware-owned.
constexpr u64 kSctrlBase = 0xfff0a000;
constexpr u32 kFrequencyState = 0x41c;
constexpr u32 kClockMailbox = 13;
constexpr u32 kClockSource = 1;
constexpr u32 kClockSetRate = 0x0003030a;
constexpr u32 kOperatingMhz[] = {120, 240, 360, 480, 680, 800, 900};

// ---- Register window ------------------------------------------------------
// Vendor: hisi_6250_gpu.dtsi `gpu: mali@E8970000 { reg = <0x0 0xE8970000 0x0
// 0x4000>; }`. The host maps exactly this window; nothing else is reachable.
constexpr u64 kBase = 0xe8970000;
constexpr u32 kBytes = 0x4000;

// ---- Control block (Panfrost `GPU_*`) -------------------------------------
constexpr u32 kRegId = 0x00; // GPU_ID (RO)
// GPU_MMU_FEATURES (RO): Panfrost reads bits 0..7 as IAS and bits 8..15 as OAS
// (panfrost_mmu.c `FIELD_GET(0xff, mmu_features)` / `FIELD_GET(0xff00, ...)`).
// They are address widths in bits, not an address-space count.
constexpr u32 kRegMemFeatures = 0x10;
constexpr u32 kRegMmuFeatures = 0x14;
constexpr u32 kRegAsPresent = 0x18;    // GPU_AS_PRESENT (RO)
constexpr u32 kRegIntRawstat = 0x20;   // GPU_INT_RAWSTAT (RO)
constexpr u32 kRegIntClear = 0x24;     // GPU_INT_CLEAR (WO)
constexpr u32 kRegIntMask = 0x28;      // GPU_INT_MASK (RW)
constexpr u32 kRegCmd = 0x30;          // GPU_CMD (WO)
constexpr u32 kRegPwrKey = 0x50;       // GPU_PWR_KEY (WO)
constexpr u32 kRegPwrOverride1 = 0x58; // GPU_PWR_OVERRIDE1 (RW)
// GPU_CMD opcodes, in the order Panfrost defines them.
constexpr u32 kCmdSoftReset = 0x01;
constexpr u32 kCmdHardReset = 0x02;
constexpr u32 kCmdPerfcntClear = 0x03;
constexpr u32 kCmdPerfcntSample = 0x04;
constexpr u32 kCmdCycleCountStart = 0x05;
constexpr u32 kCmdCycleCountStop = 0x06;
constexpr u32 kCmdCleanCaches = 0x07;
constexpr u32 kCmdCleanInvCaches = 0x08;
// Performance counters are the only window that can program a DMA address, so
// the appliance refuses to enable them: the whole block is read-as-zero/write-
// ignored and a nonzero write is denied.
constexpr u32 kRegPerfcntLo = 0x60;
constexpr u32 kRegPerfcntHi = 0x7c;
// Shader/Tiler/L2 core power-on and power-off bitmaps, written as WO during
// power-on/off; Panfrost then reads STACK_PWRON_* to confirm the cores came up.
// The window spans the whole block (0x180 SHADER_PWRON_LO .. 0x1e4 L2_PWROFF_HI).
// This is NOT a coherency block: GPU_COHERENCY_FEATURES is a single RO register
// at 0x300 and needs no write permission.
constexpr u32 kRegCorePowerLo = 0x180;
constexpr u32 kRegCorePowerHi = 0x1e4;
constexpr u32 kRegCoherencyFeatures = 0x300; // RO, probe input only
// GPU_IRQ_RESET_COMPLETED is raw-interrupt bit 8; the clear register takes the
// same bit positions (mali.cc historically wrote 0x100 here).
constexpr u32 kIrqResetCompleted = 1u << 8;

// ---- Job manager (Panfrost `JOB_INT_*`) -----------------------------------
constexpr u32 kRegJobIntClear = 0x1004;
constexpr u32 kRegJobIntMask = 0x1008;

// ---- MMU (Panfrost `MMU_INT_*`, `MMU_AS*`) --------------------------------
constexpr u32 kRegMmuIntClear = 0x2004;
constexpr u32 kRegMmuIntMask = 0x2008;
// MMU_BASE = 0x2400; MMU_AS(as) = MMU_BASE + (as << 6). One slot is advertised
// to the guest, so the seven-slot stride is never exposed.
constexpr u32 kMmuBase = 0x2400;
constexpr u32 kAsShift = 6;
constexpr u32 kAddressSpace = 0; // the only slot the guest may use
constexpr u32 kAsOffset = kMmuBase + (kAddressSpace << kAsShift);
constexpr u32 kAsTranstabLo = kAsOffset + 0x00;
constexpr u32 kAsTranstabHi = kAsOffset + 0x04;
constexpr u32 kAsMemattrLo = kAsOffset + 0x08;
constexpr u32 kAsMemattrHi = kAsOffset + 0x0C;
constexpr u32 kAsLockaddrLo = kAsOffset + 0x10;
constexpr u32 kAsLockaddrHi = kAsOffset + 0x14;
constexpr u32 kAsCommand = kAsOffset + 0x18;
constexpr u32 kAsStatus = kAsOffset + 0x28; // AS_STATUS (RO), bit 0 = busy
constexpr u32 kAsStride = 1u << kAsShift;   // one slot per address space
// The MMU block spans 0x2400..0x27ff for eight slots; the guest may only reach
// the first, so this is the containment boundary for AS writes.
constexpr u32 kMmuBytes = 0x400;
// Advertised MMU geometry. Only a 32-bit GPU VA is offered, so the host-owned
// shadow tables never have to describe a 40-bit guest address. OAS is the
// Midgard 40-bit output physical address the shadow builder also assumes.
constexpr u32 kMmuInputAddressBits = 32;
constexpr u32 kMmuOutputAddressBits = 40;
constexpr u32 kMmuFeatureIasMask = 0xff;
// Panfrost AS_COMMAND opcodes.
constexpr u32 kAsCmdNop = 0x00;
constexpr u32 kAsCmdUpdate = 0x01;
constexpr u32 kAsCmdLock = 0x02;
constexpr u32 kAsCmdUnlock = 0x03;
// 0x04 is FLUSH/FLUSH_PT, 0x05 is FLUSH_MEM: both stop walks and drain
// in-flight accesses, which is what a root swap requires.
constexpr u32 kAsCmdFlush = 0x04;
constexpr u32 kAsCmdFlushMem = 0x05;
// Host-owned MEMATTR. The guest's write is ignored, never forwarded.
constexpr u64 kMemattr = 0x888d88;

// ---- Job slots (Panfrost `JS_*`) -----------------------------------------
// JS_BASE = 0x1800, JS_SLOT_STRIDE = 0x80. A slot's CONFIG carries START_MMU
// (bit 10) and its low nibble must name a real address space; JS_COMMAND 0x01
// starts the slot. Those are the two write classes that can make the GPU fetch
// a job descriptor, so both are checked before being forwarded.
constexpr u32 kJsBase = 0x1800;
constexpr u32 kJsSlotCount = 3;
constexpr u32 kJsSlotStride = 0x80;
constexpr u32 kJsSlotBytes = kJsSlotCount * kJsSlotStride; // whole block is emulated
constexpr u32 kJsRegisterMask = kJsSlotStride - 1;
constexpr u32 kJsRegConfig = 0x18;      // JS_CONFIG(n)
constexpr u32 kJsRegConfigNext = 0x58;  // JS_CONFIG_NEXT(n)
constexpr u32 kJsRegCommand = 0x20;     // JS_COMMAND(n)
constexpr u32 kJsRegCommandNext = 0x60; // JS_COMMAND_NEXT(n)
// JS_CONFIG_START_MMU is an erratum workaround, not a bypass control: Panfrost
// sets it only for HW_ISSUE_10649, which hw_issues_t830 does NOT include. On a
// T830 it is therefore absent from every legitimate job, and requiring it would
// reject all real submissions.
constexpr u32 kJsConfigStartMmu = 1u << 10;
// JS_CONFIG low nibble is the address-space index: panfrost_job.c does
// `cfg = panfrost_mmu_as_get(pfdev, job->mmu)` and ORs the result into
// JS_CONFIG_NEXT. This field, not START_MMU, decides which page table the slot
// walks, so it is the one the mediator must pin to the single owned space (0).
constexpr u32 kJsConfigAsMask = 0x0f;
// JS_COMMAND_START.
constexpr u32 kJsCommandStart = 0x01;

// ---- Implementation-specific config (Panfrost `GPU_*_CONFIG`) -------------
constexpr u32 kRegJmConfig = 0xf00;
constexpr u32 kRegShaderConfig = 0xf04;
constexpr u32 kRegTilerConfig = 0xf08;
constexpr u32 kRegL2MmuConfig = 0xf0c;

// ---- GPU identity ---------------------------------------------------------
// Vendor mali_kbase_gpu_id.h: GPU_ID_PI_T83X = 0x0830 sits in the PRODUCT_ID
// field, bits 16..31 (GPU_ID_VERSION_PRODUCT_ID_SHIFT = 16). A T830 therefore
// reads 0x0830xxxx, i.e. (GPU_ID >> 16) == 0x0830.
constexpr u32 kProductIdT830 = 0x0830;
constexpr u32 kProductIdShift = 16;

// ---- Interrupts -----------------------------------------------------------
// Vendor: hisi_6250_gpu.dtsi `interrupts = <0 258 4 0 259 4 0 260 4>` named
// JOB, MMU, GPU. GIC SPIs are numbered from 32, so SPI N is INTID 32 + N.
constexpr u32 kGicSpiBase = 32;
constexpr u32 kHostIrqJob = kGicSpiBase + 258; // 290
constexpr u32 kHostIrqMmu = kGicSpiBase + 259; // 291
constexpr u32 kHostIrqGpu = kGicSpiBase + 260; // 292
constexpr u32 kIrqCount = 3;

// ---- LPM3 power mailbox ---------------------------------------------------
// Vendor: hisi_6250_ipc.dtsi lists mailbox-14 as GIC_2 -> LPM3, "IP_REGULATOR".
// The vendor regulator driver sends g3d's enable/disable steps through
// HISI_RPROC_LPM3_MBX14 and requires a manual ACK.
constexpr u64 kIpcBase = 0xe896b000;
constexpr u32 kMailboxStride = 64;
constexpr u32 kG3dMailbox = 14;
// `src_bit`/`des_bit` are a bit mask, not an index: source GIC_2 is bit 1 and
// destination LPM3 is bit 3 (hisi_6250_ipc.dtsi comment block).
constexpr u32 kSourceGic2 = 1u << 1;
constexpr u32 kDestinationLpm3 = 1u << 3;
// Vendor: hisi_6250_powerip.dtsi g3d@14 carries
//   hi3xxx_to_lpm3_enable_step  = <0x00030004 0x00>
//   hi3xxx_to_lpm3_disable_step = <0x00030104 0x00>
// and the regulator driver calls the send successful only when
// ack[0] == command and (ack[1] >> 24) == 0.
constexpr u32 kCommandEnable = 0x00030004;
constexpr u32 kCommandDisable = 0x00030104;
constexpr u32 kAckStatusShift = 24;

// Mailbox register layout, offsets from the mailbox base.
constexpr u32 kMboxUnlock = 0xa00;
constexpr u32 kMboxUnlockKey = 0x1acce551;
constexpr u32 kMboxSource = 0x00;
constexpr u32 kMboxAckMask = 0x04;
constexpr u32 kMboxStatus = 0x10;
constexpr u32 kMboxStatusIdle = 0x10;
constexpr u32 kMboxStatusAck = 0x80;
constexpr u32 kMboxSourceRelease = 0x14;
constexpr u32 kMboxAck = 0x18;
constexpr u32 kMboxTrigger = 0x1c;
constexpr u32 kMboxData = 0x20;
constexpr u32 kMboxDataWords = 8;

// Firmware turn-around budgets. The GPU is not powered until LPM3 ACKs, and a
// timeout must leave an occupied mailbox to a delayed ACK rather than steal it.
constexpr u64 kPowerTimeoutNs = 300000000;
constexpr u64 kResetTimeoutNs = 100000000;
constexpr u64 kCommandTimeoutNs = 100000000;
// Bounds every poll even if the timebase stalls.
constexpr u32 kPollLimit = 1000000;
} // namespace anne::gpu
