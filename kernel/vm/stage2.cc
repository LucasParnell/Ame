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
    // range once, so mapped_pages is also the next free record slot. This keeps
    // VM RAM setup O(n) instead of O(n^2). One record is one mapped unit, whether
    // that unit is a 4 KiB page or a 2 MiB block.
    if (s2->mapped_pages < kMaxStage2Mappings) {
        Stage2Mapping& mapping = s2->mappings[s2->mapped_pages];
        if (!mapping.valid) {
            mapping.ipa = ipa;
            mapping.pa = pa;
            mapping.rights = rights;
            mapping.mem_type = mem_type;
            mapping.pages_log2 = pages_log2;
            mapping.valid = true;
            ++s2->mapped_pages;
            metric_inc(VM_DEBUG_STAGE2_PAGES_MAPPED);
            return E_OK;
        }
    }

    // Fallback for sparse maps after unmap or if mapped_pages no longer points
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
        ++s2->mapped_pages;
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
// table, and callers (page map, page unmap) depend on the old shape surviving a
// failure.
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

    const PhysAddr l3 = alloc_stage2_table(s2);
    if (!l3) return E_NOMEM;
    for (u32 i = 0; i < kStage2BlockPages; ++i) {
        table_ptr(l3)[i] = ((saved.pa + static_cast<PhysAddr>(i) * PAGE_SIZE) & S2_DESC_ADDR_MASK) |
                           S2_DESC_BLOCK_PAGE | stage2_page_attrs(saved.rights, saved.mem_type);
    }

    // Journal the page records; on overflow undo them and drop the new table.
    u32 written = 0;
    u32 slot_index = 0;
    while (written < kStage2BlockPages) {
        for (; slot_index < kMaxStage2Mappings; ++slot_index) {
            if (!s2->mappings[slot_index].valid) break;
        }
        if (slot_index >= kMaxStage2Mappings) {
            for (u32 i = 0; i < kMaxStage2Mappings && written; ++i) {
                Stage2Mapping& m = s2->mappings[i];
                if (m.valid && m.ipa >= block_base && m.ipa < block_base + kStage2BlockSize &&
                    m.pages_log2 == kStage2PageShift && m.ipa != saved.ipa) {
                    m.valid = false;
                    --written;
                    if (s2->mapped_pages > 0) --s2->mapped_pages;
                }
            }
            release_table(s2, l3);
            return E_OVERFLOW;
        }
        Stage2Mapping& m = s2->mappings[slot_index];
        m.ipa = block_base + static_cast<u64>(written) * PAGE_SIZE;
        m.pa = saved.pa + static_cast<PhysAddr>(written) * PAGE_SIZE;
        m.rights = saved.rights;
        m.mem_type = saved.mem_type;
        m.pages_log2 = kStage2PageShift;
        m.valid = true;
        ++s2->mapped_pages;
        ++written;
        ++slot_index;
    }

    block->valid = false;  // the block record is superseded by its 512 pages
    if (s2->mapped_pages > 0) --s2->mapped_pages;
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
    if (s2->mapped_pages >= kMaxStage2Mappings) return E_OVERFLOW;

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
    if (s2->mapped_pages >= kMaxStage2Mappings) return E_OVERFLOW;
    PhysAddr table_pa = s2->root_table_phys;
    for (u32 level = 1; level < 3; ++level) {
        auto& link = table_ptr(table_pa)[level_index(ipa, level)];
        if (!is_table_desc(link)) {
            if (link & S2_DESC_VALID) {
                // A 2 MiB block covers this page (level 2 only). Split it so a
                // page-granular mapping can coexist with block-mapped RAM.
                if (level != 2) { prune_tables(s2, ipa); return E_BUSY; }
                const SysError rc = split_block(s2, ipa & ~(kStage2BlockSize - 1));
                if (rc != E_OK) return rc;
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

static SysError unmap_page(Stage2AddressSpace* s2, u64 ipa, bool revoke_device) {
    if (!s2 || !s2->built || (s2->active && !revoke_device)) return E_INVALID;
    if ((ipa & (PAGE_SIZE - 1)) != 0 || ipa >= (1ull << s2->ipa_bits)) return E_INVALID;

    if (revoke_device) {
        Stage2MemType type{};
        if (!stage2_lookup_page(s2, ipa, nullptr, &type)) return E_NOTFOUND;
        if (type != Stage2MemType::Device) return E_PERM;
    }

    const u64 l1_desc = table_ptr(s2->root_table_phys)[level_index(ipa, 1)];
    if (is_table_desc(l1_desc)) {
        const PhysAddr l2_pa = l1_desc & S2_DESC_ADDR_MASK;
        const u64 l2_desc = table_ptr(l2_pa)[level_index(ipa, 2)];
        if (is_table_desc(l2_desc)) {
            const PhysAddr l3_pa = l2_desc & S2_DESC_ADDR_MASK;
            table_ptr(l3_pa)[level_index(ipa, 3)] = 0;
        } else if ((l2_desc & S2_DESC_VALID) != 0) {
            // A block covers this page: split so only this page is removed and
            // the rest of the 2 MiB unit keeps its mapping.
            const SysError rc = split_block(s2, ipa & ~(kStage2BlockSize - 1));
            if (rc != E_OK) return rc;
            const PhysAddr l3_pa = table_ptr(l2_pa)[level_index(ipa, 2)] & S2_DESC_ADDR_MASK;
            table_ptr(l3_pa)[level_index(ipa, 3)] = 0;
        }
    }

    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        Stage2Mapping& mapping = s2->mappings[i];
        if (!mapping.valid || mapping.ipa != ipa) continue;
        mapping.valid = false;
        if (s2->mapped_pages > 0) --s2->mapped_pages;
        prune_tables(s2, ipa);
        return E_OK;
    }
    return E_NOTFOUND;
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
