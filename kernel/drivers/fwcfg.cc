// kernel/drivers/fwcfg.cc — QEMU fw_cfg DMA write helper (M2: ramfb init).
//
// fw_cfg MMIO on QEMU virt AArch64: 0x09020000 (DEVICE_BIG_ENDIAN).
//   offset 0x00: data port (u8, r/w, auto-increment)
//   offset 0x08: selector (u16, w-only, big-endian)
//   offset 0x10: DMA high word (u32, w, big-endian)
//   offset 0x14: DMA low  word (u32, w, big-endian — triggers DMA)
//
// DEVICE_BIG_ENDIAN: multi-byte guest writes must be byte-swapped so that
// QEMU presents the correct native value to the callback.

#include "anko/types.h"
#include "anko/mm.h"
#include "anko/log.h"

namespace drivers::fwcfg {

constexpr PhysAddr kFwCfgBase   = 0x0902'0000ull;
constexpr u32 kOffData          = 0x00;
constexpr u32 kOffSelector      = 0x08;
constexpr u32 kOffDmaHigh       = 0x10;
constexpr u32 kOffDmaLow        = 0x14;

// fw_cfg DMA control bits.
constexpr u32 kDmaCtlError  = 0x01;
constexpr u32 kDmaCtlSelect = 0x08;
constexpr u32 kDmaCtlWrite  = 0x10;

// fw_cfg file directory key.
constexpr u16 kKeyFileDir = 0x0019;

// DMA access descriptor — all fields big-endian.
struct FwCfgDmaAccess {
    u32 control;
    u32 length;
    u64 address;
} __attribute__((packed));
static_assert(sizeof(FwCfgDmaAccess) == 16);

// fw_cfg file directory entry.
struct FwCfgFile {
    u32  size;    // big-endian
    u16  select;  // big-endian selector key
    u16  reserved;
    char name[56];
};
static_assert(sizeof(FwCfgFile) == 64);

// QEMU ramfb config blob written to "etc/ramfb" — all big-endian.
struct RamFbConfig {
    u64 addr;    // guest-physical address of pixel buffer
    u32 fourcc;  // 0x34325258 = XR24 = XRGB8888
    u32 flags;
    u32 width;
    u32 height;
    u32 stride;  // bytes per row
} __attribute__((packed));
static_assert(sizeof(RamFbConfig) == 28);

// ---------------------------------------------------------------------------
// Byte-swap helpers
// ---------------------------------------------------------------------------
static u16 bswap16(u16 v) { return static_cast<u16>((v >> 8) | (v << 8)); }
static u32 bswap32(u32 v) {
    return ((v & 0xFFu) << 24) | (((v >> 8) & 0xFFu) << 16) |
           (((v >> 16) & 0xFFu) << 8)  |  ((v >> 24) & 0xFFu);
}
static u64 bswap64(u64 v) {
    return (static_cast<u64>(bswap32(static_cast<u32>(v))) << 32) |
            static_cast<u64>(bswap32(static_cast<u32>(v >> 32)));
}

// ---------------------------------------------------------------------------
// Register accessors
// ---------------------------------------------------------------------------
static void write_selector(u16 key) {
    asm volatile("dsb sy" ::: "memory");
    volatile u16* sel = reinterpret_cast<volatile u16*>(kFwCfgBase + kOffSelector);
    *sel = bswap16(key); // DEVICE_BIG_ENDIAN: guest must write big-endian
    asm volatile("dsb sy" ::: "memory");
}

static u8 read_data_byte() {
    asm volatile("dsb sy" ::: "memory");
    return *reinterpret_cast<volatile u8*>(kFwCfgBase + kOffData);
}

static u32 read_be32() {
    u32 v  = static_cast<u32>(read_data_byte()) << 24;
    v |= static_cast<u32>(read_data_byte()) << 16;
    v |= static_cast<u32>(read_data_byte()) << 8;
    v |= static_cast<u32>(read_data_byte());
    return v;
}

static u16 read_be16() {
    u16 v  = static_cast<u16>(read_data_byte()) << 8;
    v |= static_cast<u16>(read_data_byte());
    return v;
}

// Write physical address of FwCfgDmaAccess struct to DMA register.
// High word written first; writing low word triggers DMA.
static void trigger_dma(PhysAddr dma_phys) {
    const u32 high = static_cast<u32>(dma_phys >> 32);
    const u32 low  = static_cast<u32>(dma_phys);
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile u32*>(kFwCfgBase + kOffDmaHigh) = bswap32(high);
    asm volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile u32*>(kFwCfgBase + kOffDmaLow) = bswap32(low);
    asm volatile("dsb sy" ::: "memory");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool write_ramfb(PhysAddr pixel_phys, u32 width, u32 height) {
    // Step 1: enumerate fw_cfg file directory to find "etc/ramfb".
    write_selector(kKeyFileDir);
    const u32 count = read_be32();
    if (count == 0 || count > 256) {
        ANKO_LOG_WARN("fwcfg", "fw_cfg file dir empty or corrupt");
        return false;
    }

    u16 ramfb_key  = 0;
    u32 ramfb_size = 0;
    bool found = false;

    for (u32 i = 0; i < count; ++i) {
        const u32 fsize   = read_be32();
        const u16 fkey    = read_be16();
        (void)read_be16(); // reserved
        char name[56]{};
        for (u32 c = 0; c < 56; ++c) name[c] = static_cast<char>(read_data_byte());
        if (!found) {
            const char* want = "etc/ramfb";
            bool match = true;
            for (u32 c = 0; want[c] && c < 56; ++c) {
                if (name[c] != want[c]) { match = false; break; }
            }
            if (match) { ramfb_key = fkey; ramfb_size = fsize; found = true; }
        }
    }

    if (!found) {
        ANKO_LOG_WARN("fwcfg", "etc/ramfb not found (missing -device ramfb?)");
        return false;
    }
    if (ramfb_size != sizeof(RamFbConfig)) {
        ANKO_LOG_WARN("fwcfg", "etc/ramfb size mismatch");
        return false;
    }

    // Step 2: build RamFbConfig in a fresh physical page.
    const PhysAddr cfg_phys = mm::alloc_page();
    if (!cfg_phys) return false;
    auto* cfg    = reinterpret_cast<RamFbConfig*>(cfg_phys);
    cfg->addr    = bswap64(static_cast<u64>(pixel_phys));
    cfg->fourcc  = bswap32(0x34325258u); // XR24 / XRGB8888
    cfg->flags   = 0;
    cfg->width   = bswap32(width);
    cfg->height  = bswap32(height);
    cfg->stride  = bswap32(width * 4u);
    asm volatile("dsb sy" ::: "memory");

    // Step 3: build DMA descriptor in a fresh physical page.
    const PhysAddr dma_phys = mm::alloc_page();
    if (!dma_phys) { mm::free_page(cfg_phys); return false; }
    auto* dma    = reinterpret_cast<FwCfgDmaAccess*>(dma_phys);
    dma->control = bswap32((static_cast<u32>(ramfb_key) << 16) | kDmaCtlSelect | kDmaCtlWrite);
    dma->length  = bswap32(sizeof(RamFbConfig));
    dma->address = bswap64(static_cast<u64>(cfg_phys));
    asm volatile("dsb sy" ::: "memory");

    // Step 4: trigger DMA and poll completion.
    trigger_dma(dma_phys);

    bool ok = false;
    for (u32 i = 0; i < 1000000; ++i) {
        asm volatile("dsb sy" ::: "memory");
        const u32 status = bswap32(
            *reinterpret_cast<volatile u32*>(dma_phys)); // read control field
        if (status == 0) { ok = true; break; }
        if (status & kDmaCtlError) { ok = false; break; }
        asm volatile("yield" ::: "memory");
    }

    mm::free_page(dma_phys);
    if (!ok) {
        ANKO_LOG_WARN("fwcfg", "ramfb DMA write failed");
        mm::free_page(cfg_phys);
        return false;
    }

    ANKO_LOGF_INFO("fwcfg", "ramfb %ux%u phys=0x%lx", width, height,
                   static_cast<unsigned long>(pixel_phys));
    return true;
}

} // namespace drivers::fwcfg
