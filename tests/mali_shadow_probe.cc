#include "anko/mali_shadow.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>

struct Fixture {
    std::array<std::array<u64, 512>, 4> guest{};
    bool writable = false, deny = false;
    unsigned allocated = 0, released = 0, table_maps = 0, pte_reads = 0;
    u64 denied_table = 0;
    Fixture() {
        guest[0][0] = 0x2003;
        guest[1][0] = 0x3003;
        guest[2][0] = 0x4003;
        guest[3][0] = 0x40000000 | 0xc0 | 1;
    }
    mali::ShadowOps ops() {
        return {this,
                [](void* ctx, u64 address, u64* out) {
                    auto& f = *static_cast<Fixture*>(ctx);
                    ++f.pte_reads;
                    if (address < 0x1000 || address >= 0x5000 || (address & 7)) {
                        return false;
                    }
                    *out = f.guest[address / 4096 - 1][(address % 4096) / 8];
                    return true;
                },
                [](void* ctx, u64 guest, bool write, u64* host) {
                    auto& f = *static_cast<Fixture*>(ctx);
                    f.writable = write;
                    if (f.deny || guest < 0x40000000 || guest >= 0x40200000) {
                        return false;
                    }
                    *host = 0x10000000 + guest - 0x40000000;
                    return true;
                },
                [](void* ctx, u64* pa) {
                    auto& f = *static_cast<Fixture*>(ctx);
                    *pa = 0x20000000 + 4096 * f.allocated++;
                    return static_cast<u64*>(std::aligned_alloc(4096, 4096));
                },
                [](void* ctx, u64* page) {
                    ++static_cast<Fixture*>(ctx)->released;
                    std::free(page);
                },
                [](void*, u64*, u32) {}};
    }
};

int main() {
    for (bool writable : {false, true}) {
        Fixture f;
        f.guest[3][0] = 0x40000000 | 0x40 | (writable ? 0x80 : 0) | 1;
        mali::Shadow s{};
        auto ops = f.ops();
        assert(mali::shadow_build(ops, 0x1000, s));
        assert(s.count == 4 && s.leaves == 1 && f.writable == writable);
        assert(s.tables[3][0] == (0x10000000 | 0x40 | (writable ? 0x80 : 0) | 1));
        const auto allocated = f.allocated;
        mali::shadow_recycle(s);
        assert(mali::shadow_build(ops, 0x1000, s));
        assert(f.allocated == allocated && s.reused == 4 && s.published == 0);
        f.guest[3][0] ^= 0x80;
        mali::shadow_recycle(s);
        assert(mali::shadow_build(ops, 0x1000, s));
        assert(f.allocated == allocated && s.published == 1);
        f.guest[3][0] = 0;
        mali::shadow_recycle(s);
        assert(mali::shadow_build(ops, 0x1000, s));
        assert(s.tables[3][0] == 0 && !s.leaves && s.published == 1);
        mali::shadow_destroy(ops, s);
        assert(f.allocated == f.released);
    }
    {
        Fixture f;
        f.guest[2][0] = 0x40000000 | 0xc0 | 1; // a Mali 2 MiB leaf
        mali::Shadow s{};
        auto ops = f.ops();
        assert(mali::shadow_build(ops, 0x1000, s));
        assert(s.leaves == 512 && s.tables[3][511] == (0x101ff000 | 0xc0 | 1));
        mali::shadow_destroy(ops, s);
    }
    {
        Fixture f;
        auto ops = f.ops();
        ops.read = nullptr; // production fast path needs no per-PTE translation
        ops.map_table = [](void* ctx, u64 guest) -> const u64* {
            auto& f = *static_cast<Fixture*>(ctx);
            ++f.table_maps;
            if ((guest & 4095) || guest < 0x1000 || guest >= 0x5000 || guest == f.denied_table) {
                return nullptr;
            }
            return f.guest[guest / 4096 - 1].data();
        };
        mali::Shadow s{};
        assert(mali::shadow_build(ops, 0x1000, s));
        assert(f.table_maps == 4 && f.pte_reads == 0 && s.leaves == 1);
        assert(s.tables[3][0] == (0x10000000 | 0xc0 | 1));
        // Mapping table pages once must still observe changed PTEs/permissions.
        f.guest[3][0] ^= 0x80;
        mali::shadow_recycle(s);
        assert(mali::shadow_build(ops, 0x1000, s));
        assert(!f.writable && s.tables[3][0] == (0x10000000 | 0x40 | 1));
        // Revalidate table ownership on every build, including reused tables.
        f.denied_table = 0x4000;
        mali::shadow_recycle(s);
        assert(!mali::shadow_build(ops, 0x1000, s));
        assert(f.allocated == f.released);
        f.denied_table = 0;
        f.deny = true; // table access never bypasses leaf ownership validation
        assert(!mali::shadow_build(ops, 0x1000, s));
        assert(f.allocated == f.released);
    }
    for (unsigned failure = 0; failure < 2; ++failure) {
        Fixture f;
        if (failure == 0) {
            f.deny = true;
        } else {
            f.guest[3][0] = 0x40000000 | 0xc0 | 3; // ordinary ARM page, invalid for Mali
        }
        mali::Shadow s{};
        auto ops = f.ops();
        assert(!mali::shadow_build(ops, 0x1000, s));
        assert(!s.root && !s.count && f.allocated == f.released);
    }
    std::puts("Mali shadow MMU probe passed");
}
