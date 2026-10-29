#pragma once
#include "anko/types.h"
#include "libk/string.h"

namespace mm::boot_cache {

constexpr u64 kPage = 4096;
constexpr u64 kBlock = 2ull * 1024 * 1024;
constexpr u64 kLimit = 1ull << 32;
constexpr u64 kTable = 3;
constexpr u64 kAccess = 1ull << 10;
constexpr u64 kShareable = 3ull << 8;
constexpr u64 kReadOnly = 1ull << 7;
constexpr u64 kExecuteNever = (1ull << 53) | (1ull << 54);
constexpr u64 kNormal = kAccess | kShareable; // MAIR[0] = Normal WBWA
constexpr u64 kDevice = kAccess | (1ull << 2) | kExecuteNever; // MAIR[1]

// Four boundary tables suffice for two disjoint, contiguous ranges. Complete
// 2 MiB blocks need no leaf tables. This is fixed storage, independent of DT
// node count and of the physical allocator (which does not exist yet).
struct alignas(kPage) Tables {
    u64 root[512];
    u64 level1[512];
    u64 level2[4][512];
    u64 leaves[4][512];
    u32 leaf_count;
};

inline bool map_range(Tables& tables, u64 begin, u64 end, u64 attributes) {
    begin &= ~(kPage - 1);
    end = (end + kPage - 1) & ~(kPage - 1);
    while (begin < end) {
        auto& descriptor = tables.level2[begin >> 30][(begin >> 21) & 511];
        if (!(begin & (kBlock - 1)) && end - begin >= kBlock) {
            descriptor = begin | attributes | 1; // L2 block
            begin += kBlock;
            continue;
        }
        if ((descriptor & 3) != kTable) {
            if (tables.leaf_count == 4) return false;
            auto* leaf = tables.leaves[tables.leaf_count++];
            const u64 block = begin & ~(kBlock - 1);
            for (u32 i = 0; i < 512; ++i) leaf[i] = (block + i * kPage) | kDevice | 3;
            descriptor = reinterpret_cast<u64>(leaf) | kTable;
        }
        auto* leaf = reinterpret_cast<u64*>(descriptor & 0x0000fffffffff000ull);
        const u64 boundary = (begin & ~(kBlock - 1)) + kBlock;
        const u64 stop = end < boundary ? end : boundary;
        for (; begin < stop; begin += kPage) {
            leaf[(begin >> 12) & 511] = begin | attributes | 3;
        }
    }
    return true;
}

inline bool build(Tables& tables, u64 kernel_begin, u64 kernel_end, u64 dtb, u64 size) {
    if (kernel_begin >= kernel_end || kernel_end > kLimit || !dtb || !size ||
        dtb >= kLimit || size > kLimit - dtb) return false;
    const u64 kernel_page_begin = kernel_begin & ~(kPage - 1);
    const u64 kernel_page_end = (kernel_end + kPage - 1) & ~(kPage - 1);
    const u64 dtb_page_begin = dtb & ~(kPage - 1);
    const u64 dtb_page_end = (dtb + size + kPage - 1) & ~(kPage - 1);
    // Shared pages cannot be both writable executable kernel memory and a
    // read-only, execute-never blob. Fall back rather than weaken permissions.
    if (kernel_page_begin < dtb_page_end && dtb_page_begin < kernel_page_end) return false;

    libk::memset(&tables, 0, sizeof(tables));
    tables.root[0] = reinterpret_cast<u64>(tables.level1) | kTable;
    for (u32 i = 0; i < 4; ++i) {
        tables.level1[i] = reinterpret_cast<u64>(tables.level2[i]) | kTable;
        for (u32 j = 0; j < 512; ++j) {
            tables.level2[i][j] = (u64(i) << 30) | (u64(j) << 21) | kDevice | 1;
        }
    }
    return map_range(tables, kernel_begin, kernel_end, kNormal) &&
           map_range(tables, dtb, dtb + size, kNormal | kReadOnly | kExecuteNever);
}

} // namespace mm::boot_cache
