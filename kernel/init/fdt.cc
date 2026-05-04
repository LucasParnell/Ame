// kernel/init/fdt.cc — generic FDT/DTB structure walker.
// See anko/fdt.h for the public API.

#include "anko/fdt.h"
#include "libk/string.h"

namespace fdt {

namespace {

inline const u8* align4(const u8* p) {
    return reinterpret_cast<const u8*>((reinterpret_cast<u64>(p) + 3ull) & ~3ull);
}

} // namespace

bool walk(const u8* dtb, const Callbacks& cb) {
    if (!dtb) return false;
    if (libk::be32(dtb) != kMagic) return false;

    const u32 total_size   = libk::be32(dtb + 4);
    const u32 off_struct   = libk::be32(dtb + 8);
    const u32 off_strings  = libk::be32(dtb + 12);
    const u32 size_strings = libk::be32(dtb + 32);
    const u32 size_struct  = libk::be32(dtb + 36);

    if (!total_size || off_struct >= total_size || off_strings >= total_size) return false;
    if (off_strings + size_strings > total_size) return false;
    if (off_struct  + size_struct  > total_size) return false;

    const u8*   p       = dtb + off_struct;
    const u8*   end     = p + size_struct;
    const char* strings = reinterpret_cast<const char*>(dtb + off_strings);
    u32 depth = 0;

    while (p + 4 <= end) {
        const u32 tok = libk::be32(p);
        p += 4;

        if (tok == kBeginNode) {
            const char* name = reinterpret_cast<const char*>(p);
            while (p < end && *p) ++p;
            if (p < end) ++p;
            p = align4(p);
            if (!cb.begin_node(cb.ctx, name, depth)) return false;
            ++depth;
            continue;
        }
        if (tok == kEndNode) {
            if (depth == 0) return false;
            --depth;
            cb.end_node(cb.ctx, depth);
            continue;
        }
        if (tok == kNop) continue;
        if (tok == kEnd) break;
        if (tok != kProp) return false;
        if (p + 8 > end) return false;

        const u32 len     = libk::be32(p);
        const u32 nameoff = libk::be32(p + 4);
        p += 8;
        if (p + len > end) return false;
        if (nameoff >= size_strings) return false;

        cb.prop(cb.ctx, strings + nameoff, p, len, depth > 0 ? depth - 1 : 0);
        p = align4(p + len);
    }
    return true;
}

} // namespace fdt
