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
constexpr u64 S2_DESC_MEM_NORMAL = 0xFull << 2;
constexpr u64 S2_DESC_XN = 1ull << 54;
constexpr u64 S2_DESC_ADDR_MASK = 0x0000'FFFF'FFFF'F000ull;
constexpr u64 S2_DESC_BLOCK_PAGE = S2_DESC_VALID | S2_DESC_TABLE;

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

bool ensure_table(Stage2AddressSpace* s2, PhysAddr table_pa, u32 idx, PhysAddr* out_pa) {
    u64* table = table_ptr(table_pa);
    const u64 desc = table[idx];
    if (is_table_desc(desc)) {
        *out_pa = desc & S2_DESC_ADDR_MASK;
        return true;
    }
    if (desc & S2_DESC_VALID) return false;

    PhysAddr child = alloc_stage2_table(s2);
    if (!child) return false;
    table[idx] = (child & S2_DESC_ADDR_MASK) | S2_DESC_VALID | S2_DESC_TABLE;
    *out_pa = child;
    return true;
}

u64 stage2_page_attrs(u32 rights) {
    /*
     * Stage-2 S2AP is independent from host capability rights.  Preserve the
     * exact guest access requested by the VM monitor rather than silently
     * upgrading every mapping to RWX.  Bits [7:6] are read/write permission in
     * the stage-2 long-descriptor format; XN blocks instruction fetches.
     */
    u64 attrs = S2_DESC_MEM_NORMAL | S2_DESC_SH_INNER | S2_DESC_AF;
    if (rights & RIGHT_READ) attrs |= S2_DESC_S2AP_READ;
    if (rights & RIGHT_WRITE) attrs |= S2_DESC_S2AP_WRITE;
    if ((rights & RIGHT_EXEC) == 0) attrs |= S2_DESC_XN;
    return attrs;
}

SysError record_mapping(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights) {
    if (!s2) return E_INVALID;

    // Fast append path. The common VM-RAM mapping path maps a contiguous IPA
    // range once, so mapped_pages is also the next free record slot. This keeps
    // 128 MiB VM RAM setup O(n) instead of O(n^2).
    if (s2->mapped_pages < kMaxStage2Mappings) {
        Stage2Mapping& mapping = s2->mappings[s2->mapped_pages];
        if (!mapping.valid) {
            mapping.ipa = ipa;
            mapping.pa = pa;
            mapping.rights = rights;
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
        mapping.valid = true;
        ++s2->mapped_pages;
        metric_inc(VM_DEBUG_STAGE2_PAGES_MAPPED);
        return E_OK;
    }

    return E_OVERFLOW;
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
    s2->ipa_bits = 39;
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

    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        const Stage2Mapping& mapping = s2->mappings[i];
        if (!mapping.valid) continue;
        if (range_overlaps(ipa, size, mapping.ipa, PAGE_SIZE)) return E_BUSY;
    }
    return E_OK;
}

SysError stage2_map_page(Stage2AddressSpace* s2, u64 ipa, PhysAddr pa, u32 rights) {
    if (!s2 || !s2->built || s2->active) return E_INVALID;
    if ((ipa & (PAGE_SIZE - 1)) != 0 || (pa & (PAGE_SIZE - 1)) != 0) return E_INVALID;
    if ((rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC)) != 0) return E_INVALID;

    PhysAddr l2_pa = 0;
    PhysAddr l3_pa = 0;
    if (!ensure_table(s2, s2->root_table_phys, level_index(ipa, 1), &l2_pa)) {
        return E_NOMEM;
    }
    if (!ensure_table(s2, l2_pa, level_index(ipa, 2), &l3_pa)) {
        return E_NOMEM;
    }
    u64* l3 = table_ptr(l3_pa);
    const u32 l3_idx = level_index(ipa, 3);
    if (l3[l3_idx] & S2_DESC_VALID) return E_BUSY;
    l3[l3_idx] = (pa & S2_DESC_ADDR_MASK) | S2_DESC_BLOCK_PAGE | stage2_page_attrs(rights);

    return record_mapping(s2, ipa, pa, rights);
}

SysError stage2_unmap_page(Stage2AddressSpace* s2, u64 ipa) {
    if (!s2 || !s2->built || s2->active) return E_INVALID;
    if ((ipa & (PAGE_SIZE - 1)) != 0) return E_INVALID;

    const u64 l1_desc = table_ptr(s2->root_table_phys)[level_index(ipa, 1)];
    if (is_table_desc(l1_desc)) {
        const PhysAddr l2_pa = l1_desc & S2_DESC_ADDR_MASK;
        const u64 l2_desc = table_ptr(l2_pa)[level_index(ipa, 2)];
        if (is_table_desc(l2_desc)) {
            const PhysAddr l3_pa = l2_desc & S2_DESC_ADDR_MASK;
            table_ptr(l3_pa)[level_index(ipa, 3)] = 0;
        }
    }

    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        Stage2Mapping& mapping = s2->mappings[i];
        if (!mapping.valid || mapping.ipa != ipa) continue;
        mapping.valid = false;
        if (s2->mapped_pages > 0) --s2->mapped_pages;
        return E_OK;
    }
    return E_NOTFOUND;
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

PhysAddr stage2_lookup_page(Stage2AddressSpace* s2, u64 ipa, u32* out_rights) {
    if (!s2 || !s2->built) return 0;
    const u64 page_ipa = ipa & ~(static_cast<u64>(PAGE_SIZE - 1));
    for (u32 i = 0; i < kMaxStage2Mappings; ++i) {
        const Stage2Mapping& mapping = s2->mappings[i];
        if (!mapping.valid || mapping.ipa != page_ipa) continue;
        if (out_rights) *out_rights = mapping.rights;
        return mapping.pa;
    }
    return 0;
}

} // namespace vm
