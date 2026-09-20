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
    if (header.flags != MX_IMAGE_FLAG_STATIC || header.checksum_algorithm != MX_CHECKSUM_CRC32C ||
        !is_zeroed(header.reserved, sizeof(header.reserved))) {
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
    candidate.entry = header.entry_va;
    candidate.count = header.segment_count;
    out = candidate;
    return nullptr;
}

} // namespace midix::format
