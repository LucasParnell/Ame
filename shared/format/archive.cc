#include "midix/format/archive.h"
#include "midix/format/bundle.h"
#include "libk/integer.h"
#include "libk/num.h"
#include "libk/str.h"

namespace midix::format {
namespace {
struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(TarHeader) == 512);

bool octal(const char* data, mx_size_t size, mx_u64_t* out) {
    mx_size_t begin = 0;
    while (begin < size && data[begin] == ' ') ++begin;
    mx_size_t end = begin;
    while (end < size && data[end] != '\0' && data[end] != ' ') ++end;
    for (mx_size_t i = end; i < size; ++i) {
        if (data[i] != '\0' && data[i] != ' ') return false;
    }
    if (begin == end) { *out = 0; return true; }
    return libk::num::parse_u64_digits(data + begin, end - begin, 8, out);
}

bool tar_name_matches(const TarHeader& header, const char* wanted) {
    const auto prefix = libk::str::length_n(header.prefix, sizeof(header.prefix));
    const auto name = libk::str::length_n(header.name, sizeof(header.name));
    const auto wanted_size = libk::str::length(wanted);
    if (wanted_size != prefix + (prefix ? 1 : 0) + name) return false;
    if (prefix && (!libk::str::equal_n(header.prefix, wanted, prefix) || wanted[prefix] != '/')) {
        return false;
    }
    return libk::str::equal_n(header.name, wanted + prefix + (prefix ? 1 : 0), name);
}

bool tar_find(libk::const_bytes archive, const char* name, libk::const_bytes* out) {
    mx_size_t offset = 0;
    while (offset <= archive.size && sizeof(TarHeader) <= archive.size - offset) {
        TarHeader header{};
        (void)libk::copy_n(libk::as_bytes(&header, sizeof(header)),
                           libk::slice(archive, offset, sizeof(header)), sizeof(header));
        if (header.name[0] == '\0') return false;
        mx_u64_t size = 0, rounded = 0, checksum = 0;
        if (!octal(header.size, sizeof(header.size), &size) ||
            !octal(header.checksum, sizeof(header.checksum), &checksum) ||
            !libk::checked_align_up(size, sizeof(TarHeader), &rounded)) return false;
        // POSIX checksum treats the checksum field as eight spaces.
        mx_u64_t actual = 0;
        constexpr auto begin = __builtin_offsetof(TarHeader, checksum);
        const auto* bytes = reinterpret_cast<const mx_u8_t*>(&header);
        for (mx_size_t i = 0; i < sizeof(header); ++i) {
            actual += i >= begin && i < begin + sizeof(header.checksum) ? ' ' : bytes[i];
        }
        if (actual != checksum) return false;
        offset += sizeof(header);
        if (rounded > archive.size - offset) return false;
        if ((header.type == '\0' || header.type == '0') && tar_name_matches(header, name)) {
            *out = libk::slice(archive, offset, size);
            return true;
        }
        offset += rounded;
    }
    return false;
}
} // namespace

bool archive_find(libk::const_bytes archive, const char* name, libk::const_bytes* out) {
    if (!archive.data || !name || !out || archive.size < sizeof(mx_u32_t)) return false;
    mx_u32_t magic = 0;
    (void)libk::copy_n(libk::as_bytes(&magic, sizeof(magic)), archive, sizeof(magic));
    if (magic != MX_BUNDLE_MAGIC) return tar_find(archive, name, out);
    if (archive.size < sizeof(MxBundleHeader)) return false;
    Bundle bundle{};
    bundle.data = archive.data;
    bundle.size = archive.size;
    (void)libk::copy_n(libk::as_bytes(&bundle.header, sizeof(bundle.header)), archive, sizeof(bundle.header));
    if (bundle.header.version != MX_BUNDLE_VERSION || bundle.header.header_size != sizeof(bundle.header) ||
        bundle.header.bundle_size < sizeof(bundle.header) || bundle.header.bundle_size > archive.size) {
        return false;
    }
    // Reuse the bounded object decoder, including its string-table checks.
    bundle.size = bundle.header.bundle_size;
    MxBundleObject entry{};
    if (!find_object(bundle, name, entry)) return false;
    *out = libk::slice(archive, entry.offset, entry.size);
    return true;
}
} // namespace midix::format
