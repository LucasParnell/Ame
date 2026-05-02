// kernel/init/boot_archive.cc — DTB initrd discovery + initrd archive.
//
// V0/V1/V2/V3 implementation uses a ustar archive passed via QEMU -initrd.
// The same archive may also be embedded into anko as a fallback for direct
// QEMU/ELF boots where the DTB initrd pointer is not available.
//
// Important:
//   - If the archive comes from QEMU -initrd, it is already a boot-time physical
//     blob and can be handed to init directly.
//   - If the archive comes from the kernel-linked _binary_initrd_tar_bin_*
//     symbols, it is kernel-linked data and must be copied into page-backed
//     boot-image memory before being handed to userspace.
//
// Files used by boot are:
//   manifest.v0
//   init
//   nameservice
//   io.console
//   demo.ping
//   demo.pong
//   dango.manager
//   dango.auth_probe
//   linux/Image
//   linux/initramfs.cpio
//   linux/dango-linux.dtb
//   linux/manifest.toml

#include "anko/init.h"
#include "anko/mm.h"
#include "anko/log.h"
#include "libk/string.h"
#include "libk/printf.h"

namespace kernel::init {

namespace {

constexpr u32 kFdtMagic = 0xD00D'FEEDu;
constexpr u32 FDT_BEGIN_NODE = 1;
constexpr u32 FDT_END_NODE = 2;
constexpr u32 FDT_PROP = 3;
constexpr u32 FDT_NOP = 4;
constexpr u32 FDT_END = 9;

struct TarEntry {
    char name[100];
    const u8* data;
    usize size;
};

static TarEntry g_entries[32];
static u32 g_entry_count = 0;

// If init_archive() parsed an already-existing tar blob, remember it.
// For an external QEMU -initrd this can be handed to userspace directly.
// For the embedded kernel-linked fallback it must be copied into page-backed
// memory first.
static const u8* g_source_tar_base = nullptr;
static usize g_source_tar_size = 0;
static bool g_source_tar_requires_copy = false;

extern "C" {
extern const u8 _binary_init_bin_start[];
extern const u8 _binary_init_bin_end[];
extern const u8 _binary_nameservice_bin_start[];
extern const u8 _binary_nameservice_bin_end[];
extern const u8 _binary_io_console_bin_start[];
extern const u8 _binary_io_console_bin_end[];
extern const u8 _binary_demo_ping_bin_start[];
extern const u8 _binary_demo_ping_bin_end[];
extern const u8 _binary_demo_pong_bin_start[];
extern const u8 _binary_demo_pong_bin_end[];
extern const u8 _binary_dango_manager_bin_start[];
extern const u8 _binary_dango_manager_bin_end[];
extern const u8 _binary_dango_auth_probe_bin_start[];
extern const u8 _binary_dango_auth_probe_bin_end[];
extern const u8 _binary_manifest_v0_bin_start[];
extern const u8 _binary_manifest_v0_bin_end[];

// Produced by kernel/CMakeLists.txt from build/initrd.tar.
extern const u8 _binary_initrd_tar_bin_start[];
extern const u8 _binary_initrd_tar_bin_end[];
}

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char _pad[12];
};
static_assert(sizeof(TarHeader) == 512);

constexpr usize align_up(usize v, usize a) {
    return (v + (a - 1)) & ~(a - 1);
}

bool block_is_zero(const u8* p, usize n) {
    for (usize i = 0; i < n; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

usize parse_octal(const char* s, usize n) {
    usize v = 0;
    for (usize i = 0; i < n; ++i) {
        const char c = s[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') continue;
        v = (v << 3) + static_cast<usize>(c - '0');
    }
    return v;
}

void copy_cstr(char* dst, usize dst_len, const char* src) {
    if (!dst || dst_len == 0 || !src) return;

    usize i = 0;
    for (; i + 1 < dst_len && src[i]; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void write_octal(char* dst, usize len, usize value) {
    if (!dst || len == 0) return;

    for (usize i = 0; i < len; ++i) dst[i] = '0';
    dst[len - 1] = '\0';

    if (len < 2) return;

    usize pos = len - 2;
    while (value && pos > 0) {
        dst[pos--] = static_cast<char>('0' + (value & 7u));
        value >>= 3;
    }
}

void add_embedded(const char* name, const u8* start, const u8* end) {
    if (!name || !start || !end || end <= start) return;
    if (g_entry_count >= 32) return;

    TarEntry& e = g_entries[g_entry_count++];
    libk::memset(e.name, 0, sizeof(e.name));

    const usize n = libk::strlen(name);
    const usize copy_n = (n < sizeof(e.name) - 1) ? n : (sizeof(e.name) - 1);
    libk::memcpy(e.name, name, copy_n);

    e.data = start;
    e.size = static_cast<usize>(end - start);
}

const u8* read_prop_data(const u8* p, u32 len, const u8* end) {
    if (!p || p + len > end) return nullptr;
    return p;
}

bool parse_tar_archive(const u8* base, usize size) {
    if (!base || size < 512) return false;

    usize off = 0;

    while (off + 512 <= size) {
        const u8* block = base + off;
        if (block_is_zero(block, 512)) break;

        const auto* h = reinterpret_cast<const TarHeader*>(block);
        const usize fsize = parse_octal(h->size, sizeof(h->size));
        const usize data_off = off + 512;
        const usize next_off = data_off + align_up(fsize, 512);

        if (next_off > size) {
            ANKO_LOGF_ERROR("kernel/init",
                            "initrd bad tar entry off=%llu size=%llu next=%llu total=%llu",
                            static_cast<unsigned long long>(off),
                            static_cast<unsigned long long>(fsize),
                            static_cast<unsigned long long>(next_off),
                            static_cast<unsigned long long>(size));
            return false;
        }

        if (g_entry_count < 32) {
            TarEntry& e = g_entries[g_entry_count++];
            libk::memset(e.name, 0, sizeof(e.name));

            for (usize i = 0; i < sizeof(e.name) - 1; ++i) {
                e.name[i] = h->name[i];
                if (h->name[i] == '\0') break;
            }

            e.data = base + data_off;
            e.size = fsize;
        }

        off = next_off;
    }

    return g_entry_count > 0;
}

bool alloc_contiguous_pages(usize pages, PhysAddr* out_base) {
    if (!out_base || pages == 0) return false;

    PhysAddr base = mm::alloc_page();
    if (!base) return false;

    PhysAddr expected = base + PAGE_SIZE;

    for (usize i = 1; i < pages; ++i) {
        PhysAddr pa = mm::alloc_page();
        if (!pa || pa != expected) {
            if (pa) mm::free_page(pa);

            for (usize j = 0; j < i; ++j) {
                mm::free_page(base + j * PAGE_SIZE);
            }

            return false;
        }

        expected += PAGE_SIZE;
    }

    *out_base = base;
    return true;
}

bool copy_source_tar_to_pages(PhysAddr* out_base, usize* out_size) {
    if (!out_base || !out_size) return false;
    if (!g_source_tar_base || g_source_tar_size == 0) return false;

    const usize alloc_size = align_up(g_source_tar_size, PAGE_SIZE);
    const usize pages = alloc_size / PAGE_SIZE;

    PhysAddr dst_phys = 0;
    if (!alloc_contiguous_pages(pages, &dst_phys)) {
        ANKO_LOGF_ERROR("kernel/init",
                        "initrd failed to allocate %llu contiguous pages for embedded archive",
                        static_cast<unsigned long long>(pages));
        return false;
    }

    auto* dst = reinterpret_cast<u8*>(dst_phys);
    libk::memset(dst, 0, alloc_size);
    libk::memcpy(dst, g_source_tar_base, g_source_tar_size);

    *out_base = dst_phys;
    *out_size = g_source_tar_size;

    ANKO_LOGF_INFO("kernel/init", "initrd copied embedded archive size=%llu pages=%llu",
                   static_cast<unsigned long long>(g_source_tar_size),
                   static_cast<unsigned long long>(pages));

    return true;
}

} // namespace

bool discover_initrd(PhysAddr dtb_phys, PhysAddr* out_base, usize* out_size) {
    if (!dtb_phys || !out_base || !out_size) return false;

    auto* dtb = reinterpret_cast<const u8*>(dtb_phys);
    if (libk::be32(dtb + 0) != kFdtMagic) {
        ANKO_LOG_WARN("kernel/dtb", "bad magic");
        return false;
    }

    const u32 total_size = libk::be32(dtb + 4);
    const u32 off_struct = libk::be32(dtb + 8);
    const u32 off_strings = libk::be32(dtb + 12);
    const u32 size_strings = libk::be32(dtb + 32);
    const u32 size_struct = libk::be32(dtb + 36);

    if (total_size < 40) return false;
    if (off_struct >= total_size || off_strings >= total_size) return false;
    if (off_struct + size_struct > total_size) return false;
    if (off_strings + size_strings > total_size) return false;

    const u8* p = dtb + off_struct;
    const u8* end = p + size_struct;
    const char* strings = reinterpret_cast<const char*>(dtb + off_strings);

    i64 depth = -1;
    i64 chosen_depth = -1;
    PhysAddr initrd_start = 0;
    PhysAddr initrd_end = 0;

    while (p + 4 <= end) {
        const u32 tok = libk::be32(p);
        p += 4;

        if (tok == FDT_BEGIN_NODE) {
            if (p >= end) return false;

            const char* name = reinterpret_cast<const char*>(p);
            const usize nlen = libk::strlen(name);
            p = reinterpret_cast<const u8*>(
                align_up(reinterpret_cast<usize>(p + nlen + 1), 4));

            ++depth;

            if (depth == 1 && libk::strcmp(name, "chosen") == 0) {
                chosen_depth = depth;
                ANKO_LOG_DEBUG("kernel/dtb", "chosen");
            }

            continue;
        }

        if (tok == FDT_END_NODE) {
            if (depth == chosen_depth) chosen_depth = -1;
            --depth;
            continue;
        }

        if (tok == FDT_NOP) continue;
        if (tok == FDT_END) break;
        if (tok != FDT_PROP) return false;
        if (p + 8 > end) return false;

        const u32 len = libk::be32(p);
        const u32 name_off = libk::be32(p + 4);
        p += 8;

        const u8* prop_data = read_prop_data(p, len, end);
        if (!prop_data) return false;

        const char* prop_name = strings + name_off;

        if (chosen_depth >= 0 && depth == chosen_depth) {
            if (libk::strcmp(prop_name, "linux,initrd-start") == 0 &&
                (len == 4 || len >= 8)) {
                initrd_start = (len == 4)
                    ? static_cast<PhysAddr>(libk::be32(prop_data))
                    : libk::be64(prop_data);
                ANKO_LOGF_DEBUG("kernel/dtb", "initrd-start len=%u", len);
            } else if (libk::strcmp(prop_name, "linux,initrd-end") == 0 &&
                       (len == 4 || len >= 8)) {
                initrd_end = (len == 4)
                    ? static_cast<PhysAddr>(libk::be32(prop_data))
                    : libk::be64(prop_data);
                ANKO_LOGF_DEBUG("kernel/dtb", "initrd-end len=%u", len);
            }
        }

        p = reinterpret_cast<const u8*>(
            align_up(reinterpret_cast<usize>(p + len), 4));
    }

    if (initrd_start == 0 || initrd_end <= initrd_start) {
        ANKO_LOGF_WARN("kernel/dtb", "missing initrd start=%llx end=%llx",
                       static_cast<unsigned long long>(initrd_start),
                       static_cast<unsigned long long>(initrd_end));
        return false;
    }

    *out_base = initrd_start;
    *out_size = static_cast<usize>(initrd_end - initrd_start);
    return true;
}

bool init_archive(PhysAddr initrd_base, usize initrd_size) {
    g_entry_count = 0;
    g_source_tar_base = nullptr;
    g_source_tar_size = 0;
    g_source_tar_requires_copy = false;

    if (!initrd_base || initrd_size == 0) {
        const usize embedded_tar_size = static_cast<usize>(
            _binary_initrd_tar_bin_end - _binary_initrd_tar_bin_start);

        if (embedded_tar_size >= sizeof(TarHeader) &&
            parse_tar_archive(_binary_initrd_tar_bin_start, embedded_tar_size)) {
            g_source_tar_base = _binary_initrd_tar_bin_start;
            g_source_tar_size = embedded_tar_size;
            g_source_tar_requires_copy = true;

            ANKO_LOGF_INFO("kernel/init",
                           "initrd using embedded full archive entries=%u size=%llu",
                           g_entry_count,
                           static_cast<unsigned long long>(embedded_tar_size));
            return true;
        }

        // Last-resort tiny embedded archive for early bring-up if initrd.tar was
        // not linked or could not be parsed. This path intentionally has no
        // linux/* artifacts.
        g_entry_count = 0;
        add_embedded("init", _binary_init_bin_start, _binary_init_bin_end);
        add_embedded("nameservice", _binary_nameservice_bin_start, _binary_nameservice_bin_end);
        add_embedded("io.console", _binary_io_console_bin_start, _binary_io_console_bin_end);
        add_embedded("demo.ping", _binary_demo_ping_bin_start, _binary_demo_ping_bin_end);
        add_embedded("demo.pong", _binary_demo_pong_bin_start, _binary_demo_pong_bin_end);
        add_embedded("dango.manager", _binary_dango_manager_bin_start, _binary_dango_manager_bin_end);
        add_embedded("dango.auth_probe", _binary_dango_auth_probe_bin_start,
                     _binary_dango_auth_probe_bin_end);
        add_embedded("manifest.v0", _binary_manifest_v0_bin_start, _binary_manifest_v0_bin_end);

        return g_entry_count > 0;
    }

    const u8* base = reinterpret_cast<const u8*>(initrd_base);
    if (!parse_tar_archive(base, initrd_size)) return false;

    // External QEMU -initrd. This is already a boot-time physical blob.
    g_source_tar_base = base;
    g_source_tar_size = initrd_size;
    g_source_tar_requires_copy = false;

    return true;
}

bool archive_materialize(PhysAddr* out_base, usize* out_size) {
    if (!out_base || !out_size || g_entry_count == 0) return false;

    if (g_source_tar_base && g_source_tar_size >= sizeof(TarHeader)) {
        if (!g_source_tar_requires_copy) {
            *out_base = reinterpret_cast<PhysAddr>(g_source_tar_base);
            *out_size = g_source_tar_size;
            return true;
        }

        return copy_source_tar_to_pages(out_base, out_size);
    }

    // Last-resort materialization for the tiny legacy embedded service archive.
    // Do not use this path for linux/Image-sized archives.
    usize total = 1024;
    for (u32 i = 0; i < g_entry_count; ++i) {
        total += 512 + align_up(g_entries[i].size, 512);
    }

    auto* blob = static_cast<u8*>(mm::heap_alloc(total, PAGE_SIZE));
    if (!blob) return false;

    libk::memset(blob, 0, total);

    usize off = 0;
    for (u32 i = 0; i < g_entry_count; ++i) {
        const TarEntry& entry = g_entries[i];

        auto* h = reinterpret_cast<TarHeader*>(blob + off);
        copy_cstr(h->name, sizeof(h->name), entry.name);
        write_octal(h->mode, sizeof(h->mode), 0644);
        write_octal(h->uid, sizeof(h->uid), 0);
        write_octal(h->gid, sizeof(h->gid), 0);
        write_octal(h->size, sizeof(h->size), entry.size);
        write_octal(h->mtime, sizeof(h->mtime), 0);

        for (char& c : h->chksum) c = ' ';
        h->typeflag = '0';
        copy_cstr(h->magic, sizeof(h->magic), "ustar");
        h->version[0] = '0';
        h->version[1] = '0';

        u32 sum = 0;
        const auto* bytes = reinterpret_cast<const u8*>(h);
        for (usize j = 0; j < sizeof(TarHeader); ++j) sum += bytes[j];

        write_octal(h->chksum, 7, sum);
        h->chksum[6] = '\0';
        h->chksum[7] = ' ';

        off += 512;
        libk::memcpy(blob + off, entry.data, entry.size);
        off += align_up(entry.size, 512);
    }

    *out_base = reinterpret_cast<PhysAddr>(blob);
    *out_size = total;
    return true;
}

bool archive_get(const char* name, const void** out_data, usize* out_size) {
    if (!name || !out_data || !out_size) return false;

    for (u32 i = 0; i < g_entry_count; ++i) {
        if (libk::strcmp(g_entries[i].name, name) == 0) {
            *out_data = g_entries[i].data;
            *out_size = g_entries[i].size;
            return true;
        }

        if (g_entries[i].name[0] == '.' &&
            g_entries[i].name[1] == '/' &&
            libk::strcmp(g_entries[i].name + 2, name) == 0) {
            *out_data = g_entries[i].data;
            *out_size = g_entries[i].size;
            return true;
        }
    }

    return false;
}

} // namespace kernel::init
