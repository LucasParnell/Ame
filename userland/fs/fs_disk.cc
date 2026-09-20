// userland/fs/fs_disk.cc — read-only midix-fs-v1 disk backend.
//
// On top of the block driver this provides a GPT scan for the AMEROOT
// partition, a small LRU block cache, and the B+tree lookups used by inode,
// extent, and directory reads. The backend is read-only: it never writes the
// image, so the mounted root cannot be mutated.

#include "fs_internal.h"
#include "mochi/log.h"

constexpr mx_vaddr_t kVaBlkReqRing = 0x2002'0000ull;
constexpr mx_vaddr_t kVaBlkReplyRing = 0x2003'0000ull;
constexpr mx_vaddr_t kVaBlkDma = 0x2100'0000ull;

constexpr mx_u16_t kBTreeInternal = 1;
constexpr mx_u16_t kBTreeLeaf = 2;
constexpr mx_u32_t kBTreeMaxEntries = 63;
constexpr mx_u32_t kDiskBlockCacheEntries = 32;

// Bound on one block-driver round trip. If drv.blk stops answering (crash or
// stall) io.fs must not wedge inside a disk read; the request instead fails and
// the caller degrades to the memory backend / an error reply.
constexpr mx_u64_t kBlkReplyTimeoutNs = 1'000'000'000ull;

struct DiskBlockCacheEntry {
    bool valid;
    mx_u64_t block;
    mx_u64_t age;
    alignas(4096) mx_u8_t data[kBlockSize];
};

static mochi::fs::BlkClient g_blk_client{};
static bool g_blk_available = false;
static mx_u8_t* g_blk_dma = nullptr;
static mx_u64_t g_blk_dma_size = 0;
static mx_u64_t g_disk_base_sector = 0;
static mx_u64_t g_disk_partition_sectors = 0;
static mx_u64_t g_disk_total_blocks = 0;
static mx_u64_t g_disk_inode_root = 0;
static DiskBlockCacheEntry g_disk_cache[kDiskBlockCacheEntries];
static mx_u64_t g_disk_cache_clock = 1;
alignas(4096) static mx_u8_t g_gpt_entries[64 * 1024];
alignas(4096) static mx_u8_t g_disk_block[kBlockSize];

static bool g_disk_mounted = false;

bool disk_mounted() { return g_disk_mounted; }

static mx_u32_t load_le32(const mx_u8_t* p) {
    return static_cast<mx_u32_t>(p[0]) |
           (static_cast<mx_u32_t>(p[1]) << 8) |
           (static_cast<mx_u32_t>(p[2]) << 16) |
           (static_cast<mx_u32_t>(p[3]) << 24);
}

static mx_u64_t load_le64(const mx_u8_t* p) {
    mx_u64_t v = 0;
    for (mx_u32_t i = 0; i < 8; ++i) v |= static_cast<mx_u64_t>(p[i]) << (i * 8);
    return v;
}

// ---------------------------------------------------------------------------
// Block cache.
// ---------------------------------------------------------------------------
static bool disk_cache_lookup(mx_u64_t block, mx_u8_t* out) {
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        DiskBlockCacheEntry& entry = g_disk_cache[i];
        if (!entry.valid || entry.block != block) continue;
        entry.age = ++g_disk_cache_clock;
        fs_copy_bytes(out, entry.data, kBlockSize);
        return true;
    }
    return false;
}

static DiskBlockCacheEntry& disk_cache_victim() {
    mx_u32_t victim = 0;
    mx_u64_t oldest = ~0ull;
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        if (!g_disk_cache[i].valid) return g_disk_cache[i];
        if (g_disk_cache[i].age < oldest) {
            oldest = g_disk_cache[i].age;
            victim = i;
        }
    }
    return g_disk_cache[victim];
}

static void disk_cache_store(mx_u64_t block, const mx_u8_t* data) {
    DiskBlockCacheEntry& entry = disk_cache_victim();
    entry.valid = true;
    entry.block = block;
    entry.age = ++g_disk_cache_clock;
    fs_copy_bytes(entry.data, data, kBlockSize);
}

static void disk_cache_reset() {
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        g_disk_cache[i].valid = false;
        g_disk_cache[i].block = 0;
        g_disk_cache[i].age = 0;
    }
    g_disk_cache_clock = 1;
}

// ---------------------------------------------------------------------------
// Block device access.
// ---------------------------------------------------------------------------
static bool disk_read_sectors(mx_u64_t sector, mx_u32_t sector_count, mx_u8_t* out) {
    if (!g_blk_available || !g_blk_dma || !out || sector_count == 0) return false;
    const mx_u64_t byte_count = static_cast<mx_u64_t>(sector_count) * 512ull;
    if (g_blk_dma_size < BLK_DMA_CLIENT_OFFSET ||
        byte_count > g_blk_dma_size - BLK_DMA_CLIENT_OFFSET) {
        return false;
    }
    BlkReply reply{};
    if (!g_blk_client.request(BLK_READ,
                              sector,
                              sector_count,
                              MX_NULL_HANDLE,
                              BLK_DMA_CLIENT_OFFSET,
                              &reply,
                              mx_clock_ns() + kBlkReplyTimeoutNs)) {
        return false;
    }
    if (reply.status != MX_OK || reply.sectors_transferred != sector_count) return false;
    fs_copy_bytes(out, g_blk_dma + BLK_DMA_CLIENT_OFFSET, byte_count);
    return true;
}

static bool disk_read_block(mx_u64_t block, mx_u8_t* out) {
    if (!g_blk_available || !g_blk_dma || !out) return false;
    if (g_disk_total_blocks != 0 && block >= g_disk_total_blocks) return false;
    if (disk_cache_lookup(block, out)) return true;
    const mx_u64_t sector = g_disk_base_sector + block * kSectorsPerFsBlock;
    if (g_disk_partition_sectors != 0 &&
        (sector < g_disk_base_sector ||
         sector + kSectorsPerFsBlock > g_disk_base_sector + g_disk_partition_sectors)) {
        return false;
    }
    if (!disk_read_sectors(sector, kSectorsPerFsBlock, out)) return false;
    disk_cache_store(block, out);
    return true;
}

// ---------------------------------------------------------------------------
// B+tree traversal.
// ---------------------------------------------------------------------------
enum class DiskTree : mx_u32_t {
    Inode,
    Extent,
    Directory,
};

static mx_u64_t disk_entry_key(const mx_u8_t* entry, DiskTree tree) {
    switch (tree) {
    case DiskTree::Inode:
        return load_le32(entry);
    case DiskTree::Extent:
    case DiskTree::Directory:
        return load_le64(entry);
    }
    return 0;
}

static bool disk_btree_find(mx_u64_t root, mx_u64_t key, DiskTree tree,
                            mx_u8_t out_entry[64]) {
    mx_u64_t block = root;
    for (mx_u32_t depth = 0; depth < 8; ++depth) {
        if (block == 0 || !disk_read_block(block, g_disk_block)) return false;
        const auto* header = reinterpret_cast<const BTreeNodeHeader*>(g_disk_block);
        if (header->entry_count > kBTreeMaxEntries) return false;
        if (header->node_type == kBTreeInternal) {
            mx_u64_t child = 0;
            const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
            for (mx_u32_t i = 0; i < header->entry_count; ++i) {
                const mx_u8_t* entry = entries + (i * 64);
                const mx_u64_t max_key = load_le64(entry);
                if (key <= max_key) {
                    child = load_le64(entry + 8);
                    break;
                }
            }
            if (child == 0) return false;
            block = child;
            continue;
        }
        if (header->node_type != kBTreeLeaf) return false;

        const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
        for (mx_u32_t i = 0; i < header->entry_count; ++i) {
            const mx_u8_t* entry = entries + (i * 64);
            const mx_u64_t entry_key = disk_entry_key(entry, tree);
            if (entry_key == key) {
                fs_copy_bytes(out_entry, entry, 64);
                return true;
            }
            if (entry_key > key) return false;
        }
        return false;
    }
    return false;
}

bool disk_inode(mx_u32_t inode_number, InodeLeafEntry* out) {
    if (!g_disk_mounted || !out) return false;
    alignas(8) mx_u8_t entry[64];
    if (!disk_btree_find(g_disk_inode_root, inode_number, DiskTree::Inode, entry)) return false;
    fs_copy_bytes(out, entry, sizeof(InodeLeafEntry));
    return out->inode_number == inode_number;
}

bool disk_dir_lookup(const InodeLeafEntry& dir, mx_u32_t parent_inode,
                     const char* name, mx_u8_t name_len,
                     InodeLeafEntry* out_inode) {
    if (dir.type != FS_INODE_DIR || name_len == 0 || !out_inode) return false;
    alignas(8) mx_u8_t entry[64];
    const mx_u64_t key =
        (static_cast<mx_u64_t>(parent_inode) << 32) | name_hash32(name, name_len);
    if (!disk_btree_find(dir.extent_tree_root, key, DiskTree::Directory, entry)) return false;
    const auto* dirent = reinterpret_cast<const DirLeafEntry*>(entry);
    if (dirent->compound_key != key || dirent->name_len != name_len) return false;
    for (mx_u32_t i = 0; i < name_len; ++i) {
        if (dirent->name[i] != name[i]) return false;
    }
    return disk_inode(dirent->child_inode, out_inode);
}

static bool disk_btree_nth_entry(mx_u64_t root, mx_u32_t target,
                                 mx_u32_t* cursor, mx_u8_t out_entry[64]) {
    if (root == 0 || !cursor || !out_entry) return false;
    if (!disk_read_block(root, g_disk_block)) return false;
    const auto* header = reinterpret_cast<const BTreeNodeHeader*>(g_disk_block);
    if (header->entry_count > kBTreeMaxEntries) return false;
    const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
    if (header->node_type == kBTreeLeaf) {
        for (mx_u32_t i = 0; i < header->entry_count; ++i) {
            if (*cursor == target) {
                fs_copy_bytes(out_entry, entries + (i * 64), 64);
                return true;
            }
            ++(*cursor);
        }
        return false;
    }
    if (header->node_type != kBTreeInternal) return false;

    mx_u64_t children[kBTreeMaxEntries];
    const mx_u32_t count = header->entry_count;
    for (mx_u32_t i = 0; i < count; ++i) {
        children[i] = load_le64(entries + (i * 64) + 8);
    }
    for (mx_u32_t i = 0; i < count; ++i) {
        if (disk_btree_nth_entry(children[i], target, cursor, out_entry)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Disk-backed file/dir reads.
// ---------------------------------------------------------------------------
mx_i32_t disk_read_file(const OpenHandle& handle, mx_u64_t offset,
                        mx_u32_t length, FsReadReply* reply) {
    if (!reply) return static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (handle.type != FS_INODE_FILE) return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    if (length > FS_INLINE_READ_MAX) return static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED);
    if (offset >= handle.size) {
        reply->length_read = 0;
        return MX_OK;
    }
    const mx_u32_t remaining = static_cast<mx_u32_t>(handle.size - offset);
    const mx_u32_t n = min_u32(length, min_u32(remaining, FS_INLINE_READ_MAX));
    mx_u32_t copied = 0;
    while (copied < n) {
        const mx_u64_t abs = offset + copied;
        const mx_u64_t logical_block = abs / kBlockSize;
        const mx_u32_t block_off = static_cast<mx_u32_t>(abs % kBlockSize);
        const mx_u32_t chunk = min_u32(n - copied, kBlockSize - block_off);
        alignas(8) mx_u8_t entry[64];
        if (!disk_btree_find(handle.tree_root, logical_block, DiskTree::Extent, entry)) {
            zero_bytes(reply->inline_data + copied, chunk);
            copied += chunk;
            continue;
        }
        const auto* extent = reinterpret_cast<const ExtentLeafEntry*>(entry);
        if (extent->logical_block != logical_block ||
            extent->phys_block >= g_disk_total_blocks ||
            !disk_read_block(extent->phys_block, g_disk_block)) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
        if (extent->data_crc32 != 0 &&
            crc32_ieee(g_disk_block, kBlockSize) != extent->data_crc32) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
        fs_copy_bytes(reply->inline_data + copied, g_disk_block + block_off, chunk);
        copied += chunk;
    }
    reply->length_read = n;
    return MX_OK;
}

mx_i32_t disk_readdir(const OpenHandle& handle,
                      const FsReaddirRequest& req,
                      FsReaddirReply* reply) {
    if (!reply) return static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (handle.type != FS_INODE_DIR) return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    mx_u32_t cursor = 0;
    alignas(8) mx_u8_t entry[64];
    if (!disk_btree_nth_entry(handle.tree_root, req.cookie, &cursor, entry)) {
        reply->flags = FS_READDIR_EOF;
        reply->entry_count = cursor;
        return MX_OK;
    }
    const auto* dirent = reinterpret_cast<const DirLeafEntry*>(entry);
    reply->flags = FS_READDIR_HAS_ENTRY;
    reply->next_cookie = req.cookie + 1;
    reply->inode_number = dirent->child_inode;
    reply->type = dirent->file_type;
    mx_u32_t n = dirent->name_len;
    if (n > sizeof(reply->name)) {
        n = sizeof(reply->name);
        reply->flags |= FS_READDIR_TRUNCATED;
    }
    reply->name_len = static_cast<mx_u8_t>(n);
    for (mx_u32_t i = 0; i < n; ++i) reply->name[i] = dirent->name[i];
    mx_u32_t after = 0;
    mx_u8_t ignored[64];
    if (!disk_btree_nth_entry(handle.tree_root, req.cookie + 1, &after, ignored)) {
        reply->flags |= FS_READDIR_EOF;
    }
    reply->entry_count = after;
    return MX_OK;
}

// ---------------------------------------------------------------------------
// Handle allocation for disk-backed inodes. These are always read-only.
// ---------------------------------------------------------------------------
fs_handle_t alloc_disk_handle(const InodeLeafEntry& inode) {
    for (mx_u32_t i = 2; i < kMaxHandles; ++i) {
        if (g_handles[i].used || g_handle_generations[i] == 0xffffff) continue;
        OpenHandle& h = g_handles[i];
        zero_bytes(&h, sizeof(h));
        h.used = true;
        h.disk_backed = true;
        h.inode_number = inode.inode_number;
        h.read_only = true;
        h.type = inode.type;
        h.inode_class = inode.inode_class;
        h.mode = inode.mode;
        h.size = inode.size;
        h.tree_root = inode.extent_tree_root;
        return handle_token(i);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Mount: GPT scan + superblock validation.
// ---------------------------------------------------------------------------
static bool gpt_name_eq(const mx_u8_t* utf16le, const char* ascii) {
    mx_u32_t i = 0;
    for (; ascii[i]; ++i) {
        if (i * 2 + 1 >= 72) return false;
        if (utf16le[i * 2] != static_cast<mx_u8_t>(ascii[i]) ||
            utf16le[i * 2 + 1] != 0) {
            return false;
        }
    }
    if (i * 2 + 1 >= 72) return true;
    return utf16le[i * 2] == 0 && utf16le[i * 2 + 1] == 0;
}

static bool select_ameroot_partition() {
    g_disk_base_sector = 0;
    g_disk_partition_sectors = 0;
    if (!disk_read_sectors(1, 1, g_disk_block)) return false;
    if (g_disk_block[0] != 'E' || g_disk_block[1] != 'F' ||
        g_disk_block[2] != 'I' || g_disk_block[3] != ' ' ||
        g_disk_block[4] != 'P' || g_disk_block[5] != 'A' ||
        g_disk_block[6] != 'R' || g_disk_block[7] != 'T') {
        return false;
    }

    const mx_u32_t header_size = load_le32(g_disk_block + 12);
    const mx_u32_t header_crc = load_le32(g_disk_block + 16);
    if (header_size < 92 || header_size > 512) return false;
    g_disk_block[16] = g_disk_block[17] = g_disk_block[18] = g_disk_block[19] = 0;
    if (crc32_ieee(g_disk_block, header_size) != header_crc) return false;

    const mx_u64_t entries_lba = load_le64(g_disk_block + 72);
    const mx_u32_t entry_count = load_le32(g_disk_block + 80);
    const mx_u32_t entry_size = load_le32(g_disk_block + 84);
    const mx_u32_t entries_crc = load_le32(g_disk_block + 88);
    if (entry_count == 0 || entry_size < 128 || entry_size > 512) return false;
    const mx_u64_t entries_bytes64 = static_cast<mx_u64_t>(entry_count) * entry_size;
    if (entries_bytes64 == 0 || entries_bytes64 > sizeof(g_gpt_entries)) return false;
    const mx_u32_t entries_bytes = static_cast<mx_u32_t>(entries_bytes64);
    const mx_u32_t entries_sectors =
        static_cast<mx_u32_t>((entries_bytes64 + 511ull) / 512ull);
    if (!disk_read_sectors(entries_lba, entries_sectors, g_gpt_entries)) return false;
    if (crc32_ieee(g_gpt_entries, entries_bytes) != entries_crc) return false;

    for (mx_u32_t i = 0; i < entry_count; ++i) {
        const mx_u8_t* entry = g_gpt_entries + static_cast<mx_u64_t>(i) * entry_size;
        bool empty = true;
        for (mx_u32_t b = 0; b < 16; ++b) empty = empty && entry[b] == 0;
        if (empty) continue;
        if (!gpt_name_eq(entry + 56, "AMEROOT")) continue;
        const mx_u64_t first = load_le64(entry + 32);
        const mx_u64_t last = load_le64(entry + 40);
        if (first == 0 || last < first) return false;
        g_disk_base_sector = first;
        g_disk_partition_sectors = last - first + 1;
        return true;
    }
    return false;
}

static bool mount_disk_fs() {
    if (!g_blk_available || !g_blk_dma) return false;
    disk_cache_reset();
    (void)select_ameroot_partition();
    if (!disk_read_block(0, g_disk_block)) return false;
    const auto* sb = reinterpret_cast<const FsSuperblock*>(g_disk_block);
    if (sb->magic != FS_MAGIC ||
        sb->version != FS_SUPERBLOCK_VERSION ||
        sb->block_size != FS_BLOCK_SIZE ||
        sb->inode_table_root == 0 ||
        sb->total_blocks == 0) {
        return false;
    }
    g_disk_total_blocks = sb->total_blocks;
    if (g_disk_partition_sectors != 0 &&
        g_disk_total_blocks > g_disk_partition_sectors / kSectorsPerFsBlock) {
        return false;
    }
    g_disk_inode_root = sb->inode_table_root;
    g_disk_mounted = true;
    InodeLeafEntry root;
    zero_bytes(&root, sizeof(root));
    if (!disk_inode(kRootInode, &root) || root.type != FS_INODE_DIR) {
        g_disk_mounted = false;
        return false;
    }
    mochi::log_raw_marker("io.fs: mounted midix-fs-v1 disk\n");
    return true;
}

// ---------------------------------------------------------------------------
// Ordered-metadata sync hook. Kept small until the durable B+tree writer lands.
// ---------------------------------------------------------------------------
static mx_i32_t flush_block_device_if_available() {
    if (!g_blk_available) return MX_OK;
    BlkReply reply{};
    if (!g_blk_client.request(BLK_FLUSH, 0, 0, MX_NULL_HANDLE, 0, &reply,
                              mx_clock_ns() + kBlkReplyTimeoutNs)) {
        return static_cast<mx_i32_t>(MX_ERR_BUSY);
    }
    if (reply.status == static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED)) return MX_OK;
    return reply.status;
}

[[maybe_unused]] static mx_i32_t sync_ordered_metadata() {
    const mx_i32_t rc = verify_refcount_index();
    if (rc != MX_OK) return rc;
    if (!g_metadata_dirty) return MX_OK;
    // ext4 data=ordered shape: data pages are made durable before metadata is
    // declared clean; V1 keeps the metadata log tiny and relies on ordering.
    const mx_i32_t flush = flush_block_device_if_available();
    if (flush != MX_OK) return flush;
    g_metadata_dirty = false;
    return MX_OK;
}

// ---------------------------------------------------------------------------
// Boot wiring for the block backend and the optional disk mount.
// ---------------------------------------------------------------------------
void fs_disk_init(const MxBootInfo* boot) {
    mx_u32_t blk_req_order = kRingOrder;
    mx_u32_t blk_reply_order = kRingOrder;
    RingPage* blk_req = map_ring_from_boot(boot, "blk.req.ring", kVaBlkReqRing, &blk_req_order);
    RingPage* blk_reply = map_ring_from_boot(boot, "blk.reply.ring", kVaBlkReplyRing, &blk_reply_order);
    const CapHandle blk_req_db = mochi::boot::handle(boot, "blk.req.doorbell");
    const CapHandle blk_reply_db = mochi::boot::handle(boot, "blk.reply.doorbell");
    if (blk_req && blk_reply && blk_req_db != MX_NULL_HANDLE && blk_reply_db != MX_NULL_HANDLE) {
        g_blk_client = {{blk_req, blk_reply, blk_req_order, blk_req_db, blk_reply_db}};
        g_blk_available = g_blk_client.valid();
        mochi::log_raw_marker("io.fs: block endpoint ready\n");
    } else {
        g_blk_client = {};
        g_blk_available = false;
        mochi::log_raw_marker("io.fs: no block endpoint; running memory backend\n");
    }

    const mx_handle_t blk_dma = mochi::boot::handle(boot, "virtio.blk.dma");
    MxCapInfo dma_info{};
    if (blk_dma != MX_NULL_HANDLE &&
        mx_cap_info(blk_dma, &dma_info) == MX_OK &&
        dma_info.size >= BLK_DMA_CLIENT_OFFSET + kBlockSize &&
        mx_map_page(blk_dma, kVaBlkDma, MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_blk_dma = reinterpret_cast<mx_u8_t*>(kVaBlkDma);
        g_blk_dma_size = dma_info.size;
        if (!mount_disk_fs()) {
            mochi::log_raw_marker("io.fs: no valid midix-fs disk; memory backend only\n");
        }
    } else if (g_blk_available) {
        mochi::log_raw_marker("io.fs: no block dma arena; disk mount skipped\n");
    }
}
