#include "anko/boot_cache.h"
#include "anko/boot_cache_tables.h"
#include "anko/fdt.h"

extern "C" char __text_start[], __kernel_end[];
extern "C" void boot_cache_restore(const u64* state, u64 kernel_begin, u64 kernel_end,
                                    u64 dtb_begin, u64 dtb_end);

namespace mm {
namespace {
boot_cache::Tables tables;
// Register order is shared with the stackless restore routine.
alignas(64) u64 saved[5]; // SCTLR, TCR, MAIR, TTBR0, TTBR1
u64 blob_begin, blob_end;

void invalidate(u64 begin, u64 end) {
    u64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (u64 p = begin & ~(line - 1); p < end; p += line) {
        asm volatile("dc ivac, %0" :: "r"(p) : "memory");
    }
}
} // namespace

BootDtbCache::BootDtbCache(PhysAddr dtb) {
    // Firmware's existing ABI requires MMU/D-cache off and DMA quiesced. Do
    // not invalidate an active cache or nest this temporary translation scope.
    u64 sctlr, daif;
    asm volatile("mrs %0, sctlr_el1; mrs %1, daif" : "=r"(sctlr), "=r"(daif));
    if ((sctlr & 5) || (daif & 0x3c0) != 0x3c0 || !dtb || (dtb & 3) ||
        dtb > boot_cache::kLimit - 40) return;
    const auto* bytes = reinterpret_cast<const u8*>(dtb);
    if (libk::be32(bytes) != fdt::kMagic) return;
    const u32 size = libk::be32(bytes + 4);
    if (size < 40 || size > 16 * 1024 * 1024 ||
        !boot_cache::build(tables, reinterpret_cast<u64>(__text_start),
                           reinterpret_cast<u64>(__kernel_end), dtb, size)) return;

    saved[0] = sctlr;
    asm volatile("mrs %0, tcr_el1; mrs %1, mair_el1; mrs %2, ttbr0_el1; mrs %3, ttbr1_el1"
                 : "=r"(saved[1]), "=r"(saved[2]), "=r"(saved[3]), "=r"(saved[4]));
    blob_begin = dtb;
    blob_end = dtb + size;
    // Writes so far were uncached. Make tables visible, then discard stale
    // firmware/speculative lines by VA, including the tables and boot stack.
    // No dirty data is discarded: this is still the M=C=0 handoff state.
    asm volatile("dsb sy" ::: "memory");
    invalidate(reinterpret_cast<u64>(__text_start), reinterpret_cast<u64>(__kernel_end));
    invalidate(blob_begin, blob_end);
    constexpr u64 mair = 0x00ff; // Normal WBWA [0], Device-nGnRnE [1]
    constexpr u64 tcr = 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12) |
                        (1ull << 23) | (2ull << 32); // 48-bit VA, 4K, EPD1, 40-bit PA
    asm volatile("dsb sy\n"
                 "msr mair_el1, %0\n"
                 "msr tcr_el1, %1\n"
                 "msr ttbr0_el1, %2\n"
                 "msr ttbr1_el1, xzr\n"
                 "isb\n"
                 "tlbi vmalle1\n"
                 "dsb sy\n"
                 "isb\n"
                 "msr sctlr_el1, %3\n"
                 "isb\n"
                 :: "r"(mair), "r"(tcr), "r"(&tables), "r"(sctlr | 0x1005)
                 : "memory");
    active_ = true;
}

BootDtbCache::~BootDtbCache() {
    if (active_) {
        // This call cannot use its stack after cleaning it. The assembly
        // routine drains all writes before disabling translation and caching.
        boot_cache_restore(saved, reinterpret_cast<u64>(__text_start),
                           reinterpret_cast<u64>(__kernel_end), blob_begin, blob_end);
    }
}
} // namespace mm
