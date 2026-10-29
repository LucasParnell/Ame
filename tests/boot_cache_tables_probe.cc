// Inspect every translation without executing privileged CPU instructions.
#include <cassert>
#include "anko/boot_cache_tables.h"
using namespace mm::boot_cache;

u64 descriptor(const Tables& tables, u64 address) {
    const u64 block = tables.level2[address >> 30][(address >> 21) & 511];
    if ((block & 3) != kTable) return block;
    const auto* leaf = reinterpret_cast<const u64*>(block & 0x0000fffffffff000ull);
    return leaf[(address >> 12) & 511];
}

int main() {
    Tables tables{};
    // Each pair shares an L2 block, exercising two boundary leaves and whole
    // blocks, including the last page of the 4 GiB translation window.
    const u64 cases[][4] = {
        {0x480000, 0x95e990, 0x7feff8, 0x1000}, // overlap: reject
        {0x480000, 0x95e990, 0x97fff8, 0x1000}, // same block, disjoint pages
        {0x40080000, 0x40601234, 0x44000000, 475354},
        {0x480000, 0x95e990, 0xfffff000, 4096},
        {0x480000, 0x95e990, 0x43000ffc, 16 * 1024 * 1024},
    };
    for (const auto& c : cases) {
        const bool ok = build(tables, c[0], c[1], c[2], c[3]);
        if (&c == &cases[0]) { assert(!ok); continue; }
        assert(ok && tables.leaf_count <= 4);
        assert(tables.root[0] == (reinterpret_cast<u64>(tables.level1) | kTable));
        for (u32 i = 1; i < 512; ++i) assert(!tables.root[i]);
        for (u32 i = 4; i < 512; ++i) assert(!tables.level1[i]);
        for (u64 pa = 0; pa < kLimit; pa += kPage) {
            const u64 entry = descriptor(tables, pa);
            const bool kernel = pa >= (c[0] & ~(kPage - 1)) && pa < c[1];
            const bool dtb = pa >= (c[2] & ~(kPage - 1)) && pa < c[2] + c[3];
            const u64 attrs = entry & 0xfff;
            if (kernel || dtb) {
                assert((attrs & (7 << 2)) == 0 && (attrs & kShareable) == kShareable);
                assert(bool(attrs & kReadOnly) == dtb);
                assert(bool(entry & kExecuteNever) == dtb);
            } else {
                assert((attrs & (7 << 2)) == (1 << 2) && (entry & kExecuteNever));
            }
            const u64 mask = (entry & 3) == kTable ? 0x0000fffffffff000ull : 0x0000ffffffe00000ull;
            assert((entry & mask) == (pa & mask)); // VA == PA for every translation
        }
    }
    assert(!build(tables, 0x480000, 0x480000, 0x43000000, 4096));
    assert(!build(tables, 0x480000, 0x95e990, kLimit - 4096, 4097));
    assert(!build(tables, 0x480000, 0x95e990, 0, 4096));
    assert(!build(tables, 0x480000, 0x95e990, 0x480123, 1));
    assert(!build(tables, 0x480000, kLimit + 1, 0x43000000, 4096));
}
