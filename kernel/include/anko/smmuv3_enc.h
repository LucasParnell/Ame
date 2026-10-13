#pragma once
// anko/smmuv3_enc.h — pure SMMUv3 register/record encoders.
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.7 (SMMUv3 QEMU backend).
//
// This header is deliberately dependency-light and side-effect free: it holds
// the register offsets, field extraction helpers, stream-table-entry (STE)
// builders, command-queue record builders, event-record accessors, and the
// queue index arithmetic used by the SMMUv3 backend. Nothing here touches
// MMIO, so tests/smmuv3_probe.cc can validate every encoding on the host.
//
// Reference: ARM IHI 0070 (SMMUv3 architecture), and the layout QEMU models in
// hw/arm/smmuv3.c + include/hw/arm/smmuv3-common.h. All records are stored in
// host byte order (the kernel is little-endian and so is the SMMU).

#include "anko/types.h"

namespace iommu::smmuv3 {

// ---------------------------------------------------------------------------
// MMIO register map (byte offsets from the SMMUv3 base)
// ---------------------------------------------------------------------------
enum Reg : u64 {
    kRegIdr0 = 0x000,
    kRegIdr1 = 0x004,
    kRegIdr3 = 0x00C,
    kRegIdr5 = 0x014,
    kRegCr0 = 0x020,
    kRegCr0Ack = 0x024,
    kRegCr1 = 0x028,
    kRegCr2 = 0x02C,
    kRegGbpa = 0x044,
    kRegIrqCtrl = 0x050,
    kRegIrqCtrlAck = 0x054,
    kRegGerror = 0x060,
    kRegGerrorN = 0x064,
    kRegStrTabBase = 0x080, // 64-bit
    kRegStrTabBaseCfg = 0x088,
    kRegCmdqBase = 0x090, // 64-bit
    kRegCmdqProd = 0x098,
    kRegCmdqCons = 0x09C,
    kRegEventqBase = 0x0A0, // 64-bit
    kRegEventqProd = 0x0A8,
    kRegEventqCons = 0x0AC,
};

// IDR0 feature bits.
constexpr u32 kIdr0S2P = 1u << 0; // stage-2 translation supported
constexpr u32 kIdr0S1P = 1u << 1; // stage-1 translation supported
constexpr u32 kIdr0Cohacc = 1u << 4; // SMMU's own walks/structures/queues only
constexpr u32 kIdr3Ril = 1u << 10; // range invalidation supported
constexpr u32 kIdr5Gran4K = 1u << 4;
constexpr u32 kIdr1Preset = (1u << 30) | (1u << 29); // tables/queues preset

// IDR1 fields.
constexpr u32 kIdr1SidSizeShift = 0;
constexpr u32 kIdr1SidSizeMask = 0x3Fu;
constexpr u32 kIdr1EventqsShift = 16;
constexpr u32 kIdr1EventqsMask = 0x1Fu;
constexpr u32 kIdr1CmdqsShift = 21;
constexpr u32 kIdr1CmdqsMask = 0x1Fu;

// IDR5 fields.
constexpr u32 kIdr5OasMask = 0x7u;

// CR0.
constexpr u32 kCr0SmmuEn = 1u << 0;
constexpr u32 kCr0EventqEn = 1u << 2;
constexpr u32 kCr0CmdqEn = 1u << 3;

// GBPA (global bypass). Only consulted while SMMUEN == 0; we set ABORT so a
// window before SMMUEN takes effect aborts rather than bypasses translation.
constexpr u32 kGbpaAbort = 1u << 20;
constexpr u32 kGbpaUpdate = 1u << 31;

// IRQ_CTRL. We poll rather than take interrupts, so this stays zero.
constexpr u32 kIrqCtrlGerrorIrqEn = 1u << 0;
constexpr u32 kIrqCtrlPriIrqEn = 1u << 1;
constexpr u32 kIrqCtrlEventqIrqEn = 1u << 2;

// GERROR.
constexpr u32 kGerrorCmdqErr = 1u << 0;
constexpr u32 kGerrorEventqAbtErr = 1u << 2;

// STRTAB_BASE_CFG.
constexpr u32 kStrTabFmtLinear = 0u;
constexpr u32 kStrTabFmt2Level = 1u;
constexpr u32 kStrTabLog2SizeMask = 0x3Fu; // [5:0]
constexpr u32 kStrTabSplitShift = 6;       // [10:6]
constexpr u32 kStrTabFmtShift = 16;        // [17:16]

// CMDQ_PROD / CMDQ_CONS. The low (log2size+1) bits are the index+wrap; CONS
// additionally carries the CERROR field at bit 24.
constexpr u32 kCmdqConsErrShift = 24;
constexpr u32 kCmdqConsErrMask = 0x7Fu;

// Base-address alignment mask applied by the architecture to STRTAB_BASE,
// CMDQ_BASE and EVENTQ_BASE (SMMU_BASE_ADDR_MASK: bits [51:6]).
constexpr u64 kBaseAddrMask = 0x0000'FFFF'FFFF'FFC0ull;

// ---------------------------------------------------------------------------
// Record sizes (also QEMU's SMMUQueue.entry_size).
// ---------------------------------------------------------------------------
constexpr u32 kSteBytes = 64; // sizeof(STE)
constexpr u32 kCdBytes = 64;  // sizeof(CD)
constexpr u32 kCmdBytes = 16; // sizeof(Cmd)
constexpr u32 kEvtBytes = 32; // sizeof(Evt)

// ---------------------------------------------------------------------------
// Stream Table Entry (STE)
//
// We drive the SMMU with *stage-2 only* translation: the device IOVA is the
// stage-2 input address (IPA) and the STE.S2TTB page tables translate it to an
// Ame physical address. There is no stage-1 table and therefore no context
// descriptor: the assignment identity lives entirely in the STE. That is the
// same construct a hypervisor uses for an IOMMU domain and it is what the
// generic core's descriptor bit convention (APT[1] = read, AP[2] = write)
// already encodes. A missing/invalid STE aborts; there is no bypass value.
// ---------------------------------------------------------------------------
struct alignas(8) Ste {
    u32 w[16];
};
static_assert(sizeof(Ste) == kSteBytes);

// STE.Config encodings (SMMUv3 §5.2.1).
constexpr u32 kCfgAbort = 0u;  // terminate the transaction
constexpr u32 kCfgBypass = 4u; // never used by this backend
constexpr u32 kCfgS1Translate = 5u;
constexpr u32 kCfgS2Translate = 6u;
constexpr u32 kCfgNested = 7u;

// Stage-2 granule encodings for STE.S2TG.
constexpr u32 kS2Tg4Kb = 0u;
constexpr u32 kS2Tg64Kb = 1u;
constexpr u32 kS2Tg16Kb = 2u;

// STE.S2SL0: start level offset. 0b01 => begin the walk at level 1, which is
// the top of the core's 3-level, 39-bit IOVA table.
constexpr u32 kS2Sl0Level1 = 1u;

// STE.S2PS: 0b100 => 44-bit stage-2 output address size (covers all of RAM
// plus the PCI MMIO aperture on the boards we target).
constexpr u32 kS2Ps44 = 4u;

// The whole STE is zero except the fields below. An all-zero STE has Valid=0
// and therefore faults (C_BAD_STE): that is the default-deny entry.
inline Ste make_ste_invalid() {
    Ste s{};
    return s;
}

// Valid, Config=abort. Used as the explicit "this stream may not translate"
// entry; the transaction is terminated before any memory is touched.
inline Ste make_ste_abort() {
    Ste s{};
    s.w[0] = 1u | (kCfgAbort << 1);
    return s;
}

// Stage-2 translating STE. `s2ttb` is the root of the core's IOVA page table,
// `vmid` scopes this domain's translations, `t0sz` is the input-address size
// (64 - IOVA bits), `ps` is the STE.S2PS field, and `record_faults` enables
// F_TRANSLATION/F_PERMISSION event recording (STE.S2R).
inline Ste make_ste_stage2(PhysAddr s2ttb, u16 vmid, u32 t0sz, u32 sl0, u32 ps,
                           bool record_faults, bool coherent_walk = false) {
    Ste s{};
    s.w[0] = 1u | (kCfgS2Translate << 1);
    s.w[4] = static_cast<u32>(vmid) & 0xFFFFu;
    s.w[5] = (t0sz & 0x3Fu) | ((sl0 & 0x3u) << 6) | ((kS2Tg4Kb & 0x3u) << 14) |
             ((ps & 0x7u) << 16) | (1u << 19) /* S2AA64 */ |
             ((record_faults ? 1u : 0u) << 26) /* S2R */;
    // Match CPU WB mappings only when the SMMU can access them coherently.
    if (coherent_walk) s.w[5] |= (1u << 8) | (1u << 10) | (3u << 12);
    s.w[6] = static_cast<u32>(s2ttb & 0xFFFF'FFF0ull);
    s.w[7] = static_cast<u32>((s2ttb >> 32) & 0x000F'FFFFull);
    return s;
}

// ---------------------------------------------------------------------------
// Command queue records
// ---------------------------------------------------------------------------
struct Cmd {
    u32 w[4];
};
static_assert(sizeof(Cmd) == kCmdBytes);

enum CmdType : u32 {
    kCmdPrefetchConfig = 0x01,
    kCmdCfgiSte = 0x03,
    kCmdCfgiAll = 0x07,
    kCmdTlbiNhAll = 0x10,
    kCmdTlbiS12Vmall = 0x28,
    kCmdTlbiS2Ipa = 0x2A,
    kCmdTlbiNsnhAll = 0x30,
    kCmdSync = 0x46,
};

// CMD_SYNC. `signal` selects the completion notification (0 = none); we poll
// CMDQ_CONS instead of taking the CMD_SYNC interrupt.
inline Cmd make_cmd_sync(u32 signal = 0) {
    Cmd c{};
    c.w[0] = kCmdSync | ((signal & 0x3u) << 12);
    return c;
}

// TLBI_S12_VMALL: invalidate every stage-2 translation for `vmid`.
inline Cmd make_cmd_tlbi_s12_vmall(u16 vmid) {
    Cmd c{};
    c.w[0] = kCmdTlbiS12Vmall;
    c.w[1] = static_cast<u32>(vmid);
    return c;
}

// Encode `pages` 4 KiB pages as the TLBI NUM/SCALE pair: NUM in [0,31],
// SCALE in [0,31], covered pages = (NUM + 1) << SCALE >= pages.
inline void range_num_scale(u64 pages, u32* num, u32* scale) {
    u64 cover = pages == 0 ? 1u : pages;
    u32 s = 0;
    while (cover > 32u && s < 31u) {
        cover = (cover + 1u) / 2u;
        ++s;
    }
    if (cover > 32u) {
        cover = 32u;
    }
    *num = static_cast<u32>(cover - 1u);
    *scale = s;
}

// TLBI_S2_IPA: invalidate a 4 KiB-granule IPA range for `vmid`. `pages` is the
// number of 4 KiB pages to cover; the encoding over-invalidates to the next
// representable granularity, which is always safe (it never drops less than
// requested).
inline Cmd make_cmd_tlbi_s2_ipa(u16 vmid, u64 iova, u32 pages) {
    u32 num = 0;
    u32 scale = 0;
    const u64 first_page = iova >> 12;
    const u64 count = pages ? pages : 1;
    // Hardware aligns the base to SCALE. Include the leading pages in the
    // coverage calculation, otherwise an unaligned range loses its tail.
    for (;;) {
        const u64 granule = 1ull << scale;
        const u64 offset = first_page & (granule - 1);
        const u64 units = (count + offset + granule - 1) / granule;
        if (units <= 32) { num = static_cast<u32>(units - 1); break; }
        ++scale;
    }
    const u64 base = (first_page & ~((1ull << scale) - 1)) << 12;
    Cmd c{};
    c.w[0] = kCmdTlbiS2Ipa | ((num & 0x1Fu) << 12) | ((scale & 0x1Fu) << 20);
    c.w[1] = static_cast<u32>(vmid);
    c.w[2] = static_cast<u32>(base & 0xFFFF'F000ull) | (1u << 10) /* TG=4KB */;
    c.w[3] = static_cast<u32>(base >> 32);
    return c;
}

// CFGI_STE: drop any cached configuration for stream `sid` after its STE was
// rewritten. Required by the architecture whenever a configuration STE
// changes; without it a translation could keep using the previous config.
inline Cmd make_cmd_cfgi_ste(u32 sid) {
    Cmd c{};
    c.w[0] = kCmdCfgiSte;
    c.w[1] = sid;
    return c;
}

// ---------------------------------------------------------------------------
// Event queue records
// ---------------------------------------------------------------------------
struct Evt {
    u32 w[8];
};
static_assert(sizeof(Evt) == kEvtBytes);

enum EvtType : u32 {
    kEvtNone = 0x00,
    kEvtCBadStreamId = 0x02,
    kEvtFSteFetch = 0x03,
    kEvtCBadSte = 0x04,
    kEvtFStreamDisabled = 0x06,
    kEvtFTransForbidden = 0x07,
    kEvtFTranslation = 0x10,
    kEvtFAddrSize = 0x11,
    kEvtFAccess = 0x12,
    kEvtFPermission = 0x13,
    kEvtFTlbConflict = 0x20,
    kEvtFCfgConflict = 0x21,
    kEvtEPageReq = 0x24,
};

inline u32 evt_word0_type(const u32 word0) {
    return word0 & 0xFFu;
}
inline u32 evt_word0_ssid(const u32 word0) {
    return word0 >> 12;
}
inline u32 evt_sid(const Evt& e) {
    return e.w[1];
}
inline u64 evt_addr(const Evt& e) {
    return (static_cast<u64>(e.w[5]) << 32) | e.w[4];
}
inline bool evt_rnw(const Evt& e) {
    return ((e.w[3] >> 3) & 1u) != 0;
}
inline bool evt_s2(const Evt& e) {
    return ((e.w[3] >> 7) & 1u) != 0;
}
inline u32 evt_class(const Evt& e) {
    return (e.w[3] >> 8) & 0x3u;
}
inline bool evt_stall(const Evt& e) {
    return ((e.w[2] >> 31) & 1u) != 0;
}

// Short human-readable tag for an event type (fixed strings only).
inline const char* evt_type_name(u32 type) {
    switch (type) {
    case kEvtNone:
        return "none";
    case kEvtCBadStreamId:
        return "bad-streamid";
    case kEvtFSteFetch:
        return "ste-fetch";
    case kEvtCBadSte:
        return "bad-ste";
    case kEvtFStreamDisabled:
        return "stream-disabled";
    case kEvtFTransForbidden:
        return "trans-forbidden";
    case kEvtFTranslation:
        return "translation";
    case kEvtFAddrSize:
        return "addr-size";
    case kEvtFAccess:
        return "access";
    case kEvtFPermission:
        return "permission";
    case kEvtFTlbConflict:
        return "tlb-conflict";
    case kEvtFCfgConflict:
        return "cfg-conflict";
    case kEvtEPageReq:
        return "page-req";
    default:
        return "other";
    }
}

// ---------------------------------------------------------------------------
// Queue index arithmetic
//
// A queue of 2^log2size entries tracks a monotonically increasing software
// counter; the register value is the low (log2size+1) bits, where the top bit
// is the wrap flag. Comparing the full (log2size+1)-bit value distinguishes
// "empty" from "full" without a separate count.
// ---------------------------------------------------------------------------
inline u32 q_index(u32 counter, u32 log2size) {
    return counter & ((1u << log2size) - 1u);
}
inline u32 q_wrap_index(u32 counter, u32 log2size) {
    return counter & ((1u << (log2size + 1u)) - 1u);
}
inline u32 q_entries(u32 log2size) {
    return 1u << log2size;
}
inline bool q_full(u32 producer, u32 consumer, u32 log2size) {
    return q_index(producer, log2size) == q_index(consumer, log2size) &&
        q_wrap_index(producer, log2size) != q_wrap_index(consumer, log2size);
}
inline bool q_overflow_changed(u32 producer, u32 consumer) {
    return ((producer ^ consumer) & (1u << 31)) != 0;
}
inline u32 oas_bits(u32 encoding) {
    constexpr u8 widths[] = {32, 36, 40, 42, 44, 48, 52};
    return encoding < 7 ? widths[encoding] : 0;
}
// CR1.{TABLE,QUEUE}: Inner Shareable WB for coherent access; otherwise
// Non-cacheable Outer Shareable. These attributes describe SMMU accesses.
inline u32 make_cr1(bool coherent) {
    return coherent ? (3u << 10) | (1u << 8) | (1u << 6) |
        (3u << 4) | (1u << 2) | 1u : (2u << 10) | (2u << 4);
}

// ---------------------------------------------------------------------------
// Decoding helpers for the ID registers.
// ---------------------------------------------------------------------------
inline u32 idr1_sid_size(u32 idr1) {
    return (idr1 >> kIdr1SidSizeShift) & kIdr1SidSizeMask;
}
inline u32 idr1_eventq_size(u32 idr1) {
    return (idr1 >> kIdr1EventqsShift) & kIdr1EventqsMask;
}
inline u32 idr1_cmdq_size(u32 idr1) {
    return (idr1 >> kIdr1CmdqsShift) & kIdr1CmdqsMask;
}
inline u32 idr5_oas(u32 idr5) {
    return idr5 & kIdr5OasMask;
}

// STRTAB_BASE_CFG builder for the linear (single-level) stream table.
inline u32 make_strtab_base_cfg_linear(u32 log2size) {
    return (kStrTabFmtLinear << kStrTabFmtShift) | (log2size & kStrTabLog2SizeMask);
}

// CMDQ/EVENTQ base register value: address (bits [51:6]) plus the log2size in
// the low six bits, exactly as the architecture specifies.
inline u64 make_queue_base(PhysAddr pa, u32 log2size) {
    return (static_cast<u64>(pa) & kBaseAddrMask) | (log2size & 0x3Fu);
}

} // namespace iommu::smmuv3
