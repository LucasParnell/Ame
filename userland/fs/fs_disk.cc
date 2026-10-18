// userland/fs/fs_disk.cc — midix-fs-v1 block transport and mount.
//
// On top of the block driver this provides a GPT scan for the AMEROOT
// partition, a small LRU block cache, and the B+tree lookups used by inode,
// extent, and directory reads. store.cc journals mutable /data metadata and
// allocates across the volume; handles keep the packaged root read-only.

#include "fs_internal.h"
#include "libk/endian.h"
#include "mochi/log.h"

constexpr mx_u16_t kBTreeInternal = 1;
constexpr mx_u16_t kBTreeLeaf = 2;
constexpr mx_u32_t kBTreeEntrySize = 64;          // every tree entry occupies one 64-byte slot
constexpr mx_u32_t kBTreeMaxEntries = (kBlockSize - sizeof(BTreeNodeHeader)) / kBTreeEntrySize;
constexpr mx_u32_t kBTreeMaxDepth = 8;            // bound on a single descent before giving up
constexpr mx_u32_t kBTreeInternalChildOffset = 8; // BTreeInternalEntry.child_block
constexpr mx_u32_t kDiskBlockCacheEntries = 32;

// GPT header (LBA 1) and partition-entry field offsets, from the UEFI spec.
constexpr char kGptSignature[9] = "EFI PART"; // 8 bytes, the trailing space included
constexpr mx_u32_t kGptHeaderSizeOffset = 12;
constexpr mx_u32_t kGptHeaderCrcOffset = 16;
constexpr mx_u32_t kGptEntriesLbaOffset = 72;
constexpr mx_u32_t kGptEntryCountOffset = 80;
constexpr mx_u32_t kGptEntrySizeOffset = 84;
constexpr mx_u32_t kGptEntriesCrcOffset = 88;
constexpr mx_u32_t kGptMinHeaderSize = 92;
constexpr mx_u32_t kGptMaxHeaderSize = 512;
constexpr mx_u32_t kGptMinEntrySize = 128;
constexpr mx_u32_t kGptMaxEntrySize = 512;
constexpr mx_u32_t kGptEntryTypeBytes = 16;
constexpr mx_u32_t kGptEntryFirstLbaOffset = 32;
constexpr mx_u32_t kGptEntryLastLbaOffset = 40;
constexpr mx_u32_t kGptEntryNameOffset = 56;
constexpr mx_u32_t kGptEntryNameBytes = 72;
constexpr char kAmerootPartitionName[] = "AMEROOT";

// Bound on one block-driver round trip. If drv.blk stops answering (crash or
// stall) io.fs must not wedge inside a disk read; the request instead fails and
// the caller degrades to the memory backend / an error reply.
constexpr mx_u64_t kBlkReplyTimeoutNs = 1'000'000'000ull;

static mochi::fs::BlkClient g_blk_client{};
static bool g_blk_available = false;
static mx_u8_t* g_blk_dma = nullptr;
static mx_u64_t g_blk_dma_size = 0;
static mx_u64_t g_disk_base_sector = 0;
static mx_u64_t g_disk_partition_sectors = 0;
static mx_u64_t g_disk_total_blocks = 0;
static mx_u64_t g_disk_inode_root = 0;
// Reserved region geometry from the superblock, cached at mount.
static mx_u64_t g_disk_journal_start = 0;
static mx_u64_t g_disk_journal_blocks = 0;
static mx_u64_t g_disk_first_data = 0;
// Probe identities without dragging 4 KiB payloads through the CPU cache.
static mx_u64_t g_cache_blocks[kDiskBlockCacheEntries];
static mx_u64_t g_cache_ages[kDiskBlockCacheEntries];
static mx_u32_t g_cache_valid;
alignas(4096) static mx_u8_t g_cache_data[kDiskBlockCacheEntries][kBlockSize];
static mx_u64_t g_disk_cache_clock = 1;
alignas(4096) static mx_u8_t g_gpt_entries[64 * 1024];
alignas(4096) static mx_u8_t g_disk_block[kBlockSize];

static bool g_disk_mounted = false;

bool disk_mounted() {
    return g_disk_mounted;
}

// ---------------------------------------------------------------------------
// Block cache.
// ---------------------------------------------------------------------------
static bool disk_cache_lookup(mx_u64_t block, mx_u8_t* out) {
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        if ((g_cache_valid & (1u << i)) && g_cache_blocks[i] == block) {
            g_cache_ages[i] = ++g_disk_cache_clock;
            libk::copy_exact(libk::as_bytes(out, kBlockSize), libk::as_bytes(g_cache_data[i]));
            return true;
        }
    }
    return false;
}
static void disk_cache_store(mx_u64_t block, const mx_u8_t* data) {
    mx_u32_t victim = 0;
    bool empty = false;
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        if ((g_cache_valid & (1u << i)) && g_cache_blocks[i] == block) { victim = i; break; }
        if (!(g_cache_valid & (1u << i))) { if (!empty) victim = i; empty = true; }
        else if (!empty && g_cache_ages[i] < g_cache_ages[victim]) victim = i;
    }
    g_cache_blocks[victim] = block;
    g_cache_ages[victim] = ++g_disk_cache_clock;
    libk::copy_exact(libk::as_bytes(g_cache_data[victim]), libk::as_bytes(data, kBlockSize));
    g_cache_valid |= 1u << victim;
}
static void disk_cache_invalidate(mx_u64_t block) {
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i)
        if ((g_cache_valid & (1u << i)) && g_cache_blocks[i] == block) g_cache_valid &= ~(1u << i);
}
static void disk_cache_reset() {
    g_cache_valid = 0;
    g_disk_cache_clock = 1;
}
// An unanswered command may still own the shared transfer arena. Never reuse
// it or accept an uncorrelated late reply after the bounded wait expires.
static bool block_request(mx_u32_t type, mx_u64_t sector, mx_u32_t count, BlkReply* reply) {
    if (!g_blk_available) return false;
    if (!g_blk_client.request(type, sector, count, MX_NULL_HANDLE,
                             type == BLK_FLUSH ? 0 : BLK_DMA_CLIENT_OFFSET,
                             reply, mx_clock_ns() + kBlkReplyTimeoutNs)) {
        g_blk_available = false;
        disk_cache_reset();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Block device access.
// ---------------------------------------------------------------------------
static bool disk_read_sectors(mx_u64_t sector, mx_u32_t sector_count, mx_u8_t* out) {
    if (!g_blk_available || !g_blk_dma || !out || sector_count == 0) {
        return false;
    }
    const mx_u64_t byte_count = static_cast<mx_u64_t>(sector_count) * kSectorSize;
    if (g_blk_dma_size < BLK_DMA_CLIENT_OFFSET ||
        byte_count > g_blk_dma_size - BLK_DMA_CLIENT_OFFSET) {
        return false;
    }
    BlkReply reply{};
    if (!block_request(BLK_READ, sector, sector_count, &reply)) {
        return false;
    }
    if (reply.status != MX_OK || reply.sectors_transferred != sector_count) {
        return false;
    }
    libk::copy_exact(libk::as_bytes(out, byte_count),
                     libk::as_bytes(g_blk_dma + BLK_DMA_CLIENT_OFFSET, byte_count));
    return true;
}

// Resolve an fs block inside the mounted partition without wrapping either
// multiplication or addition. Also used before a cache lookup.
static bool disk_sector_for_block(mx_u64_t block, mx_u64_t* sector) {
    if (g_disk_total_blocks != 0 && block >= g_disk_total_blocks) return false;
    mx_u64_t relative = 0;
    if (!libk::checked_mul(block, kSectorsPerFsBlock, &relative)) return false;
    if (g_disk_partition_sectors != 0 &&
        (relative > g_disk_partition_sectors ||
         kSectorsPerFsBlock > g_disk_partition_sectors - relative)) return false;
    return libk::checked_add(g_disk_base_sector, relative, sector) &&
           libk::checked_add(*sector, kSectorsPerFsBlock - 1, nullptr);
}

// Write one fs block through the block service. Mirrors disk_read_block's
// bounds checks so a durable write can never leave the mounted partition, and
// keeps the read cache coherent with the new contents.
bool disk_write_blocks(mx_u64_t block, mx_u32_t count, const void* input) {
    if (!g_blk_available || !g_blk_dma || !input || !count || count > 16 ||
        block > ~mx_u64_t{0} - (count - 1)) return false;
    mx_u64_t sector = 0, last = 0;
    if (!disk_sector_for_block(block, &sector) ||
        !disk_sector_for_block(block + count - 1, &last)) return false;
    const auto bytes = static_cast<mx_u64_t>(count) * kBlockSize;
    if (!libk::copy_exact(libk::slice(libk::as_bytes(g_blk_dma, g_blk_dma_size),
                                     BLK_DMA_CLIENT_OFFSET, bytes), libk::as_bytes(input, bytes))) return false;
    // Invalidate before submission, including every block in a partial or
    // ambiguous write. Old cached bytes can never conceal a failed transfer.
    for (mx_u32_t i = 0; i < count; ++i) disk_cache_invalidate(block + i);
    BlkReply reply{};
    if (!block_request(BLK_WRITE, sector, count * kSectorsPerFsBlock, &reply) ||
        reply.status != MX_OK || reply.sectors_transferred != count * kSectorsPerFsBlock) return false;
    const auto* data = static_cast<const mx_u8_t*>(input);
    for (mx_u32_t i = 0; i < count; ++i) disk_cache_store(block + i, data + i * kBlockSize);
    return true;
}
// Device-level read accounting for the mount phases: cache hits are excluded,
// so this counts real round trips to the block service.
mx_u64_t g_dev_reads = 0;
mx_u64_t g_dev_read_ns = 0;

bool disk_write_block(mx_u64_t block, const mx_u8_t* data) {
    return disk_write_blocks(block, 1, data);
}
bool disk_read_blocks(mx_u64_t block, mx_u32_t count, void* out) {
    if (!out || !count || count > 16 || block > ~mx_u64_t{0} - (count - 1)) return false;
    auto* data = static_cast<mx_u8_t*>(out);
    if (count == 1) return disk_read_block(block, data);
    mx_u64_t sector = 0, last = 0;
    if (!disk_sector_for_block(block, &sector) ||
        !disk_sector_for_block(block + count - 1, &last) ||
        !disk_read_sectors(sector, count * kSectorsPerFsBlock, data)) return false;
    for (mx_u32_t i = 0; i < count; ++i) disk_cache_store(block + i, data + i * kBlockSize);
    return true;
}

// Each 4 KiB block otherwise costs a full request/reply round trip to the block
// service, so contiguous metadata is fetched one request deep.
alignas(4096) static mx_u8_t g_readahead[16 * kBlockSize];
static void disk_readahead(mx_u64_t first, mx_u32_t count) {
    if (!g_blk_available || !g_blk_dma || !count) return;
    if (count > 16) count = 16;
    const mx_u64_t started = mx_clock_ns();
    const bool ok = disk_read_blocks(first, count, g_readahead);
    g_dev_read_ns += mx_clock_ns() - started;
    ++g_dev_reads;
    (void)ok;  // A failed read-ahead only means the caller reads block by block.
}

bool disk_read_block(mx_u64_t block, mx_u8_t* out) {
    if (!g_blk_available || !g_blk_dma || !out) {
        return false;
    }
    mx_u64_t sector = 0;
    if (!disk_sector_for_block(block, &sector)) return false;
    if (disk_cache_lookup(block, out)) {
        return true;
    }
    const mx_u64_t read_started = mx_clock_ns();
    const bool read_ok = disk_read_sectors(sector, kSectorsPerFsBlock, out);
    g_dev_read_ns += mx_clock_ns() - read_started;
    ++g_dev_reads;
    if (!read_ok) {
        return false;
    }
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
        return libk::load_le<mx_u32_t>(entry);
    case DiskTree::Extent:
    case DiskTree::Directory:
        return libk::load_le<mx_u64_t>(entry);
    }
    return 0;
}

enum class DiskLookup { Found, Missing, Invalid };

static DiskLookup disk_btree_find(mx_u64_t root, mx_u64_t key, DiskTree tree,
                            mx_u8_t out_entry[kBTreeEntrySize]) {
    if (!root) return DiskLookup::Missing;
    mx_u64_t block = root;
    for (mx_u32_t depth = 0; depth < kBTreeMaxDepth; ++depth) {
        if (block == 0 || !disk_read_block(block, g_disk_block)) {
            return DiskLookup::Invalid;
        }
        const auto* header = reinterpret_cast<const BTreeNodeHeader*>(g_disk_block);
        if (header->entry_count > kBTreeMaxEntries) {
            return DiskLookup::Invalid;
        }
        if (header->node_type == kBTreeInternal) {
            mx_u64_t child = 0;
            const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
            for (mx_u32_t i = 0; i < header->entry_count; ++i) {
                const mx_u8_t* entry = entries + (i * kBTreeEntrySize);
                const mx_u64_t max_key = libk::load_le<mx_u64_t>(entry);
                if (key <= max_key) {
                    child = libk::load_le<mx_u64_t>(entry + kBTreeInternalChildOffset);
                    if (!child) return DiskLookup::Invalid;
                    break;
                }
            }
            if (child == 0) {
                return DiskLookup::Missing;
            }
            block = child;
            continue;
        }
        if (header->node_type != kBTreeLeaf) {
            return DiskLookup::Invalid;
        }

        const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
        for (mx_u32_t i = 0; i < header->entry_count; ++i) {
            const mx_u8_t* entry = entries + (i * kBTreeEntrySize);
            const mx_u64_t entry_key = disk_entry_key(entry, tree);
            if (entry_key == key) {
                libk::copy_exact(libk::as_bytes(out_entry, kBTreeEntrySize),
                                 libk::as_bytes(entry, kBTreeEntrySize));
                return DiskLookup::Found;
            }
            if (entry_key > key) {
                return DiskLookup::Missing;
            }
        }
        return DiskLookup::Missing;
    }
    return DiskLookup::Invalid;
}

bool disk_inode(mx_u32_t inode_number, InodeLeafEntry* out) {
    if (!g_disk_mounted || g_volume.poisoned || !out) {
        return false;
    }
    alignas(8) mx_u8_t entry[kBTreeEntrySize];
    const auto root = g_volume.mounted ? g_volume.checkpoint.inode_table_root : g_disk_inode_root;
    if (disk_btree_find(root, inode_number, DiskTree::Inode, entry) != DiskLookup::Found) {
        return false;
    }
    libk::copy_exact(libk::as_bytes(out, sizeof(*out)), libk::as_bytes(entry));
    return out->inode_number == inode_number;
}

bool disk_dir_lookup(const InodeLeafEntry& dir, mx_u32_t parent_inode, const char* name,
                     mx_u8_t name_len, InodeLeafEntry* out_inode) {
    if (dir.type != FS_INODE_DIR || name_len == 0 || name_len > kMaxName || !name || !out_inode) {
        return false;
    }
    if (g_volume.mounted) return mochi::store::lookup(g_volume, parent_inode, name, name_len, *out_inode) == MX_OK;
    alignas(8) mx_u8_t entry[kBTreeEntrySize];
    const mx_u64_t key = (static_cast<mx_u64_t>(parent_inode) << 32) | name_hash32(name, name_len);
    if (disk_btree_find(dir.extent_tree_root, key, DiskTree::Directory, entry) != DiskLookup::Found) {
        return false;
    }
    const auto* dirent = reinterpret_cast<const DirLeafEntry*>(entry);
    if (dirent->compound_key != key || dirent->name_len != name_len) {
        return false;
    }
    for (mx_u32_t i = 0; i < name_len; ++i) {
        if (dirent->name[i] != name[i]) {
            return false;
        }
    }
    return disk_inode(dirent->child_inode, out_inode);
}

static DiskLookup disk_btree_nth_entry(mx_u64_t root, mx_u32_t target, mx_u32_t* cursor,
                                       mx_u8_t out_entry[kBTreeEntrySize]) {
    if (!cursor || !out_entry) return DiskLookup::Invalid;
    if (!root) return DiskLookup::Missing;
    struct Frame { mx_u64_t block; mx_u32_t next_child; };
    Frame path[kBTreeMaxDepth]{{root, 0}};
    mx_u32_t depth = 1;
    mx_u64_t visits = 0;
    while (depth) {
        auto& frame = path[depth - 1];
        // Reload the parent after descending: g_disk_block is shared scratch,
        // so pointers into it cannot survive a child read.
        if (!disk_read_block(frame.block, g_disk_block)) return DiskLookup::Invalid;
        const auto& header = *reinterpret_cast<const BTreeNodeHeader*>(g_disk_block);
        if (header.entry_count > kBTreeMaxEntries) return DiskLookup::Invalid;
        const auto* entries = g_disk_block + sizeof(BTreeNodeHeader);
        if (header.node_type == kBTreeLeaf) {
            for (mx_u32_t i = 0; i < header.entry_count; ++i) {
                if (*cursor == target) {
                    (void)libk::copy_exact(libk::as_bytes(out_entry, kBTreeEntrySize),
                                           libk::as_bytes(entries + i * kBTreeEntrySize, kBTreeEntrySize));
                    return DiskLookup::Found;
                }
                if (*cursor == ~mx_u32_t{0}) return DiskLookup::Invalid;
                ++*cursor;
            }
            --depth;
        } else if (header.node_type == kBTreeInternal) {
            if (frame.next_child == header.entry_count) { --depth; continue; }
            const auto child = libk::load_le<mx_u64_t>(
                entries + frame.next_child++ * kBTreeEntrySize + kBTreeInternalChildOffset);
            if (!child || depth == kBTreeMaxDepth || ++visits > g_disk_total_blocks) {
                return DiskLookup::Invalid;
            }
            for (mx_u32_t i = 0; i < depth; ++i) {
                if (path[i].block == child) return DiskLookup::Invalid;
            }
            path[depth++] = {child, 0};
        } else return DiskLookup::Invalid;
    }
    return DiskLookup::Missing;
}

// ---------------------------------------------------------------------------
// Disk-backed file/dir reads.
// ---------------------------------------------------------------------------
mx_i32_t disk_read_file(const OpenHandle& handle, mx_u64_t offset, mx_u32_t length,
                        FsReadReply* reply) {
    if (!reply) {
        return static_cast<mx_i32_t>(MX_ERR_INVALID);
    }
    if (handle.type != FS_INODE_FILE) {
        return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    }
    if (length > FS_INLINE_READ_MAX) {
        return static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED);
    }
    if (g_volume.mounted) return mochi::store::read(g_volume, handle.inode_number, offset,
                                                    reply->inline_data, length, reply->length_read);
    if (offset >= handle.size) {
        reply->length_read = 0;
        return MX_OK;
    }
    const auto available = handle.size - offset;
    const auto remaining = static_cast<mx_u32_t>(available < FS_INLINE_READ_MAX ? available : FS_INLINE_READ_MAX);
    const mx_u32_t n = min_u32(length, min_u32(remaining, FS_INLINE_READ_MAX));
    mx_u32_t copied = 0;
    while (copied < n) {
        const mx_u64_t abs = offset + copied;
        const mx_u64_t logical_block = abs / kBlockSize;
        const mx_u32_t block_off = static_cast<mx_u32_t>(abs % kBlockSize);
        const mx_u32_t chunk = min_u32(n - copied, kBlockSize - block_off);
        alignas(8) mx_u8_t entry[kBTreeEntrySize];
        const auto lookup = disk_btree_find(handle.tree_root, logical_block, DiskTree::Extent, entry);
        if (lookup == DiskLookup::Invalid) return MX_ERR_FAULT;
        if (lookup == DiskLookup::Missing) {
            libk::zero(reply->inline_data + copied, chunk);
            copied += chunk;
            continue;
        }
        const auto* extent = reinterpret_cast<const ExtentLeafEntry*>(entry);
        if (extent->logical_block != logical_block || extent->phys_block >= g_disk_total_blocks ||
            !disk_read_block(extent->phys_block, g_disk_block)) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
        if (extent->data_crc32 != 0 && libk::crc32_ieee(libk::as_bytes(g_disk_block, kBlockSize)) != extent->data_crc32) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
        libk::copy_n(libk::slice(libk::as_bytes(reply->inline_data), copied, chunk),
                     libk::slice(libk::as_bytes(g_disk_block), block_off, chunk), chunk);
        copied += chunk;
    }
    reply->length_read = n;
    return MX_OK;
}

mx_i32_t disk_readdir(const OpenHandle& handle, const FsReaddirRequest& req,
                      FsReaddirReply* reply) {
    if (!reply) {
        return static_cast<mx_i32_t>(MX_ERR_INVALID);
    }
    if (handle.type != FS_INODE_DIR) {
        return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    }
    mx_u32_t cursor = 0;
    alignas(8) mx_u8_t entry[kBTreeEntrySize];
    const auto lookup = disk_btree_nth_entry(handle.tree_root, req.cookie, &cursor, entry);
    if (lookup == DiskLookup::Invalid) return MX_ERR_FAULT;
    if (lookup == DiskLookup::Missing) {
        reply->flags = FS_READDIR_EOF;
        reply->entry_count = cursor;
        return MX_OK;
    }
    const auto* dirent = reinterpret_cast<const DirLeafEntry*>(entry);
    if (dirent->name_len > sizeof(dirent->name) || req.cookie == ~mx_u32_t{0}) return MX_ERR_FAULT;
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
    for (mx_u32_t i = 0; i < n; ++i) {
        reply->name[i] = dirent->name[i];
    }
    mx_u32_t after = 0;
    mx_u8_t ignored[kBTreeEntrySize];
    const auto next = disk_btree_nth_entry(handle.tree_root, req.cookie + 1, &after, ignored);
    if (next == DiskLookup::Invalid) return MX_ERR_FAULT;
    if (next == DiskLookup::Missing) {
        reply->flags |= FS_READDIR_EOF;
    }
    reply->entry_count = after;
    return MX_OK;
}

// ---------------------------------------------------------------------------
// Handle allocation for disk-backed inodes. These are always read-only.
// ---------------------------------------------------------------------------
fs_handle_t alloc_disk_handle(const InodeLeafEntry& inode) {
    for (mx_u32_t i = kFirstClientHandle; i < kMaxHandles; ++i) {
        if (g_handles[i].used || g_handle_generations[i] == kMaxHandleGeneration) {
            continue;
        }
        OpenHandle& h = g_handles[i];
        libk::zero(h);
        h.used = true;
        h.disk_backed = true;
        h.inode_number = inode.inode_number;
        h.access = fs_handle_access(inode.type, false);
        h.type = inode.type;
        h.inode_class = inode.inode_class;
        h.size = inode.size;
        h.tree_root = inode.extent_tree_root;
        return handle_token(i);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Mount: GPT scan + superblock validation.
// ---------------------------------------------------------------------------
// Compare a GPT UTF-16LE partition name against an ASCII literal. The name
// field is fixed at kGptEntryNameBytes bytes; a name that fills it has no
// terminator to require.
static bool gpt_name_eq(const mx_u8_t* utf16le, const char* ascii) {
    mx_u32_t i = 0;
    for (; ascii[i]; ++i) {
        if (i * 2 + 1 >= kGptEntryNameBytes) {
            return false;
        }
        if (utf16le[i * 2] != static_cast<mx_u8_t>(ascii[i]) || utf16le[i * 2 + 1] != 0) {
            return false;
        }
    }
    if (i * 2 + 1 >= kGptEntryNameBytes) {
        return true;
    }
    return utf16le[i * 2] == 0 && utf16le[i * 2 + 1] == 0;
}

static bool select_ameroot_partition() {
    g_disk_base_sector = 0;
    g_disk_partition_sectors = 0;
    if (!disk_read_sectors(1, 1, g_disk_block)) {
        return false;
    }
    for (mx_u32_t i = 0; i < sizeof(kGptSignature) - 1; ++i) {
        if (g_disk_block[i] != static_cast<mx_u8_t>(kGptSignature[i])) {
            return false;
        }
    }

    const mx_u32_t header_size = libk::load_le<mx_u32_t>(g_disk_block + kGptHeaderSizeOffset);
    const mx_u32_t header_crc = libk::load_le<mx_u32_t>(g_disk_block + kGptHeaderCrcOffset);
    if (header_size < kGptMinHeaderSize || header_size > kGptMaxHeaderSize) {
        return false;
    }
    libk::zero(libk::slice(libk::as_bytes(g_disk_block), kGptHeaderCrcOffset, sizeof(header_crc)));
    if (libk::crc32_ieee(libk::as_bytes(g_disk_block, header_size)) != header_crc) {
        return false;
    }

    const mx_u64_t entries_lba = libk::load_le<mx_u64_t>(g_disk_block + kGptEntriesLbaOffset);
    const mx_u32_t entry_count = libk::load_le<mx_u32_t>(g_disk_block + kGptEntryCountOffset);
    const mx_u32_t entry_size = libk::load_le<mx_u32_t>(g_disk_block + kGptEntrySizeOffset);
    const mx_u32_t entries_crc = libk::load_le<mx_u32_t>(g_disk_block + kGptEntriesCrcOffset);
    if (entry_count == 0 || entry_size < kGptMinEntrySize || entry_size > kGptMaxEntrySize) {
        return false;
    }
    const mx_u64_t entries_bytes64 = static_cast<mx_u64_t>(entry_count) * entry_size;
    if (entries_bytes64 == 0 || entries_bytes64 > sizeof(g_gpt_entries)) {
        return false;
    }
    const mx_u32_t entries_bytes = static_cast<mx_u32_t>(entries_bytes64);
    const mx_u32_t entries_sectors =
        static_cast<mx_u32_t>((entries_bytes64 + kSectorSize - 1) / kSectorSize);
    if (!disk_read_sectors(entries_lba, entries_sectors, g_gpt_entries)) {
        return false;
    }
    if (libk::crc32_ieee(libk::as_bytes(g_gpt_entries, entries_bytes)) != entries_crc) {
        return false;
    }

    for (mx_u32_t i = 0; i < entry_count; ++i) {
        const mx_u8_t* entry = g_gpt_entries + static_cast<mx_u64_t>(i) * entry_size;
        bool empty = true;
        for (mx_u32_t b = 0; b < kGptEntryTypeBytes; ++b) {
            empty = empty && entry[b] == 0;
        }
        if (empty) {
            continue;
        }
        if (!gpt_name_eq(entry + kGptEntryNameOffset, kAmerootPartitionName)) {
            continue;
        }
        const mx_u64_t first = libk::load_le<mx_u64_t>(entry + kGptEntryFirstLbaOffset);
        const mx_u64_t last = libk::load_le<mx_u64_t>(entry + kGptEntryLastLbaOffset);
        if (first == 0 || last < first) {
            return false;
        }
        g_disk_base_sector = first;
        g_disk_partition_sectors = last - first + 1;
        return true;
    }
    return false;
}

static bool mount_disk_fs() {
    if (!g_blk_available || !g_blk_dma) {
        return false;
    }
    const mx_u64_t mount_started = mx_clock_ns();
    g_dev_reads = 0;
    g_dev_read_ns = 0;
    disk_cache_reset();
    (void)select_ameroot_partition();
    // Superblock, checkpoint and the start of the journal are contiguous.
    disk_readahead(0, 16);
    if (!disk_read_block(0, g_disk_block)) {
        return false;
    }
    const auto* sb = reinterpret_cast<const FsSuperblock*>(g_disk_block);
    if (sb->magic != FS_MAGIC || sb->version != FS_SUPERBLOCK_VERSION ||
        sb->block_size != FS_BLOCK_SIZE || sb->inode_table_root == 0 || sb->total_blocks == 0) {
        return false;
    }
    g_disk_total_blocks = sb->total_blocks;
    if (g_disk_partition_sectors != 0 &&
        g_disk_total_blocks > g_disk_partition_sectors / kSectorsPerFsBlock) {
        return false;
    }
    g_disk_inode_root = sb->inode_table_root;
    g_disk_journal_start = sb->journal_start_block;
    g_disk_journal_blocks = sb->journal_block_count;
    g_disk_first_data = sb->first_data_block;
    const mochi::store::Io io{
        nullptr,
        [](void*, mx_u64_t first, mx_u32_t count, void* out) { return disk_read_blocks(first, count, out); },
        [](void*, mx_u64_t first, mx_u32_t count, const void* data) { return disk_write_blocks(first, count, data); },
        [](void*) { return disk_flush() == MX_OK; }};
    mochi::log_counter("io.fs.mount_super_ns", mx_clock_ns() - mount_started);
    const mx_u64_t store_started = mx_clock_ns();
    if (!mochi::store::mount(g_volume, io) && g_volume.error != MX_ERR_NOT_IMPLEMENTED) {
        mochi::log_raw_marker("io.fs: disk recovery failed; volume not writable\n");
        g_volume.mounted = false;
        return false;
    }
    mochi::log_counter("io.fs.store_mount_ns", mx_clock_ns() - store_started);
    mochi::log_counter("io.fs.mount_dev_reads", g_dev_reads);
    mochi::log_counter("io.fs.mount_dev_read_ns", g_dev_read_ns);
    if (g_volume.mounted) {
        g_disk_inode_root = g_volume.checkpoint.inode_table_root;
        mochi::log_raw_marker("io.fs: full-volume /data allocator and journal ready\n");
    }
    g_disk_mounted = true;
    InodeLeafEntry root;
    libk::zero(root);
    if (!disk_inode(kRootInode, &root) || root.type != FS_INODE_DIR) {
        g_disk_mounted = false;
        g_volume.mounted = false;
        return false;
    }
    mochi::log_raw_marker("io.fs: mounted midix-fs-v1 disk\n");
    return true;
}

// ---------------------------------------------------------------------------
// Device flush + durable-region access, used by the fs_durable write path.
// ---------------------------------------------------------------------------
mx_i32_t disk_flush() {
    if (!g_blk_available) {
        return static_cast<mx_i32_t>(MX_ERR_SERVICE_UNAVAILABLE);
    }
    BlkReply reply{};
    if (!block_request(BLK_FLUSH, 0, 0, &reply)) {
        return static_cast<mx_i32_t>(MX_ERR_BUSY);
    }
    return reply.status;
}

// The durable user region is the disk's reserved journal area [journal_start,
// journal_start + journal_blocks). It is only exposed when it is provably
// disjoint from every block the read-only root allocates (all root blocks live
// at or after first_data_block), so replaying it can never corrupt the signed
// image.
bool disk_durable_region(mx_u64_t* start_block, mx_u64_t* block_count) {
    if (!g_disk_mounted) {
        return false;
    }
    if (g_disk_journal_blocks < kMinDurableRegionBlocks) {
        return false;
    }
    if (g_disk_journal_start < 2) {
        return false; // never overlap the superblock/checkpoint
    }
    mx_u64_t end = 0;
    if (!libk::checked_add(g_disk_journal_start, g_disk_journal_blocks, &end) ||
        end > g_disk_total_blocks) {
        return false;
    }
    if (g_disk_first_data == 0) {
        return false;
    }
    if (end > g_disk_first_data) {
        return false;
    }
    if (start_block) {
        *start_block = g_disk_journal_start;
    }
    if (block_count) {
        *block_count = g_disk_journal_blocks;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Boot wiring for the block backend and the optional disk mount.
// ---------------------------------------------------------------------------
void fs_disk_init(const MxBootInfo* boot) {
    g_blk_client.ep = mochi::boot::map_endpoint(boot,
        {"blk.req.ring", kVaBlkReqRing, "blk.req.doorbell"},
        {"blk.reply.ring", kVaBlkReplyRing, "blk.reply.doorbell"});
    g_blk_available = g_blk_client.valid();
    mochi::log_raw_marker(g_blk_available ? "io.fs: block endpoint ready\n"
        : "io.fs: no block endpoint; running memory backend\n");

    mx_handle_t blk_dma = mochi::boot::handle(boot, "blk.buffer");
    if (blk_dma == MX_NULL_HANDLE) blk_dma = mochi::boot::handle(boot, "virtio.blk.dma");
    MxCapInfo dma_info{};
    if (blk_dma != MX_NULL_HANDLE && mx_cap_info(blk_dma, &dma_info) == MX_OK &&
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
