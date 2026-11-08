#include "midix/format/image.h"
#include "midix/format/integrity.h"
#include "midix/abi/cap.h"
#include "libk/string.h"

namespace midix::format {
namespace {

constexpr mx_u64_t kPageSize = 4096;
constexpr mx_u64_t kUserImageStart = 0x10010000; // leaves the BootInfo page reserved
constexpr mx_u64_t kUserImageEnd = 0x40000000;
constexpr mx_u64_t kMaxImageSize = 16 * 1024 * 1024;
constexpr mx_u64_t kMaxImagePages = 4096;
constexpr mx_size_t kImageChecksumOffset = __builtin_offsetof(MxImageHeader, image_crc32c);

bool contains_range(mx_u64_t offset, mx_u64_t length, mx_u64_t available) {
    return offset <= available && length <= available - offset;
}

bool is_zeroed(const mx_u8_t* bytes, mx_size_t size) {
    for (mx_size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

const char* validate_image_header(const MxImageHeader& header, const mx_u8_t* data,
                                  mx_size_t size) {
    if (header.magic != MX_IMAGE_MAGIC || header.version != MX_IMAGE_VERSION ||
        header.header_size != sizeof(header) || header.kind != MX_IMAGE_KIND_EXEC ||
        header.arch != MX_IMAGE_ARCH_AARCH64 || header.abi_version != MX_IMAGE_ABI_VERSION) {
        return "unsupported image format or ABI";
    }
    if ((header.flags != MX_IMAGE_FLAG_STATIC &&
         header.flags != (MX_IMAGE_FLAG_STATIC | MX_IMAGE_FLAG_ARENA) &&
         header.flags != (MX_IMAGE_FLAG_STATIC | MX_IMAGE_FLAG_ARENA | MX_IMAGE_FLAG_ASLR)) ||
        header.checksum_algorithm != MX_CHECKSUM_CRC32C ||
        (!(header.flags & MX_IMAGE_FLAG_ARENA) && !is_zeroed(header.reserved, sizeof(header.reserved)))) {
        return "unsupported image flags or checksum";
    }
    if (header.image_size != size || size > kMaxImageSize || header.segment_count == 0 ||
        header.segment_count > kMaxSegments || header.segment_table_offset != sizeof(header) ||
        !contains_range(header.segment_table_offset, header.segment_count * sizeof(MxImageSegment),
                        size)) {
        return "invalid image size or segment table";
    }

    // Static MXI executables have no dynamic imports, exports or requirements.
    if (header.requirement_count || header.requirement_table_offset || header.import_count ||
        header.export_count || header.import_table_offset || header.export_table_offset ||
        header.string_table_offset || header.string_table_size) {
        return "unsupported image tables";
    }
    if (header.stack_size != kPageSize) {
        return "unsupported image stack size";
    }

    const mx_u32_t checksum =
        integrity::crc32c_zero_field(data, size, kImageChecksumOffset, sizeof(header.image_crc32c));
    if (checksum != header.image_crc32c) {
        return "image checksum mismatch";
    }
    return nullptr;
}

const char* validate_segment(const MxImageSegment& segment, mx_size_t image_size,
                             mx_u64_t previous_file_end) {
    if (segment.memory_size == 0 || segment.file_size > segment.memory_size ||
        !contains_range(segment.file_offset, segment.file_size, image_size) ||
        segment.file_offset < previous_file_end) {
        return "invalid segment file range";
    }
    if (segment.virtual_address < kUserImageStart || segment.virtual_address >= kUserImageEnd ||
        segment.memory_size > kUserImageEnd - segment.virtual_address) {
        return "segment overlaps reserved address space";
    }
    if (segment.alignment != kPageSize || segment.file_offset % kPageSize != 0 ||
        segment.virtual_address % kPageSize != 0) {
        return "unsupported segment alignment";
    }

    const mx_rights_t supported_rights = MX_RIGHT_READ | MX_RIGHT_WRITE | MX_RIGHT_EXEC;
    const bool writable = (segment.rights & MX_RIGHT_WRITE) != 0;
    const bool executable = (segment.rights & MX_RIGHT_EXEC) != 0;
    if (!(segment.rights & MX_RIGHT_READ) || (segment.rights & ~supported_rights) != 0 ||
        (writable && executable)) {
        return "invalid segment permissions";
    }
    if ((segment.flags & ~MX_IMAGE_SEG_ZERO_FILL) != 0 || segment.reserved0 != 0 ||
        !is_zeroed(segment.reserved, sizeof(segment.reserved))) {
        return "unsupported segment flags";
    }
    return nullptr;
}

SegmentPlan plan_segment(const MxImageSegment& segment) {
    SegmentPlan plan{};
    plan.va_start = segment.virtual_address;
    plan.map_size = (segment.memory_size + kPageSize - 1) & ~(kPageSize - 1);
    plan.file_offset = segment.file_offset;
    plan.file_size = segment.file_size;
    plan.rights = segment.rights;
    return plan;
}

bool overlaps_previous_segment(const ImagePlan& image, mx_u32_t index) {
    const auto& candidate = image.segments[index];
    for (mx_u32_t previous = 0; previous < index; ++previous) {
        const auto& existing = image.segments[previous];
        if (existing.va_start < candidate.va_start + candidate.map_size &&
            candidate.va_start < existing.va_start + existing.map_size) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* parse_image(const mx_u8_t* data, mx_size_t size, ImagePlan& out) {
    out = {};
    if (!data || size < sizeof(MxImageHeader)) {
        return "truncated image";
    }

    MxImageHeader header;
    libk::memcpy(&header, data, sizeof(header));
    if (const char* error = validate_image_header(header, data, size)) {
        return error;
    }

    ImagePlan candidate{};
    mx_u64_t previous_file_end =
        header.segment_table_offset + header.segment_count * sizeof(MxImageSegment);
    mx_u64_t mapped_pages = 0;
    if (header.flags & MX_IMAGE_FLAG_ARENA) {
        mx_u64_t offset;
        libk::memcpy(&offset, header.reserved, sizeof(offset));
        if (offset != previous_file_end || !contains_range(offset, sizeof(MxArenaCreate), size))
            return "invalid arena declaration location";
        libk::memcpy(&candidate.arena, data + offset, sizeof(candidate.arena));
        const auto& a = candidate.arena;
        if (!a.size || a.size > a.capacity || a.capacity > a.cage_size ||
            a.cage_size > MX_PROCESS_ARENA_LIMIT || a.cage_base < kUserImageStart ||
            a.cage_base >= kUserImageEnd || a.cage_size > kUserImageEnd - a.cage_base ||
            ((a.size | a.capacity | a.cage_base | a.cage_size) & (kPageSize - 1)) ||
            !a.type_id || a.reserved || (a.policy & ~MX_ARENA_COMPACT_IDLE))
            return "invalid arena declaration";
        previous_file_end += sizeof(MxArenaCreate);
    }
    if (header.flags & MX_IMAGE_FLAG_ASLR) {
        if (!contains_range(previous_file_end, sizeof(MxImageLayout), size)) return "truncated image layout";
        libk::memcpy(&candidate.layout, data + previous_file_end, sizeof(candidate.layout));
        previous_file_end += sizeof(candidate.layout);
        if (candidate.layout.version != 1 || candidate.layout.count > 16384 ||
            candidate.layout.minimum < kUserImageStart || candidate.layout.maximum > kUserImageEnd ||
            candidate.layout.maximum <= candidate.layout.minimum || (candidate.layout.minimum & 4095) ||
            !contains_range(previous_file_end, candidate.layout.count * sizeof(MxImageRelocation), size))
            return "invalid image layout";
        candidate.relocation_offset = previous_file_end; candidate.aslr = true;
        previous_file_end += candidate.layout.count * sizeof(MxImageRelocation);
    }
    bool entry_is_executable = false;

    for (mx_u32_t index = 0; index < header.segment_count; ++index) {
        MxImageSegment segment;
        const mx_u64_t offset = header.segment_table_offset + index * sizeof(segment);
        libk::memcpy(&segment, data + offset, sizeof(segment));
        if (const char* error = validate_segment(segment, size, previous_file_end)) {
            return error;
        }
        previous_file_end = segment.file_offset + segment.file_size;
        candidate.segments[index] = plan_segment(segment);
        if (candidate.arena.cage_base < candidate.segments[index].va_start + candidate.segments[index].map_size &&
            candidate.segments[index].va_start < candidate.arena.cage_base + candidate.arena.cage_size)
            return "image overlaps its declared arena";

        mapped_pages += candidate.segments[index].map_size / kPageSize;
        if (mapped_pages > kMaxImagePages) {
            return "image page capacity exceeded";
        }
        if (overlaps_previous_segment(candidate, index)) {
            return "overlapping image segments";
        }
        if ((segment.rights & MX_RIGHT_EXEC) && header.entry_va >= segment.virtual_address &&
            header.entry_va < segment.virtual_address + segment.file_size) {
            entry_is_executable = true;
        }
    }

    if (!entry_is_executable || header.entry_va % 4 != 0) {
        return "invalid image entry";
    }
    if (candidate.aslr) {
        mx_u64_t previous = 0;
        for (mx_u32_t i = 0; i < candidate.layout.count; ++i) {
            MxImageRelocation r;
            libk::memcpy(&r, data + candidate.relocation_offset + i * sizeof(r), sizeof(r));
            if ((r.width != 4 && r.width != 8) || r.reserved || (r.address & (r.width - 1)) ||
                (i && r.address < previous) || r.value < candidate.layout.minimum || r.value > candidate.layout.maximum)
                return "invalid image relocation";
            bool target = false;
            for (mx_u32_t j = 0; j < header.segment_count; ++j) {
                const auto& segment = candidate.segments[j];
                if (r.address >= segment.va_start && r.address - segment.va_start <= segment.file_size &&
                    r.width <= segment.file_size - (r.address - segment.va_start)) {
                    mx_u64_t value = 0;
                    libk::memcpy(&value, data + segment.file_offset + r.address - segment.va_start, r.width);
                    if (value != r.value) return "relocation value mismatch";
                    target = true;
                }
            }
            if (!target) return "relocation target outside file segments";
            previous = r.address + r.width;
        }
        for (mx_u32_t j = 0; j < header.segment_count; ++j) {
            const auto& segment = candidate.segments[j];
            if (segment.va_start < candidate.layout.minimum ||
                segment.va_start + segment.map_size > ((candidate.layout.maximum + 4095) & ~mx_u64_t{4095}))
                return "segment outside image layout";
        }
    }
    candidate.entry = header.entry_va;
    candidate.count = header.segment_count;
    out = candidate;
    return nullptr;
}

} // namespace midix::format
