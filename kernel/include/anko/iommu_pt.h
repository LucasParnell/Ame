#pragma once
// Pure descriptor data for the bounded three-level, 4 KiB radix walker.
// Other walk geometries require a separate walker; descriptor encodings and
// address limits belong to the backend, never to device services.
#include "anko/types.h"

namespace iommu {

struct IommuTableFormat {
    u64 address_mask;
    u64 valid_mask;
    u64 table_mask;
    u64 table_bits;
    u64 block_bits;
    u64 page_bits;
    u64 read_bits;
    u64 write_bits;
    u64 attributes;
};

// Arm VMSAv8-64 stage 2: AF set, Normal WB, Inner Shareable, execute never.
inline constexpr IommuTableFormat kArmStage2Tables{
    0x0000'FFFF'FFFF'F000ull, 1, 2, 3, 1, 3,
    1ull << 6, 1ull << 7, (0xFull << 2) | (3ull << 8) | (1ull << 10) | (1ull << 54),
};

} // namespace iommu
