// Bounded flattened-device-tree walker. Integer offsets avoid pointer overflow.
#include "anko/fdt.h"
#include "libk/string.h"

namespace fdt {
namespace {
bool span(u32 off, u32 len, u32 total) {
    return off <= total && len <= total - off;
}
bool terminated(const u8* p, u32 len) {
    for (u32 i = 0; i < len; ++i) {
        if (p[i] == 0) {
            return true;
        }
    }
    return false;
}
bool header(const u8* dtb, usize available, u32* total) {
    if (!dtb || (available && available < 40) || libk::be32(dtb) != kMagic) {
        return false;
    }
    *total = libk::be32(dtb + 4);
    return *total >= 40 && *total <= 16 * 1024 * 1024 && (!available || *total <= available) &&
           libk::be32(dtb + 20) >= 17 && libk::be32(dtb + 24) <= 17;
}
} // namespace
bool walk(const u8* dtb, const Callbacks& cb, usize available) {
    u32 total = 0;
    if (!header(dtb, available, &total) || !cb.begin_node || !cb.end_node || !cb.prop) {
        return false;
    }
    const u32 off = libk::be32(dtb + 8), strings = libk::be32(dtb + 12);
    const u32 string_size = libk::be32(dtb + 32), size = libk::be32(dtb + 36);
    if ((off & 3) || off < 40 || strings < 40 || !span(off, size, total) ||
        !span(strings, string_size, total)) {
        return false;
    }
    u32 pos = 0, depth = 0;
    bool root = false;
    while (span(pos, 4, size)) {
        const u32 tok = libk::be32(dtb + off + pos);
        pos += 4;
        if (tok == kBeginNode) {
            if (depth >= 64 || (root && depth == 0)) {
                return false;
            }
            const u32 start = pos;
            if (!terminated(dtb + off + pos, size - pos)) {
                return false;
            }
            while (dtb[off + pos]) {
                ++pos;
            }
            ++pos;
            pos = (pos + 3u) & ~3u;
            if (pos > size || (!root && dtb[off + start] != 0)) {
                return false;
            }
            root = true;
            if (!cb.begin_node(cb.ctx, reinterpret_cast<const char*>(dtb + off + start), depth++)) {
                return false;
            }
        } else if (tok == kEndNode) {
            if (!depth) {
                return false;
            }
            cb.end_node(cb.ctx, --depth);
        } else if (tok == kProp) {
            if (!depth || !span(pos, 8, size)) {
                return false;
            }
            const u32 len = libk::be32(dtb + off + pos);
            const u32 name = libk::be32(dtb + off + pos + 4);
            pos += 8;
            if (!span(pos, len, size) || name >= string_size ||
                !terminated(dtb + strings + name, string_size - name)) {
                return false;
            }
            cb.prop(cb.ctx, reinterpret_cast<const char*>(dtb + strings + name), dtb + off + pos,
                    len, depth - 1);
            pos = (pos + len + 3u) & ~3u;
            if (pos > size) {
                return false;
            }
        } else if (tok == kEnd) {
            return root && depth == 0;
        } else if (tok != kNop) {
            return false;
        }
    }
    return false;
}
bool walk_memreserve(const u8* dtb, void* ctx, bool (*entry)(void*, u64, u64), usize available) {
    u32 total = 0;
    if (!entry || !header(dtb, available, &total)) {
        return false;
    }
    u32 pos = libk::be32(dtb + 16);
    if (pos < 40 || (pos & 7)) {
        return false;
    }
    while (span(pos, 16, total)) {
        const u64 address = libk::be64(dtb + pos), size = libk::be64(dtb + pos + 8);
        pos += 16;
        if (!address && !size) {
            return true;
        }
        if (size > ~u64{0} - address || (size && !entry(ctx, address, size))) {
            return false;
        }
    }
    return false;
}
} // namespace fdt
