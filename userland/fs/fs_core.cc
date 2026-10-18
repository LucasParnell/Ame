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
mx_u32_t g_data_inode = 0;

// Currently served client, published by the session loop in fs_ring.cc.
mx_u32_t g_session = 0;
mx_u64_t g_session_generation = 0;
mochi::store::State g_volume{};

// Small registered arenas. These exist so the kernel's accounting sees the
// service's memory; the actual storage is the static tables above.
alignas(4096) static mx_u8_t g_metadata_arena[4096 * 2];
alignas(4096) static mx_u8_t g_inode_arena[4096 * 2];
alignas(4096) static mx_u8_t g_extent_arena[4096 * 2];
alignas(4096) static mx_u8_t g_snapshot_arena[4096 * 2];

static const char kHelloName[] = "hello.txt";
static const char kHelloData[] = "Midix io.fs V1: ordered CoW extents and inline I/O are alive.\n";

// ---------------------------------------------------------------------------
// Checksums.
// ---------------------------------------------------------------------------
mx_u32_t name_hash32(const char* name, mx_u8_t len) {
    return libk::crc32_ieee(libk::as_bytes(name, len));
}

mx_u32_t fnv1a32_block(const mx_u8_t* data) {
    return libk::fnv1a32(libk::as_bytes(data, kBlockSize));
}

void mark_metadata_dirty() {
    g_metadata_dirty = true;
}

// ---------------------------------------------------------------------------
// Inode and extent tables.
// ---------------------------------------------------------------------------
InodeState* live_inode(mx_u32_t inode_number) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (g_inodes[i].used && g_inodes[i].inode_number == inode_number) {
            return &g_inodes[i];
        }
    }
    return nullptr;
}

static PhysRefState* phys_ref(mx_u64_t phys_block) {
    if (phys_block >= kMaxPhysBlocks) return nullptr;
    auto& ref = g_phys_refs[phys_block];
    return ref.used && ref.phys_block == phys_block ? &ref : nullptr;
}

mx_i32_t inc_phys_ref(mx_u64_t phys_block) {
    PhysRefState* ref = phys_ref(phys_block);
    if (!ref) {
        return static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
    }
    if (ref->refcount == 0xFFFF'FFFFu) {
        return static_cast<mx_i32_t>(MX_ERR_OVERFLOW);
    }
    ++ref->refcount;
    mark_metadata_dirty();
    return MX_OK;
}

void dec_phys_ref(mx_u64_t phys_block) {
    PhysRefState* ref = phys_ref(phys_block);
    if (!ref || ref->refcount == 0) {
        return;
    }
    --ref->refcount;
    mark_metadata_dirty();
    if (ref->refcount == 0) {
        libk::zero(g_data_blocks[phys_block], kBlockSize);
        libk::zero(*ref);
        mark_metadata_dirty();
    }
}

static mx_i32_t alloc_phys_block(mx_u64_t* out_phys) {
    for (mx_u32_t i = 1; i < kMaxPhysBlocks; ++i) {
        if (g_phys_refs[i].used) {
            continue;
        }
        g_phys_refs[i].used = true;
        g_phys_refs[i].phys_block = i;
        g_phys_refs[i].length_blocks = 1;
        g_phys_refs[i].refcount = 1;
        g_phys_refs[i].flags = 0;
        libk::zero(g_data_blocks[i], kBlockSize);
        if (out_phys) {
            *out_phys = i;
        }
        mark_metadata_dirty();
        return MX_OK;
    }
    return static_cast<mx_i32_t>(MX_ERR_NOMEM);
}

const ExtentState* find_extent_const(const InodeState& inode, mx_u64_t logical_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        const ExtentState& extent = inode.extents[i];
        if (!extent.used) {
            continue;
        }
        if (logical_block >= extent.logical_block &&
            logical_block - extent.logical_block < extent.length_blocks) {
            return &extent;
        }
    }
    return nullptr;
}

// Mutable view of the same lookup. The extent table is shared, so the const
// form above is the one implementation and this casts away const for callers
// that will update the extent they found.
static ExtentState* find_extent(InodeState& inode, mx_u64_t logical_block) {
    return const_cast<ExtentState*>(find_extent_const(inode, logical_block));
}

static ExtentState* alloc_extent_slot(InodeState& inode, mx_u64_t logical_block,
                                      mx_u64_t phys_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        if (inode.extents[i].used) {
            continue;
        }
        ExtentState& extent = inode.extents[i];
        extent.used = true;
        extent.logical_block = logical_block;
        extent.phys_block = phys_block;
        extent.length_blocks = 1;
        extent.generation = g_generation;
        extent.content_hash32 = fnv1a32_block(g_data_blocks[phys_block]);
        extent.flags = 0;
        mark_metadata_dirty();
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
        if (rc != MX_OK) {
            return rc;
        }
        extent = alloc_extent_slot(inode, logical_block, phys);
        if (!extent) {
            dec_phys_ref(phys);
            return static_cast<mx_i32_t>(MX_ERR_NOMEM);
        }
        if (out_phys) {
            *out_phys = phys;
        }
        return MX_OK;
    }

    PhysRefState* ref = phys_ref(extent->phys_block);
    if (!ref) {
        return static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
    }
    if (ref->refcount > 1) {
        mx_u64_t new_phys = 0;
        const mx_i32_t rc = alloc_phys_block(&new_phys);
        if (rc != MX_OK) {
            return rc;
        }
        libk::copy_exact(libk::as_bytes(g_data_blocks[new_phys]), libk::as_bytes(g_data_blocks[extent->phys_block]));
        dec_phys_ref(extent->phys_block);
        extent->phys_block = new_phys;
        extent->generation = g_generation;
        extent->content_hash32 = fnv1a32_block(g_data_blocks[new_phys]);
        mark_metadata_dirty();
    }

    if (out_phys) {
        *out_phys = extent->phys_block;
    }
    return MX_OK;
}

InodeState* alloc_inode(mx_u16_t type, mx_u16_t inode_class, mx_u32_t parent_inode,
                        const char* name, mx_u8_t name_len) {
    if (g_next_inode == 0 || name_len > kMaxName || (!name && name_len)) return nullptr;
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (g_inodes[i].used) {
            continue;
        }
        InodeState& inode = g_inodes[i];
        libk::zero(inode);
        inode.used = true;
        inode.inode_number = g_next_inode++;
        inode.parent_inode = parent_inode;
        inode.type = type;
        inode.inode_class = inode_class;
        inode.nlink = 1;
        inode.size = 0;
        copy_name(inode.name, name, name_len);
        mark_metadata_dirty();
        return &inode;
    }
    return nullptr;
}

const InodeState* find_child(mx_u64_t snapshot_id, mx_u32_t parent_inode, const char* name,
                             mx_u8_t name_len) {
    const SnapshotState* snap = snapshot_id ? snapshot_by_id(snapshot_id) : nullptr;
    if (snapshot_id && !snap) return nullptr;
    const InodeState* table = snap ? snap->inodes : g_inodes;
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        const auto& inode = table[i];
        if (inode.used && inode.parent_inode == parent_inode && name_eq(inode.name, name, name_len)) {
            return &inode;
        }
    }
    return nullptr;
}

InodeState* find_live_child(mx_u32_t parent_inode, const char* name, mx_u8_t name_len) {
    return const_cast<InodeState*>(find_child(0, parent_inode, name, name_len));
}

void release_inode_extents(InodeState& inode) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        if (!inode.extents[i].used) {
            continue;
        }
        dec_phys_ref(inode.extents[i].phys_block);
        libk::zero(inode.extents[i]);
    }
}

// ---------------------------------------------------------------------------
// Refcount verification (bcachefs-shaped invariant: extents are the index,
// phys refs are a derived authority, sync checks the two agree).
// ---------------------------------------------------------------------------
static mx_i32_t verify_extent_table(const InodeState* table, mx_u32_t count) {
    for (mx_u32_t i = 0; i < count; ++i) {
        if (!table[i].used) {
            continue;
        }
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            const ExtentState& extent = table[i].extents[e];
            if (!extent.used) {
                continue;
            }
            if (extent.phys_block >= kMaxPhysBlocks || !phys_ref(extent.phys_block)) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
            if (fnv1a32_block(g_data_blocks[extent.phys_block]) != extent.content_hash32) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
        }
    }
    return MX_OK;
}

static mx_u32_t count_extent_refs(const InodeState* table, mx_u32_t count, mx_u64_t phys_block) {
    mx_u32_t refs = 0;
    for (mx_u32_t i = 0; i < count; ++i) {
        if (!table[i].used) {
            continue;
        }
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            const ExtentState& extent = table[i].extents[e];
            if (extent.used && extent.phys_block == phys_block) {
                ++refs;
            }
        }
    }
    return refs;
}

mx_i32_t verify_refcount_index() {
    mx_i32_t rc = verify_extent_table(g_inodes, kMaxInodes);
    if (rc != MX_OK) {
        return rc;
    }
    for (mx_u32_t s = 0; s < kMaxSnapshots; ++s) {
        if (!g_snapshots[s].used) {
            continue;
        }
        rc = verify_extent_table(g_snapshots[s].inodes, kMaxInodes);
        if (rc != MX_OK) {
            return rc;
        }
    }

    for (mx_u32_t p = 0; p < kMaxPhysBlocks; ++p) {
        const PhysRefState& ref = g_phys_refs[p];
        if (!ref.used) {
            continue;
        }
        mx_u32_t counted = count_extent_refs(g_inodes, kMaxInodes, ref.phys_block);
        for (mx_u32_t s = 0; s < kMaxSnapshots; ++s) {
            if (g_snapshots[s].used) {
                counted += count_extent_refs(g_snapshots[s].inodes, kMaxInodes, ref.phys_block);
            }
        }
        if (counted != ref.refcount) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
    }
    return MX_OK;
}

// ---------------------------------------------------------------------------
// Data writes. write_bytes_unbounded is the whole path; write_inline is the
// client-facing entry that bounds one request to FS_INLINE_WRITE_MAX.
// ---------------------------------------------------------------------------
mx_i32_t write_bytes_unbounded(InodeState& inode, mx_u64_t offset, const mx_u8_t* data,
                               mx_u32_t len) {
    if (inode.type != FS_INODE_FILE) {
        return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    }
    if (inode.inode_class == FS_CLASS_CONTENT_STORE && inode.size != 0) {
        return static_cast<mx_i32_t>(MX_ERR_PERM);
    }

    mx_u64_t end = 0;
    if (!data && len) return MX_ERR_INVALID;
    if (!libk::checked_add(offset, len, &end)) return MX_ERR_OVERFLOW;
    if (len == 0) return MX_OK;
    mx_u32_t written = 0;
    while (written < len) {
        const mx_u64_t abs = offset + written;
        const mx_u64_t logical_block = abs / kBlockSize;
        const mx_u32_t block_off = static_cast<mx_u32_t>(abs % kBlockSize);
        const mx_u32_t chunk = min_u32(len - written, kBlockSize - block_off);
        mx_u64_t phys = 0;
        const mx_i32_t rc = ensure_writable_block(inode, logical_block, &phys);
        if (rc != MX_OK) {
            return rc;
        }
        libk::copy_n(libk::slice(libk::as_bytes(g_data_blocks[phys]), block_off, chunk),
                     libk::as_bytes(data + written, len - written), chunk);
        ExtentState* extent = find_extent(inode, logical_block);
        if (!extent) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
        extent->content_hash32 = fnv1a32_block(g_data_blocks[phys]);
        written += chunk;
    }

    if (end > inode.size) {
        inode.size = end;
    }
    ++g_generation;
    mark_metadata_dirty();
    return MX_OK;
}

mx_i32_t write_inline(InodeState& inode, mx_u64_t offset, const mx_u8_t* data, mx_u32_t len) {
    if (len > FS_INLINE_WRITE_MAX) {
        return static_cast<mx_i32_t>(MX_ERR_INVALID);
    }
    return write_bytes_unbounded(inode, offset, data, len);
}

// Seed an immutable content-store inode with its initial bytes. write_inline
// refuses to extend a non-empty content-store, so the content is staged while
// the inode is temporarily user-data, then the class is restored.
static void init_file_content(InodeState& inode, const char* data, mx_u32_t len) {
    const auto* bytes = reinterpret_cast<const mx_u8_t*>(data);
    inode.size = 0;
    inode.inode_class = FS_CLASS_USER_DATA;
    for (mx_u32_t off = 0; off < len;) {
        const mx_u32_t chunk = min_u32(len - off, FS_INLINE_WRITE_MAX);
        (void)write_inline(inode, off, bytes + off, chunk);
        off += chunk;
    }
    inode.inode_class = FS_CLASS_CONTENT_STORE;
}

// ---------------------------------------------------------------------------
// Formatting and arena registration.
// ---------------------------------------------------------------------------
void format_memory_fs() {
    libk::zero(g_inodes);
    libk::zero(g_phys_refs);
    libk::zero(g_snapshots);
    libk::zero(g_handles);
    libk::zero(g_data_blocks);
    g_next_inode = kRootInode + 1;
    g_generation = 1;
    g_last_snapshot_id = 0;
    g_metadata_dirty = false;

    g_data_inode = 0;
    InodeState& root = g_inodes[0];
    root.used = true;
    root.inode_number = kRootInode;
    root.parent_inode = 0;
    root.type = FS_INODE_DIR;
    root.inode_class = FS_CLASS_USER_DATA;
    root.nlink = 1;
    copy_name(root.name, "", 0);

    g_handles[kRootHandle].used = true;
    g_handles[kRootHandle].inode_number = kRootInode;
    g_handles[kRootHandle].snapshot_id = 0;
    g_handles[kRootHandle].access = fs_handle_access(FS_INODE_DIR, false);
    g_handles[kRootHandle].type = FS_INODE_DIR;

    // /tmp is created only in the in-memory table. It is the volatile mount: the
    // disk root stays read-only and never sees these inodes.
    (void)alloc_inode(FS_INODE_DIR, FS_CLASS_USER_DATA, kRootInode, kTmpMountName, kTmpMountNameLen);

    // /data is the durable user volume. It always exists; whether it survives a
    // reboot depends on a writable disk region (see fs_durable.cc).
    auto* data = alloc_inode(FS_INODE_DIR, FS_CLASS_USER_DATA, kRootInode, kDataMountName,
                             kDataMountNameLen);
    g_data_inode = data ? data->inode_number : 0;

    InodeState* hello =
        alloc_inode(FS_INODE_FILE, FS_CLASS_CONTENT_STORE, kRootInode, kHelloName,
                    static_cast<mx_u8_t>(str_len(kHelloName, sizeof(kHelloName))));
    if (hello) {
        init_file_content(*hello, kHelloData, static_cast<mx_u32_t>(sizeof(kHelloData) - 1));
    }
}

void register_service_arenas() {
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(&g_volume), sizeof(g_volume), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_metadata_arena),
                             sizeof(g_metadata_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_inode_arena), sizeof(g_inode_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_extent_arena), sizeof(g_extent_arena),
                             0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_snapshot_arena),
                             sizeof(g_snapshot_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_data_blocks), sizeof(g_data_blocks), 0);
}
