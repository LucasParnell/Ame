// userland/fs/fs_core.cc — in-memory filesystem core.
//
// The fixed-capacity inode/extent/phys-ref tables, the checksums they carry,
// and the ordered-write path. This module owns the memory filesystem and the
// shared tables; it has no ring or session knowledge.

#include "fs_internal.h"

// Shared tables. Defined once here; declared extern in fs_internal.h.
InodeState g_inodes[kMaxInodes];
PhysRefState g_phys_refs[kMaxPhysBlocks];
SnapshotState g_snapshots[kMaxSnapshots];
OpenHandle g_handles[kMaxHandles];
mx_u32_t g_handle_generations[kMaxHandles];
alignas(4096) mx_u8_t g_data_blocks[kMaxPhysBlocks][kBlockSize];
mx_u32_t g_next_inode = kRootInode + 1;
mx_u64_t g_generation = 1;
mx_u64_t g_last_snapshot_id = 0;
bool g_metadata_dirty = false;

// Currently served client, published by the session loop in fs_ring.cc.
mx_u32_t g_session = 0;
mx_u64_t g_session_generation = 0;

// Small registered arenas. These exist so the kernel's accounting sees the
// service's memory; the actual storage is the static tables above.
alignas(4096) static mx_u8_t g_metadata_arena[4096 * 2];
alignas(4096) static mx_u8_t g_inode_arena[4096 * 2];
alignas(4096) static mx_u8_t g_extent_arena[4096 * 2];
alignas(4096) static mx_u8_t g_snapshot_arena[4096 * 2];

[[maybe_unused]] static mx_u32_t g_tmp_inode;

static const char kHelloName[] = "hello.txt";
static const char kHelloData[] =
    "Midix io.fs V1: ordered CoW extents and inline I/O are alive.\n";

// ---------------------------------------------------------------------------
// Checksums.
// ---------------------------------------------------------------------------
mx_u32_t crc32_ieee(const mx_u8_t* data, mx_u64_t len) {
    mx_u32_t crc = 0xFFFF'FFFFu;
    for (mx_u64_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (mx_u32_t bit = 0; bit < 8; ++bit) {
            const mx_u32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB8'8320u & mask);
        }
    }
    return ~crc;
}

mx_u32_t name_hash32(const char* name, mx_u8_t len) {
    return crc32_ieee(reinterpret_cast<const mx_u8_t*>(name), len);
}

mx_u32_t checksum_block(const mx_u8_t* data) {
    mx_u32_t hash = 2166136261u;
    for (mx_u32_t i = 0; i < kBlockSize; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

void note_metadata_delta(FsMutation type, mx_u32_t target) {
    (void)type;
    (void)target;
    g_metadata_dirty = true;
}

// ---------------------------------------------------------------------------
// Inode and extent tables.
// ---------------------------------------------------------------------------
InodeState* live_inode(mx_u32_t inode_number) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (g_inodes[i].used && g_inodes[i].inode_number == inode_number) return &g_inodes[i];
    }
    return nullptr;
}

static PhysRefState* phys_ref(mx_u64_t phys_block) {
    for (mx_u32_t i = 0; i < kMaxPhysBlocks; ++i) {
        if (g_phys_refs[i].used && g_phys_refs[i].phys_block == phys_block) {
            return &g_phys_refs[i];
        }
    }
    return nullptr;
}

mx_i32_t inc_phys_ref(mx_u64_t phys_block) {
    PhysRefState* ref = phys_ref(phys_block);
    if (!ref) return static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
    if (ref->refcount == 0xFFFF'FFFFu) return static_cast<mx_i32_t>(MX_ERR_OVERFLOW);
    ++ref->refcount;
    note_metadata_delta(FsMutation::PhysRefInc, static_cast<mx_u32_t>(phys_block));
    return MX_OK;
}

void dec_phys_ref(mx_u64_t phys_block) {
    PhysRefState* ref = phys_ref(phys_block);
    if (!ref || ref->refcount == 0) return;
    --ref->refcount;
    note_metadata_delta(FsMutation::PhysRefDec, static_cast<mx_u32_t>(phys_block));
    if (ref->refcount == 0) {
        zero_bytes(g_data_blocks[phys_block], kBlockSize);
        zero_bytes(ref, sizeof(*ref));
        note_metadata_delta(FsMutation::ExtentFree, static_cast<mx_u32_t>(phys_block));
    }
}

static mx_i32_t alloc_phys_block(mx_u64_t* out_phys) {
    for (mx_u32_t i = 1; i < kMaxPhysBlocks; ++i) {
        if (g_phys_refs[i].used) continue;
        g_phys_refs[i].used = true;
        g_phys_refs[i].phys_block = i;
        g_phys_refs[i].length_blocks = 1;
        g_phys_refs[i].refcount = 1;
        g_phys_refs[i].flags = 0;
        zero_bytes(g_data_blocks[i], kBlockSize);
        if (out_phys) *out_phys = i;
        note_metadata_delta(FsMutation::ExtentAlloc, i);
        return MX_OK;
    }
    return static_cast<mx_i32_t>(MX_ERR_NOMEM);
}

static ExtentState* find_extent(InodeState& inode, mx_u64_t logical_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        ExtentState& extent = inode.extents[i];
        if (!extent.used) continue;
        if (logical_block >= extent.logical_block &&
            logical_block < extent.logical_block + extent.length_blocks) {
            return &extent;
        }
    }
    return nullptr;
}

const ExtentState* find_extent_const(const InodeState& inode, mx_u64_t logical_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        const ExtentState& extent = inode.extents[i];
        if (!extent.used) continue;
        if (logical_block >= extent.logical_block &&
            logical_block < extent.logical_block + extent.length_blocks) {
            return &extent;
        }
    }
    return nullptr;
}

static ExtentState* alloc_extent_slot(InodeState& inode, mx_u64_t logical_block,
                                      mx_u64_t phys_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        if (inode.extents[i].used) continue;
        ExtentState& extent = inode.extents[i];
        extent.used = true;
        extent.logical_block = logical_block;
        extent.phys_block = phys_block;
        extent.length_blocks = 1;
        extent.generation = g_generation;
        extent.data_crc32 = checksum_block(g_data_blocks[phys_block]);
        extent.flags = 0;
        note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
        return &extent;
    }
    return nullptr;
}

static mx_i32_t ensure_writable_block(InodeState& inode, mx_u64_t logical_block,
                                      mx_u64_t* out_phys) {
    ExtentState* extent = find_extent(inode, logical_block);
    if (!extent) {
        mx_u64_t phys = 0;
        const mx_i32_t rc = alloc_phys_block(&phys);
        if (rc != MX_OK) return rc;
        extent = alloc_extent_slot(inode, logical_block, phys);
        if (!extent) {
            dec_phys_ref(phys);
            return static_cast<mx_i32_t>(MX_ERR_NOMEM);
        }
        if (out_phys) *out_phys = phys;
        return MX_OK;
    }

    PhysRefState* ref = phys_ref(extent->phys_block);
    if (!ref) return static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
    if (ref->refcount > 1) {
        mx_u64_t new_phys = 0;
        const mx_i32_t rc = alloc_phys_block(&new_phys);
        if (rc != MX_OK) return rc;
        fs_copy_bytes(g_data_blocks[new_phys], g_data_blocks[extent->phys_block], kBlockSize);
        dec_phys_ref(extent->phys_block);
        extent->phys_block = new_phys;
        extent->generation = g_generation;
        extent->data_crc32 = checksum_block(g_data_blocks[new_phys]);
        note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
    }

    if (out_phys) *out_phys = extent->phys_block;
    return MX_OK;
}

InodeState* alloc_inode(mx_u16_t type, mx_u16_t inode_class,
                        mx_u32_t parent_inode, const char* name,
                        mx_u8_t name_len, mx_u32_t mode) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (g_inodes[i].used) continue;
        InodeState& inode = g_inodes[i];
        zero_bytes(&inode, sizeof(inode));
        inode.used = true;
        inode.inode_number = g_next_inode++;
        inode.parent_inode = parent_inode;
        inode.type = type;
        inode.inode_class = inode_class;
        inode.mode = mode;
        inode.nlink = 1;
        inode.size = 0;
        copy_name(inode.name, name, name_len);
        note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
        return &inode;
    }
    return nullptr;
}

const InodeState* find_child(mx_u64_t snapshot_id, mx_u32_t parent_inode,
                             const char* name, mx_u8_t name_len) {
    if (snapshot_id) {
        const SnapshotState* snap = snapshot_by_id(snapshot_id);
        if (!snap) return nullptr;
        for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
            const InodeState& inode = snap->inodes[i];
            if (inode.used && inode.parent_inode == parent_inode &&
                name_eq(inode.name, name, name_len)) {
                return &inode;
            }
        }
        return nullptr;
    }

    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        const InodeState& inode = g_inodes[i];
        if (inode.used && inode.parent_inode == parent_inode &&
            name_eq(inode.name, name, name_len)) {
            return &inode;
        }
    }
    return nullptr;
}

InodeState* find_live_child(mx_u32_t parent_inode, const char* name, mx_u8_t name_len) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        InodeState& inode = g_inodes[i];
        if (inode.used && inode.parent_inode == parent_inode &&
            name_eq(inode.name, name, name_len)) {
            return &inode;
        }
    }
    return nullptr;
}

void release_inode_extents(InodeState& inode) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        if (!inode.extents[i].used) continue;
        dec_phys_ref(inode.extents[i].phys_block);
        zero_bytes(&inode.extents[i], sizeof(inode.extents[i]));
    }
}

// ---------------------------------------------------------------------------
// Refcount verification (bcachefs-shaped invariant: extents are the index,
// phys refs are a derived authority, sync checks the two agree).
// ---------------------------------------------------------------------------
static mx_i32_t verify_extent_table(const InodeState* table, mx_u32_t count) {
    for (mx_u32_t i = 0; i < count; ++i) {
        if (!table[i].used) continue;
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            const ExtentState& extent = table[i].extents[e];
            if (!extent.used) continue;
            if (extent.phys_block >= kMaxPhysBlocks || !phys_ref(extent.phys_block)) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
            if (checksum_block(g_data_blocks[extent.phys_block]) != extent.data_crc32) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
        }
    }
    return MX_OK;
}

static mx_u32_t count_extent_refs(const InodeState* table, mx_u32_t count,
                                  mx_u64_t phys_block) {
    mx_u32_t refs = 0;
    for (mx_u32_t i = 0; i < count; ++i) {
        if (!table[i].used) continue;
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            const ExtentState& extent = table[i].extents[e];
            if (extent.used && extent.phys_block == phys_block) ++refs;
        }
    }
    return refs;
}

mx_i32_t verify_refcount_index() {
    mx_i32_t rc = verify_extent_table(g_inodes, kMaxInodes);
    if (rc != MX_OK) return rc;
    for (mx_u32_t s = 0; s < kMaxSnapshots; ++s) {
        if (!g_snapshots[s].used) continue;
        rc = verify_extent_table(g_snapshots[s].inodes, kMaxInodes);
        if (rc != MX_OK) return rc;
    }

    for (mx_u32_t p = 0; p < kMaxPhysBlocks; ++p) {
        const PhysRefState& ref = g_phys_refs[p];
        if (!ref.used) continue;
        mx_u32_t counted = count_extent_refs(g_inodes, kMaxInodes, ref.phys_block);
        for (mx_u32_t s = 0; s < kMaxSnapshots; ++s) {
            if (g_snapshots[s].used) {
                counted += count_extent_refs(g_snapshots[s].inodes, kMaxInodes, ref.phys_block);
            }
        }
        if (counted != ref.refcount) return static_cast<mx_i32_t>(MX_ERR_FAULT);
    }
    return MX_OK;
}

// ---------------------------------------------------------------------------
// Inline writes.
// ---------------------------------------------------------------------------
mx_i32_t write_inline(InodeState& inode, mx_u64_t offset,
                      const mx_u8_t* data, mx_u32_t len) {
    if (inode.type != FS_INODE_FILE) return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    if (inode.inode_class == FS_CLASS_CONTENT_STORE && inode.size != 0) {
        return static_cast<mx_i32_t>(MX_ERR_PERM);
    }
    if (len > FS_INLINE_WRITE_MAX) return static_cast<mx_i32_t>(MX_ERR_INVALID);

    mx_u32_t written = 0;
    while (written < len) {
        const mx_u64_t abs = offset + written;
        const mx_u64_t logical_block = abs / kBlockSize;
        const mx_u32_t block_off = static_cast<mx_u32_t>(abs % kBlockSize);
        const mx_u32_t chunk = min_u32(len - written, kBlockSize - block_off);
        mx_u64_t phys = 0;
        const mx_i32_t rc = ensure_writable_block(inode, logical_block, &phys);
        if (rc != MX_OK) return rc;
        fs_copy_bytes(g_data_blocks[phys] + block_off, data + written, chunk);
        ExtentState* extent = find_extent(inode, logical_block);
        if (!extent) return static_cast<mx_i32_t>(MX_ERR_FAULT);
        extent->data_crc32 = checksum_block(g_data_blocks[phys]);
        written += chunk;
    }

    if (offset + len > inode.size) inode.size = offset + len;
    ++g_generation;
    note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
    return MX_OK;
}

static void init_file_content(InodeState& inode, const char* data, mx_u32_t len) {
    inode.size = 0;
    inode.inode_class = FS_CLASS_CONTENT_STORE;
    const mx_u16_t old_class = inode.inode_class;
    inode.inode_class = FS_CLASS_USER_DATA;
    (void)write_inline(inode, 0, reinterpret_cast<const mx_u8_t*>(data), min_u32(len, FS_INLINE_WRITE_MAX));
    if (len > FS_INLINE_WRITE_MAX) {
        mx_u32_t off = FS_INLINE_WRITE_MAX;
        while (off < len) {
            const mx_u32_t chunk = min_u32(FS_INLINE_WRITE_MAX, len - off);
            (void)write_inline(inode, off, reinterpret_cast<const mx_u8_t*>(data + off), chunk);
            off += chunk;
        }
    }
    inode.inode_class = old_class;
}

// ---------------------------------------------------------------------------
// Formatting and arena registration.
// ---------------------------------------------------------------------------
void format_memory_fs() {
    zero_bytes(g_inodes, sizeof(g_inodes));
    zero_bytes(g_phys_refs, sizeof(g_phys_refs));
    zero_bytes(g_snapshots, sizeof(g_snapshots));
    zero_bytes(g_handles, sizeof(g_handles));
    zero_bytes(g_data_blocks, sizeof(g_data_blocks));
    g_next_inode = kRootInode + 1;
    g_generation = 1;
    g_last_snapshot_id = 0;
    g_metadata_dirty = false;

    InodeState& root = g_inodes[0];
    root.used = true;
    root.inode_number = kRootInode;
    root.parent_inode = 0;
    root.type = FS_INODE_DIR;
    root.inode_class = FS_CLASS_USER_DATA;
    root.mode = 0755;
    root.nlink = 1;
    copy_name(root.name, "", 0);

    g_handles[kRootHandle].used = true;
    g_handles[kRootHandle].inode_number = kRootInode;
    g_handles[kRootHandle].snapshot_id = 0;
    g_handles[kRootHandle].read_only = true;
    g_handles[kRootHandle].type = FS_INODE_DIR;

    // /tmp is created only in the in-memory table. It is the volatile mount: the
    // disk root stays read-only and never sees these inodes.
    auto* tmp = alloc_inode(FS_INODE_DIR, FS_CLASS_USER_DATA, kRootInode, "tmp", 3, 0755);
    g_tmp_inode = tmp ? tmp->inode_number : 0;

    InodeState* hello = alloc_inode(FS_INODE_FILE, FS_CLASS_CONTENT_STORE,
                                    kRootInode, kHelloName,
                                    static_cast<mx_u8_t>(str_len(kHelloName, sizeof(kHelloName))),
                                    0444);
    if (hello) {
        init_file_content(*hello, kHelloData, static_cast<mx_u32_t>(sizeof(kHelloData) - 1));
    }
}

void register_service_arenas() {
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_metadata_arena),
                             sizeof(g_metadata_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_inode_arena),
                             sizeof(g_inode_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_extent_arena),
                             sizeof(g_extent_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_snapshot_arena),
                             sizeof(g_snapshot_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_data_blocks),
                             sizeof(g_data_blocks), 0);
}
