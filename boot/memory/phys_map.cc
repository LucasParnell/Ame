// boot/memory/phys_map.cpp
//
// Early physical memory map.
//
// Responsibilities:
//   - Receive raw regions from the DTB parser
//   - Carve out reserved ranges:
//       * The DTB blob itself
//       * The boot image (ame-boot.elf load address + size)
//       * Any /reserved-memory nodes from the DTB
//   - Expose a simple bump allocator for early page-table pages

#include "boot/memory.hpp"

namespace boot::memory {

void build_phys_map(BootInfo& /*info*/) {
    // TODO
}

void reserve(uint64_t /*base*/, uint64_t /*size*/) {
    // TODO
}

uint64_t early_alloc_pages(uint32_t /*count*/) {
    // TODO: return page-aligned physical address from bump region
    return 0;
}

} // namespace boot::memory
