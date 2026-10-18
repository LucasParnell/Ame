// Bounded flattened-device-tree walker. Integer offsets avoid pointer overflow.
#include "anko/fdt.h"
#include "libk/string.h"

namespace fdt {
namespace {
bool span(u32 off, u32 len, u32 total) {
    return off <= total && len <= total - off;
}
// Return the length as well as checking bounds: callers must not scan a node
// name twice while the input may still be uncached physical memory.
u32 string_length(const u8* p, u32 len) {
    u32 i = 0;
    while (i < len && p[i]) ++i;
    return i;
}
Property classify(const char* name, u32 length) {
    // Length selects a small group before comparing bytes. No hashing of the
    // actual string is necessary: FDT already interns names by integer offset.
    switch (length) {
    case 3: if (libk::memcmp(name, "reg", 3) == 0) return Property::Reg; break;
    case 5: if (libk::memcmp(name, "width", 5) == 0) return Property::Width; break;
    case 6:
        if (libk::memcmp(name, "ranges", 6) == 0) return Property::Ranges;
        if (libk::memcmp(name, "status", 6) == 0) return Property::Status;
        if (libk::memcmp(name, "height", 6) == 0) return Property::Height;
        if (libk::memcmp(name, "stride", 6) == 0) return Property::Stride;
        if (libk::memcmp(name, "format", 6) == 0) return Property::Format;
        break;
    case 10: if (libk::memcmp(name, "compatible", 10) == 0) return Property::Compatible; break;
    case 11: if (libk::memcmp(name, "stdout-path", 11) == 0) return Property::StdoutPath; break;
    }
    return Property::Unknown;
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
    if (!header(dtb, available, &total) || !cb.begin_node || !cb.end_node ||
        (!cb.prop && !cb.property)) {
        return false;
    }
    const u32 off = libk::be32(dtb + 8), strings = libk::be32(dtb + 12);
    const u32 string_size = libk::be32(dtb + 32), size = libk::be32(dtb + 36);
    if ((off & 3) || off < 40 || strings < 40 || !span(off, size, total) ||
        !span(strings, string_size, total)) {
        return false;
    }
    // Walk-local, direct-mapped cache. Full offset tags make collisions harmless;
    // zero denotes an empty entry. Never retain a validation across blobs or
    // calls, where the same offset could refer to changed or malformed bytes.
    struct NameEntry { u32 tag; Property id; };
    NameEntry names[128]{};
    const u8* structure = dtb + off;
    const u8* string_data = dtb + strings;
    u32 pos = 0, depth = 0;
    bool root = false;
    while (span(pos, 4, size)) {
        const u32 tok = libk::be32(structure + pos);
        pos += 4;
        if (tok == kBeginNode) {
            if (depth >= 64 || (root && depth == 0)) {
                return false;
            }
            const u32 start = pos;
            const u32 length = string_length(structure + pos, size - pos);
            if (length == size - pos) {
                return false;
            }
            pos += length + 1;
            pos = (pos + 3u) & ~3u;
            if (pos > size || (!root && structure[start] != 0)) {
                return false;
            }
            root = true;
            if (!cb.begin_node(cb.ctx, reinterpret_cast<const char*>(structure + start), depth++)) {
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
            const u32 len = libk::be32(structure + pos);
            const u32 name = libk::be32(structure + pos + 4);
            pos += 8;
            if (!span(pos, len, size) || name >= string_size) {
                return false;
            }
            auto& entry = names[(name * 2654435761u) >> 25];
            const auto* property_name = reinterpret_cast<const char*>(string_data + name);
            if (entry.tag != name + 1) {
                const u32 length = string_length(string_data + name, string_size - name);
                if (length == string_size - name) return false;
                entry.tag = name + 1;
                entry.id = cb.property ? classify(property_name, length) : Property::Unknown;
            }
            if (cb.property) {
                cb.property(cb.ctx, entry.id, property_name, structure + pos, len, depth - 1);
            } else {
                cb.prop(cb.ctx, property_name, structure + pos, len, depth - 1);
            }
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
