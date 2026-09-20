// kernel/drivers/sdmmc/sdmmc.cc — Minimal T210 SDMMC1 SDHCI driver.
//
// Targets Nintendo Switch Tegra X1 T210 microSD (SDMMC1) at 0x700B0000.
// U-Boot pre-initialises the controller; we soft-reset CMD/DAT paths then
// re-enumerate the card from CMD0 to reach a clean transfer state.
//
// Only PIO mode is used (no DMA), sufficient for a 64 KB boot log write.
//
// Cross-reference: Linux sdhci-tegra.c, Hekate sdmmc/sdmmc.c (CTCaer).
// T210 SDHCI is SDHCI 3.0-compatible with Tegra vendor register extensions;
// the standard SDHCI register map is used below.

#include "anko/sdmmc.h"
#include "anko/types.h"

namespace drivers::sdmmc {

namespace {

// ---------------------------------------------------------------------------
// SDHCI register byte offsets (relative to SDMMC1 base 0x700B0000)
// ---------------------------------------------------------------------------
constexpr PhysAddr kBase = 0x700B'0000ull;

constexpr u32 kBlkSzCnt  = 0x04; // [15:0]=block_size [31:16]=block_count
constexpr u32 kArg       = 0x08;
constexpr u32 kXferMode  = 0x0C; // 16-bit; 32-bit store covers 0x0E (cmd) too
constexpr u32 kRsp0      = 0x10; // response word 0  (bits 39:8 of R1/R6/R7)
constexpr u32 kBufData   = 0x20; // 32-bit PIO data port
constexpr u32 kPresentSt = 0x24;
constexpr u32 kHostCtrl1 = 0x28; // 8-bit
constexpr u32 kTimeoutCtrl = 0x2E; // 8-bit
constexpr u32 kSoftReset = 0x2F; // 8-bit
constexpr u32 kIntStatus = 0x30; // 16-bit (normal)
constexpr u32 kErrIntSt  = 0x32; // 16-bit (error)
constexpr u32 kIntEnable = 0x34; // 16-bit (normal status enable)
constexpr u32 kErrIntEn  = 0x36; // 16-bit (error status enable)

// Present State bits
constexpr u32 kPsCmdInhibit = 1u << 0;
constexpr u32 kPsDatInhibit = 1u << 1;

// Normal Interrupt Status bits
constexpr u16 kIntCmdComplete  = 1u << 0;
constexpr u16 kIntXferComplete = 1u << 1;
constexpr u16 kIntBufWrReady   = 1u << 4;
constexpr u16 kIntBufRdReady   = 1u << 5;
constexpr u16 kIntError        = 1u << 15;

// Soft-reset bits
constexpr u8 kResetCmd = 0x02;
constexpr u8 kResetDat = 0x04;

// Command register flags [5:0] (index placed in [15:8] at call site)
constexpr u16 kRspNone = 0x00;  // no response
constexpr u16 kRsp136  = 0x01;  // 136-bit R2
constexpr u16 kRsp48   = 0x02;  // 48-bit R1/R3/R6/R7
constexpr u16 kRsp48b  = 0x03;  // 48-bit + busy R1b
constexpr u16 kCrcChk  = 1u << 3;
constexpr u16 kIdxChk  = 1u << 4;
constexpr u16 kDataPrs = 1u << 5;

// Transfer mode flags
constexpr u16 kXferRead  = 1u << 4; // DAT_DIR: 1=card→host (read)
constexpr u16 kXferWrite = 0u;      // DAT_DIR: 0=host→card (write)

// ---------------------------------------------------------------------------
// MMIO helpers — cast once; volatile ensures no CSE across barriers.
// ---------------------------------------------------------------------------
static volatile u8*  const r8  = reinterpret_cast<volatile u8* >(kBase);
static volatile u16* const r16 = reinterpret_cast<volatile u16*>(kBase);
static volatile u32* const r32 = reinterpret_cast<volatile u32*>(kBase);

static inline u32  rd32(u32 off) { return r32[off / 4]; }
static inline u16  rd16(u32 off) { return r16[off / 2]; }
static inline u8   rd8 (u32 off) { return r8 [off];     }
static inline void wr32(u32 off, u32 v) { r32[off / 4] = v; }
static inline void wr16(u32 off, u16 v) { r16[off / 2] = v; }
static inline void wr8 (u32 off, u8  v) { r8 [off]     = v; }
static inline void dsb() { asm volatile("dsb sy" ::: "memory"); }

// Spin-wait with counter on a u8 register bitmask.
static bool wait8_clear(u32 off, u8 mask, u32 spins) {
    for (u32 i = 0; i < spins; ++i) {
        dsb();
        if ((rd8(off) & mask) == 0) return true;
    }
    return false;
}

// Poll Normal Interrupt Status for `bit`; clears it on match.
// Returns false on timeout or if any error bit fires.
static bool wait_int(u16 bit, u32 spins = 2000000u) {
    for (u32 i = 0; i < spins; ++i) {
        dsb();
        const u16 st = rd16(kIntStatus);
        if (st & kIntError) {
            wr16(kIntStatus, 0xFFFFu);
            wr16(kErrIntSt, 0xFFFFu);
            return false;
        }
        if (st & bit) {
            wr16(kIntStatus, bit);
            return true;
        }
    }
    return false;
}

// Wait for CMD and optionally DAT inhibit to clear.
static bool wait_idle(bool dat, u32 spins = 2000000u) {
    const u32 mask = kPsCmdInhibit | (dat ? kPsDatInhibit : 0u);
    for (u32 i = 0; i < spins; ++i) {
        dsb();
        if (!(rd32(kPresentSt) & mask)) return true;
    }
    return false;
}

// Busy-loop delay (~1 ms at ~1 GHz, very approximate).
static void delay_ms(u32 ms) {
    for (u32 i = 0; i < ms * 50000u; ++i)
        asm volatile("" ::: "memory");
}

// ---------------------------------------------------------------------------
// Low-level command send.
// cmd_flags encodes response type + CRC/IDX/DATA bits [5:0].
// cmd_idx is placed in bits [15:8] of the command register.
// xfer_mode is written to 0x0C paired with the command word.
// ---------------------------------------------------------------------------
static bool send_cmd(u8 cmd_idx, u32 arg, u16 cmd_flags, u16 xfer_mode = 0u) {
    const bool has_data = (cmd_flags & kDataPrs) != 0u;

    if (!wait_idle(has_data)) return false;

    // Clear stale interrupt status.
    wr16(kIntStatus, 0xFFFFu);
    wr16(kErrIntSt,  0xFFFFu);
    dsb();

    wr32(kArg, arg);
    dsb();

    // Write Transfer Mode (0x0C) and Command (0x0E) as one 32-bit store.
    // Little-endian layout: bits[15:0] → 0x0C (xfer), bits[31:16] → 0x0E (cmd).
    const u16 cmd_word = static_cast<u16>((static_cast<u32>(cmd_idx) << 8u) | cmd_flags);
    const u32 pair = (static_cast<u32>(cmd_word) << 16u) | xfer_mode;
    wr32(kXferMode, pair);
    dsb();

    return wait_int(kIntCmdComplete);
}

// ACMD41 variant: R3 has no CRC; some controllers raise false CRC errors.
// Send CMD41 and read OCR regardless of the interrupt result.
static u32 send_acmd41(u32 arg) {
    if (!wait_idle(false)) return 0;
    wr16(kIntStatus, 0xFFFFu);
    wr16(kErrIntSt,  0xFFFFu);
    dsb();
    wr32(kArg, arg);
    dsb();
    const u16 cmd_word = static_cast<u16>((41u << 8u) | kRsp48); // no CRC/IDX for R3
    wr32(kXferMode, static_cast<u32>(cmd_word) << 16u);
    dsb();
    // Wait for CMD_COMPLETE or error; ignore error since R3 may trigger CRC.
    for (u32 i = 0; i < 500000u; ++i) {
        dsb();
        const u16 st = rd16(kIntStatus);
        if (st & (kIntCmdComplete | kIntError)) {
            wr16(kIntStatus, 0xFFFFu);
            wr16(kErrIntSt,  0xFFFFu);
            break;
        }
    }
    return rd32(kRsp0);
}

// ---------------------------------------------------------------------------
// Card state
// ---------------------------------------------------------------------------
static bool g_sdhc = false; // true = SDHC/SDXC (LBA addressing)
static u16  g_rca  = 0;     // Relative Card Address assigned by CMD3

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init() {
    // 1. Soft-reset CMD and DAT pipelines only (preserves U-Boot clock setup).
    wr8(kSoftReset, kResetCmd | kResetDat);
    if (!wait8_clear(kSoftReset, kResetCmd | kResetDat, 50000u)) return false;

    // Enable all status bits for polling; disable signal enables (no IRQ line).
    wr16(kIntEnable, 0xFFFFu);
    wr16(kErrIntEn,  0xFFFFu);
    wr32(0x38, 0u); // INT_SIGNAL_ENABLE = 0

    wr8(kTimeoutCtrl, 0x0Eu); // max timeout (2^27 base clocks)
    dsb();

    // 2. CMD0 — reset card to idle state.
    // ignore return: card may be in undefined state after U-Boot.
    send_cmd(0, 0u, kRspNone);
    delay_ms(2);

    // 3. CMD8 — identify SDHC capability.
    // Argument: VHS=1 (2.7–3.6 V), check pattern 0xAA.
    if (send_cmd(8, 0x000001AAu, kRsp48 | kCrcChk | kIdxChk)) {
        g_sdhc = ((rd32(kRsp0) & 0xFFu) == 0xAAu);
    } else {
        g_sdhc = false; // SDSC v1.x — uncommon; continue anyway
    }

    // 4. ACMD41 loop — wait for card to finish internal init (≤ 1 s spec).
    bool card_ready = false;
    for (u32 attempt = 0; attempt < 100u; ++attempt) {
        // CMD55 (APP_CMD with RCA=0)
        if (!send_cmd(55u, 0u, kRsp48 | kCrcChk | kIdxChk)) return false;

        const u32 hcs_arg = g_sdhc ? 0xC0FF8000u : 0x00FF8000u;
        const u32 ocr = send_acmd41(hcs_arg);
        if (ocr & (1u << 31u)) {
            g_sdhc = static_cast<bool>((ocr >> 30u) & 1u); // CCS
            card_ready = true;
            break;
        }
        delay_ms(10u);
    }
    if (!card_ready) return false;

    // 5. CMD2 — get CID (136-bit R2).
    if (!send_cmd(2u, 0u, kRsp136 | kCrcChk)) return false;

    // 6. CMD3 — get RCA.
    if (!send_cmd(3u, 0u, kRsp48 | kCrcChk | kIdxChk)) return false;
    g_rca = static_cast<u16>(rd32(kRsp0) >> 16u);

    // 7. CMD7 — select card (R1b: busy on DAT0 until ready).
    if (!send_cmd(7u, static_cast<u32>(g_rca) << 16u, kRsp48b | kCrcChk | kIdxChk))
        return false;
    wait_idle(true); // wait for card busy to deassert

    // 8. CMD55 + ACMD6 — switch to 4-bit bus.
    if (!send_cmd(55u, static_cast<u32>(g_rca) << 16u, kRsp48 | kCrcChk | kIdxChk))
        return false;
    if (!send_cmd(6u, 2u, kRsp48 | kCrcChk | kIdxChk)) return false;
    wr8(kHostCtrl1, static_cast<u8>(rd8(kHostCtrl1) | (1u << 1u))); // 4-bit DAT
    dsb();

    return true;
}

bool read_sector(u32 lba, void* buf) {
    if (!buf) return false;

    wr32(kBlkSzCnt, (1u << 16u) | 512u); // count=1, size=512
    dsb();

    const u16 cmd = static_cast<u16>((17u << 8u) | kRsp48 | kCrcChk | kIdxChk | kDataPrs);
    if (!send_cmd(17u, g_sdhc ? lba : lba * 512u, cmd, kXferRead)) return false;

    if (!wait_int(kIntBufRdReady)) return false;

    auto* dst = static_cast<volatile u32*>(buf);
    for (u32 i = 0; i < 128u; ++i) dst[i] = rd32(kBufData);
    dsb();

    return wait_int(kIntXferComplete);
}

bool write_sector(u32 lba, const void* buf) {
    if (!buf) return false;

    wr32(kBlkSzCnt, (1u << 16u) | 512u);
    dsb();

    const u16 cmd = static_cast<u16>((24u << 8u) | kRsp48 | kCrcChk | kIdxChk | kDataPrs);
    if (!send_cmd(24u, g_sdhc ? lba : lba * 512u, cmd, kXferWrite)) return false;

    if (!wait_int(kIntBufWrReady)) return false;

    const auto* src = static_cast<const volatile u32*>(buf);
    for (u32 i = 0; i < 128u; ++i) wr32(kBufData, src[i]);
    dsb();

    return wait_int(kIntXferComplete);
}

} // namespace drivers::sdmmc
