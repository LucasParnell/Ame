#include "anko/vm.h"
#include "libk/string.h"

namespace vm {

namespace {

constexpr u64 S2_DESC_VALID = 1ull << 0;
constexpr u64 S2_DESC_TABLE = 1ull << 1;
constexpr u64 S2_DESC_AF = 1ull << 10;
constexpr u64 S2_DESC_SH_INNER = 3ull << 8;
constexpr u64 S2_DESC_S2AP_READ = 1ull << 6;
constexpr u64 S2_DESC_S2AP_WRITE = 1ull << 7;
constexpr u64 S2_DESC_MEM_DEVICE_NGNRE = 0x1ull << 2;
constexpr u64 S2_DESC_MEM_NORMAL_NC = 0x3ull << 2;
constexpr u64 S2_DESC_MEM_NORMAL = 0xFull << 2;
constexpr u64 S2_DESC_XN = 1ull << 54;
constexpr u64 S2_DESC_ADDR_MASK = 0x0000'FFFF'FFFF'F000ull;
constexpr u64 S2_DESC_BLOCK_PAGE = S2_DESC_VALID | S2_DESC_TABLE;
// A level-1/2 block descriptor: VALID without TABLE. Level 1 would be a 1 GiB
// block (unused); level 2 is the 2 MiB block this kernel installs.
constexpr u64 S2_DESC_BLOCK = S2_DESC_VALID;

bool range_overlaps(u64 a_base, u64 a_size, u64 b_base, u64 b_size) {
    u64 a_end = 0;
    u64 b_end = 0;
    if (__builtin_add_overflow(a_base, a_size, &a_end)) return true;
    if (__builtin_add_overflow(b_base, b_size, &b_end)) return true;
    return a_base < b_end && a_end > b_base;
}

u64* table_ptr(PhysAddr pa) {
    return reinterpret_cast<u64*>(pa);
}

u32 level_index(u64 ipa, u32 level) {
    const u32 shift = (level == 1) ? 30 : (level == 2 ? 21 : 12);
    return static_cast<u32>((ipa >> shift) & 0x1FFu);
}

bool is_table_desc(u64 desc) {
    return (desc & (S2_DESC_VALID | S2_DESC_TABLE)) == (S2_DESC_VALID | S2_DESC_TABLE);
}

PhysAddr alloc_stage2_table(Stage2AddressSpace* s2) {
    if (!s2 || s2->next_level_table_count >=
                   (sizeof(s2->next_level_tables) / sizeof(s2->next_level_tables[0]))) {
        return 0;
    }
    PhysAddr pa = mm::alloc_page();
    if (!pa) return 0;
    libk::memset(reinterpret_cast<void*>(pa), 0, PAGE_SIZE);
    s2->next_level_tables[s2->next_level_table_count++] = pa;
    metric_inc(VM_DEBUG_STAGE2_TABLES_CREATED);
    return pa;
}

bool table_empty(PhysAddr pa) {
    for (u32 i = 0; i < 512; ++i) if (table_ptr(pa)[i] & S2_DESC_VALID) return false;
    return true;
}

void release_table(Stage2AddressSpace* s2, PhysAddr pa) {
    for (u32 i = 0; i < s2->next_level_table_count; ++i) {
        if (s2->next_level_tables[i] != pa) continue;
        s2->next_level_tables[i] = s2->next_level_tables[--s2->next_level_table_count];
        s2->next_level_tables[s2->next_level_table_count] = 0;
        mm::free_page(pa);
        return;
    }
}

// No guest can run during this operation. Guest entry invalidates stage-2 TLBs.
void prune_tables(Stage2AddressSpace* s2, u64 ipa) {
    auto& root = table_ptr(s2->root_table_phys)[level_index(ipa, 1)];
    if (!is_table_desc(root)) return;
    PhysAddr l2 = root & S2_DESC_ADDR_MASK;
    auto& link = table_ptr(l2)[level_index(ipa, 2)];
    if (is_table_desc(link)) {
        PhysAddr l3 = link & S2_DESC_ADDR_MASK;
        if (table_empty(l3)) { link = 0; release_table(s2, l3); }
    }
    if (table_empty(l2)) { root = 0; release_table(s2, l2); }
}

bool mem_type_valid(Stage2MemType mem_type) {
    switch (mem_type) {
    case Stage2MemType::NormalWB:
    case Stage2MemType::NormalNC:
    case Stage2MemType::Device:
        return true;
    }
    return false;
}

u64 stage2_page_attrs(u32 rights, Stage2MemType mem_type) {
    /*
     * Stage-2 S2AP is independent from host capability rights.  Preserve the
     * exact guest access requested by the VM monitor rather than silently
     * upgrading every mapping to RWX.  Bits [7:6] are read/write permission in
     * the stage-2 long-descriptor format; XN blocks instruction fetches.
     *
     * MemAttr[3:0] lives in descriptor bits [5:2]:
     *   0b1111 Normal, inner/outer write-back (guest RAM)
     *   0b0011 Normal, inner/outer non-cacheable (shared/DMA buffers)
     *   0b0001 Device-nGnRE (MMIO), which is always execute-never.
     */
    u64 attrs = S2_DESC_SH_INNER | S2_DESC_AF;
    switch (mem_type) {
    case Stage2MemType::NormalWB:
        attrs |= S2_DESC_MEM_NORMAL;
        break;
    case Stage2MemType::NormalNC:
        attrs |= S2_DESC_MEM_NORMAL_NC;
        break;
    case Stage2MemType::Device:
        attrs |= S2_DESC_MEM_DEVICE_NGNRE;
        break;
    }
    if (rights & RIGHT_READ) attrs |= S2_DESC_S2AP_READ;
    if (rights & RIGHT_WRITE) attrs |= S2_DESC_S2AP_WRITE;
    if (mem_type == Stage2MemType::Device || (rights & RIGHT_EXEC) == 0) {
        attrs |= S2_DESC_XN;
    }
    return attrs;
}

SysError record_mapping(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights,
                        Stage2MemType mem_type, u8 pages_log2) {
    if (!s2) return E_INVALID;

    // Fast append path. The common VM-RAM mapping path maps a contiguous IPA
    // range once, so mapped_records is also the next free record slot. This keeps
    // VM RAM setup O(n) instead of O(n^2). One record is one mapped unit, whether
    // that unit is a 4 KiB page or a 2 MiB block.
    if (s2->mapped_records < kMaxStage2Mappings) {
        Stage2Mapping& mapping = s2->mappings[s2->mapped_records];
        if (!mapping.valid) {
            mapping.ipa = ipa;
            mapping.pa = pa;
            mapping.rights = rights;
            mapping.mem_type = mem_type;
            mapping.pages_log2 = pages_log2;
            mapping.valid = true;
            ++s2->mapped_records;
            s2->mapped_pages += (1ull << (pages_log2 - kStage2PageShift));
            metric_inc(VM_DEBUG_STAGE2_PAGES_MAPPED);
            return E_OK;
        }
    }

    // Fallback for sparse maps after unmap or if mapped_records no longer points
    // at a free slot.
    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        Stage2Mapping& mapping = s2->mappings[i];
        if (mapping.valid) continue;
        mapping.ipa = ipa;
        mapping.pa = pa;
        mapping.rights = rights;
        mapping.mem_type = mem_type;
        mapping.pages_log2 = pages_log2;
        mapping.valid = true;
        ++s2->mapped_records;
        s2->mapped_pages += (1ull << (pages_log2 - kStage2PageShift));
        metric_inc(VM_DEBUG_STAGE2_PAGES_MAPPED);
        return E_OK;
    }

    return E_OVERFLOW;
}

// Find the record describing exactly this base address, or nullptr.
Stage2Mapping* find_mapping(Stage2AddressSpace* s2, u64 base) {
    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        Stage2Mapping& mapping = s2->mappings[i];
        if (mapping.valid && mapping.ipa == base) return &mapping;
    }
    return nullptr;
}

// Replace a 2 MiB block with 512 page entries, keeping the block intact if the
// record budget cannot hold the pages: a split must never leave a half-published
// table. Partial page removal depends on the old shape surviving failure.
SysError split_block(Stage2AddressSpace* s2, u64 block_base) {
    Stage2Mapping* block = find_mapping(s2, block_base);
    if (!block || block->pages_log2 != kStage2BlockShift) return E_NOTFOUND;
    const Stage2Mapping saved = *block;

    const PhysAddr l1_pa = s2->root_table_phys;
    const u64 l1_desc = table_ptr(l1_pa)[level_index(block_base, 1)];
    if (!is_table_desc(l1_desc)) return E_INVALID;
    const PhysAddr l2_pa = l1_desc & S2_DESC_ADDR_MASK;
    auto& slot = table_ptr(l2_pa)[level_index(block_base, 2)];
    if (is_table_desc(slot)) return E_INVALID;

    // Reuse the block record; only 511 additional slots are needed. All
    // failure checks precede both metadata and descriptor mutation.
    if (!stage2_has_capacity(s2, kStage2BlockPages - 1)) {
        return E_OVERFLOW;
    }
    const PhysAddr l3 = alloc_stage2_table(s2);
    if (!l3) return E_NOMEM;
    for (u32 i = 0; i < kStage2BlockPages; ++i) {
        table_ptr(l3)[i] = ((saved.pa + static_cast<PhysAddr>(i) * PAGE_SIZE) & S2_DESC_ADDR_MASK) |
                           S2_DESC_BLOCK_PAGE | stage2_page_attrs(saved.rights, saved.mem_type);
    }
    u32 page = 1;
    for (auto& mapping : s2->mappings) {
        if (mapping.valid) {
            continue;
        }
        mapping = saved;
        mapping.ipa += static_cast<u64>(page) * PAGE_SIZE;
        mapping.pa += static_cast<PhysAddr>(page) * PAGE_SIZE;
        mapping.pages_log2 = kStage2PageShift;
        if (++page == kStage2BlockPages) {
            break;
        }
    }
    block->pages_log2 = kStage2PageShift;
    s2->mapped_records += kStage2BlockPages - 1;
    // Coverage is unchanged by a split; only the number of records grows.
    slot = l3 | S2_DESC_VALID | S2_DESC_TABLE;
    return E_OK;
}

} // namespace

SysError stage2_create(Stage2AddressSpace* s2) {
    if (!s2) return E_INVALID;
    libk::memset(s2, 0, sizeof(*s2));

    const PhysAddr root = mm::alloc_page();
    if (!root) return E_NOMEM;
    libk::memset(reinterpret_cast<void*>(root), 0, PAGE_SIZE);

    s2->root_table_phys = root;
    s2->root_table_va = static_cast<VirtAddr>(root);
    s2->ipa_bits = kVmIpaBits;
    s2->page_size = PAGE_SIZE;
    s2->mapped_pages = 0;
    s2->mapped_records = 0;
    s2->built = true;
    s2->active = false;
    metric_inc(VM_DEBUG_STAGE2_TABLES_CREATED);
    return E_OK;
}

SysError stage2_validate_no_overlap(Stage2AddressSpace* s2, u64 ipa, u64 size) {
    if (!s2 || !s2->built || s2->active) return E_INVALID;
    if (size == 0) return E_INVALID;
    if ((ipa & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0) return E_INVALID;
    if (ipa >= (1ull << s2->ipa_bits) || size > (1ull << s2->ipa_bits) - ipa) return E_INVALID;
    for (const auto& trap : s2->traps)
        if (trap.size && range_overlaps(ipa, size, trap.base, trap.size)) return E_BUSY;

    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        const Stage2Mapping& mapping = s2->mappings[i];
        if (!mapping.valid) continue;
        if (range_overlaps(ipa, size, mapping.ipa, 1ull << mapping.pages_log2)) return E_BUSY;
    }
    return E_OK;
}

SysError stage2_reserve_trap(Stage2AddressSpace* s2, u64 ipa, u64 size) {
    const auto rc = stage2_validate_no_overlap(s2, ipa, size);
    if (rc != E_OK) return rc;
    for (auto& trap : s2->traps) {
        if (!trap.size) { trap = {ipa, size}; return E_OK; }
    }
    return E_OVERFLOW;
}

void stage2_release_trap(Stage2AddressSpace* s2, u64 ipa, u64 size) {
    if (!s2 || s2->active) return;
    for (auto& trap : s2->traps)
        if (trap.base == ipa && trap.size == size) { trap = {}; return; }
}

SysError stage2_map_page(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights) {
    return stage2_map_page_ex(s2, ipa, pa, rights, Stage2MemType::NormalWB);
}

SysError stage2_map_block(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights,
                          Stage2MemType mem_type) {
    if (!mem_type_valid(mem_type)) {
        return E_INVALID;
    }
    if (!s2 || !s2->built || s2->active) return E_INVALID;
    if ((ipa & (kStage2BlockSize - 1)) != 0 || (pa & (kStage2BlockSize - 1)) != 0) return E_INVALID;
    if ((rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC)) != 0 || !rights) return E_INVALID;
    if (ipa >= (1ull << s2->ipa_bits) || kStage2BlockSize > (1ull << s2->ipa_bits) - ipa) {
        return E_INVALID;
    }
    if ((pa & ~S2_DESC_ADDR_MASK) != 0) return E_INVALID;
    for (const auto& trap : s2->traps) {
        if (trap.size && range_overlaps(ipa, kStage2BlockSize, trap.base, trap.size)) return E_BUSY;
    }
    if (!stage2_has_capacity(s2, 1)) {
        return E_OVERFLOW;
    }

    const u64 l1_desc = table_ptr(s2->root_table_phys)[level_index(ipa, 1)];
    PhysAddr l2_pa = 0;
    if (is_table_desc(l1_desc)) {
        l2_pa = l1_desc & S2_DESC_ADDR_MASK;
    } else {
        if ((l1_desc & S2_DESC_VALID) != 0) return E_BUSY;
        l2_pa = alloc_stage2_table(s2);
        if (!l2_pa) return E_NOMEM;
        table_ptr(s2->root_table_phys)[level_index(ipa, 1)] =
            l2_pa | S2_DESC_VALID | S2_DESC_TABLE;
    }
    auto& slot = table_ptr(l2_pa)[level_index(ipa, 2)];
    if (slot & S2_DESC_VALID) return E_BUSY;  // a table or block already covers it
    const SysError rc = record_mapping(s2, ipa, pa, rights, mem_type, kStage2BlockShift);
    if (rc != E_OK) {
        prune_tables(s2, ipa);
        return rc;
    }
    slot = (pa & S2_DESC_ADDR_MASK) | S2_DESC_BLOCK | stage2_page_attrs(rights, mem_type);
    return E_OK;
}

SysError stage2_map_page_ex(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights,
                            Stage2MemType mem_type) {
    if (!mem_type_valid(mem_type)) {
        return E_INVALID;
    }
    if (!s2 || !s2->built || s2->active) return E_INVALID;
    if ((ipa & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return E_INVALID;
    if ((rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC)) != 0) return E_INVALID;

    if (ipa >= (1ull << s2->ipa_bits) || (pa & ~S2_DESC_ADDR_MASK) || !rights) return E_INVALID;
    for (const auto& trap : s2->traps)
        if (trap.size && range_overlaps(ipa, PAGE_SIZE, trap.base, trap.size)) return E_BUSY;
    if (!stage2_has_capacity(s2, 1)) {
        return E_OVERFLOW;
    }
    PhysAddr table_pa = s2->root_table_phys;
    for (u32 level = 1; level < 3; ++level) {
        auto& link = table_ptr(table_pa)[level_index(ipa, level)];
        if (!is_table_desc(link)) {
            if (link & S2_DESC_VALID) {
                // Mapping is never replacement. Reject overlap without
                // allocating or fragmenting the existing block.
                return E_BUSY;
            } else {
                PhysAddr child = alloc_stage2_table(s2);
                if (!child) { prune_tables(s2, ipa); return E_NOMEM; }
                link = child | S2_DESC_VALID | S2_DESC_TABLE;
            }
        }
        table_pa = link & S2_DESC_ADDR_MASK;
    }
    auto& leaf = table_ptr(table_pa)[level_index(ipa, 3)];
    if (leaf & S2_DESC_VALID) return E_BUSY;
    const SysError rc = record_mapping(s2, ipa, pa, rights, mem_type, kStage2PageShift);
    if (rc != E_OK) { prune_tables(s2, ipa); return rc; }
    leaf = (pa & S2_DESC_ADDR_MASK) | S2_DESC_BLOCK_PAGE | stage2_page_attrs(rights, mem_type);
    return E_OK;
}

// Range removal never splits or allocates. Teardown must work even when
// mapping records and physical memory are exhausted. CallGuard excludes guest
// entry; EL2 entry invalidates the selected VMID before execution resumes.
static SysError unmap_range(Stage2AddressSpace* s2, u64 ipa, u64 size, bool device_only) {
    if (!s2 || !s2->built || (s2->active && !device_only) || !size || ipa % PAGE_SIZE ||
        size % PAGE_SIZE || ipa >= (1ull << s2->ipa_bits) || size > (1ull << s2->ipa_bits) - ipa) {
        return E_INVALID;
    }
    const u64 end = ipa + size;
    // Validate the whole request before mutation, including mixed Device/RAM
    // requests. Revocation must never remove ordinary RAM.
    for (const auto& mapping : s2->mappings) {
        if (!mapping.valid) {
            continue;
        }
        const u64 unit = 1ull << mapping.pages_log2;
        if (!range_overlaps(ipa, size, mapping.ipa, unit)) {
            continue;
        }
        if (device_only && mapping.mem_type != Stage2MemType::Device) {
            return E_PERM;
        }
        if (mapping.ipa < ipa || unit > end - mapping.ipa) {
            return E_INVALID;
        }
    }
    for (auto& mapping : s2->mappings) {
        if (!mapping.valid || mapping.ipa < ipa || mapping.ipa >= end) {
            continue;
        }
        const u64 root = table_ptr(s2->root_table_phys)[level_index(mapping.ipa, 1)];
        const PhysAddr l2 = root & S2_DESC_ADDR_MASK;
        auto& link = table_ptr(l2)[level_index(mapping.ipa, 2)];
        if (mapping.pages_log2 == kStage2BlockShift) {
            link = 0;
        } else {
            table_ptr(link & S2_DESC_ADDR_MASK)[level_index(mapping.ipa, 3)] = 0;
        }
        mapping.valid = false;
        --s2->mapped_records;
        s2->mapped_pages -= 1ull << (mapping.pages_log2 - kStage2PageShift);
        prune_tables(s2, mapping.ipa);
    }
    return E_OK;
}

SysError stage2_unmap_range(Stage2AddressSpace* s2, u64 ipa, u64 size) {
    return unmap_range(s2, ipa, size, false);
}

SysError stage2_revoke_device_range(Stage2AddressSpace* s2, u64 ipa, u64 size) {
    return unmap_range(s2, ipa, size, true);
}

static SysError unmap_page(Stage2AddressSpace* s2, u64 ipa, bool revoke_device) {
    if (!s2 || !s2->built || (s2->active && !revoke_device)) {
        return E_INVALID;
    }
    if (ipa % PAGE_SIZE || ipa >= (1ull << s2->ipa_bits)) {
        return E_INVALID;
    }
    Stage2Mapping* mapping = find_mapping(s2, ipa);
    if (!mapping) {
        mapping = find_mapping(s2, ipa & ~(kStage2BlockSize - 1));
        if (!mapping || mapping->pages_log2 != kStage2BlockShift) {
            return E_NOTFOUND;
        }
    }
    if (revoke_device && mapping->mem_type != Stage2MemType::Device) {
        return E_PERM;
    }
    if (mapping->pages_log2 == kStage2BlockShift) {
        const SysError rc = split_block(s2, mapping->ipa);
        if (rc != E_OK) {
            return rc;
        }
    }
    return unmap_range(s2, ipa, PAGE_SIZE, revoke_device);
}

SysError stage2_unmap_page(Stage2AddressSpace* s2, u64 ipa) {
    return unmap_page(s2, ipa, false);
}

SysError stage2_revoke_device_page(Stage2AddressSpace* s2, u64 ipa) {
    // CallGuard excludes all guest execution. EL2 entry invalidates translations
    // for the selected VMID on every CPU before any guest instruction resumes.
    return unmap_page(s2, ipa, true);
}

SysError stage2_destroy(Stage2AddressSpace* s2) {
    if (!s2) return E_INVALID;
    for (u32 i = 0; i < s2->next_level_table_count; ++i) {
        if (s2->next_level_tables[i]) {
            mm::free_page(s2->next_level_tables[i]);
        }
    }
    if (s2->root_table_phys) {
        mm::free_page(s2->root_table_phys);
    }
    libk::memset(s2, 0, sizeof(*s2));
    return E_OK;
}

SysError stage2_activate(Stage2AddressSpace* s2) {
    if (!s2 || !s2->built || !s2->root_table_phys) return E_INVALID;
    s2->active = true;
    return E_OK;
}

PhysAddr stage2_lookup_page(Stage2AddressSpace* s2, u64 ipa, u32* out_rights,
                            Stage2MemType* out_mem_type) {
    if (!s2 || !s2->built) return 0;
    const u64 page_ipa = ipa & ~(static_cast<u64>(PAGE_SIZE - 1));
    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        const Stage2Mapping& mapping = s2->mappings[i];
        if (!mapping.valid) continue;
        // A block record describes a whole 2 MiB unit, so a lookup must accept
        // any page inside it, not only the block's base address.
        const u64 unit = 1ull << mapping.pages_log2;
        if (page_ipa < mapping.ipa || page_ipa >= mapping.ipa + unit) continue;
        if (out_rights) *out_rights = mapping.rights;
        if (out_mem_type) {
            *out_mem_type = mapping.mem_type;
        }
        return mapping.pa + static_cast<PhysAddr>(page_ipa - mapping.ipa);
    }
    return 0;
}

} // namespace vm
