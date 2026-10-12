// userland/fs/fs_durable.cc — durable /data store backed by the disk's
// reserved region.
//
// The disk root stays READ-ONLY: io.fs never mutates the signed image's B+tree.
// Durability is delivered by a small, self-describing log serialised into the
// reserved journal region of the mounted midix-fs-v1 partition
// ([journal_start_block, +journal_block_count)); that region is provably
// disjoint from every block the read-only root allocates (see
// disk_durable_region()). The whole /data subtree is written back through the
// block service with BLK_WRITE and ordered by BLK_FLUSH barriers.
//
// On-disk layout (offsets are fs blocks relative to the region start):
//
//   block 0          DurableHeader   magic/version/state/content_blocks/crc
//   block 1..        FileRecord      one metadata block per inode, followed by
//                                    ceil(size/block) raw data blocks
//
// Commit protocol: persist a Dirty header before replacing records, flush the
// records, then persist a Committed header. An interrupted sync may lose the
// previous checkpoint; it must never expose a partially written new tree.
// This single-region format detects interrupted updates, not atomic rollback.
//
// Non-durable mode: when no usable region exists (memory backend, or a disk
// whose reserved region is missing/unsafe) durable_available() is false, /data
// is an ordinary in-memory volume, and FS_SYNC reports MX_ERR_NOT_IMPLEMENTED.
//
// Stack discipline: every on-disk block, and every table sized like one (the
// record enumeration and the replay slot table), lives in a file-scope static.
// io.fs has a small user stack, so no 4096-byte record/header may ever live in
// a stack frame.

#include "fs_internal.h"
#include "mochi/log.h"

constexpr mx_u32_t kDurableHeaderMagic = 0x44555231u; // 'DUR1'
constexpr mx_u32_t kDurableRecordMagic = 0x414D4546u; // 'AMEF'
constexpr mx_u32_t kDurableVersion = 1u;
constexpr mx_u32_t kDurableStateDirty = 0u;
constexpr mx_u32_t kDurableStateCommitted = 1u;
constexpr mx_u32_t kDurableMaxRecords = kMaxInodes;
// Region-relative block geometry: the header occupies block 0 and each record is
// one metadata block followed by its file's data run.
constexpr mx_u64_t kDurableHeaderBlock = 0;
constexpr mx_u64_t kDurableHeaderBlocks = 1;
constexpr mx_u64_t kDurableFirstRecordBlock = kDurableHeaderBlock + kDurableHeaderBlocks;
constexpr mx_u64_t kDurableRecordMetaBlocks = 1;

struct DurableHeader {
    mx_u32_t magic;
    mx_u32_t version;
    mx_u32_t state;
    mx_u32_t reserved;
    mx_u64_t generation;
    mx_u32_t content_blocks; // region blocks used after the header
    mx_u32_t header_crc32;   // crc32 over this header with the field zeroed
    mx_u8_t pad[kBlockSize - 32];
};
static_assert(sizeof(DurableHeader) == kBlockSize, "durable header must be one block");

struct DurableRecord {
    mx_u32_t magic;
    mx_u32_t name_len;
    mx_u16_t type;
    mx_u16_t inode_class;
    mx_u32_t legacy_mode; // ignored; retained for checkpoint layout compatibility
    mx_u32_t parent_slot; // record index of the parent; 0 is the /data root
    mx_u32_t reserved0;   // explicit padding keeps every offset deterministic
    mx_u64_t size;
    mx_u64_t data_blocks;
    mx_u32_t data_crc32;   // crc32 folded over every data block
    mx_u32_t record_crc32; // crc32 over this record with the field zeroed
    char name[kNameBytes];
    mx_u8_t pad[kBlockSize - 96];
};
static_assert(sizeof(DurableRecord) == kBlockSize, "durable record must be one block");

// Region geometry, resolved once from the mounted superblock.
static mx_u64_t g_region_start = 0;
static mx_u64_t g_region_blocks = 0;
static bool g_region_ready = false;

alignas(kBlockSize) static mx_u8_t g_data_stage[kBlockSize];
alignas(kBlockSize) static mx_u8_t g_record_block[kBlockSize];
alignas(kBlockSize) static mx_u8_t g_header_block[kBlockSize];

bool durable_available() {
    if (g_region_ready) {
        return true;
    }
    if (!disk_mounted()) {
        return false;
    }
    mx_u64_t start = 0;
    mx_u64_t count = 0;
    if (!disk_durable_region(&start, &count)) {
        return false;
    }
    g_region_start = start;
    g_region_blocks = count;
    g_region_ready = true;
    return true;
}

static bool region_read_block(mx_u64_t rel, mx_u8_t* out) {
    if (!g_region_ready || rel >= g_region_blocks || !out) {
        return false;
    }
    return disk_read_block(g_region_start + rel, out);
}

static bool region_write_block(mx_u64_t rel, const mx_u8_t* data) {
    if (!g_region_ready || rel >= g_region_blocks || !data) {
        return false;
    }
    return disk_write_block(g_region_start + rel, data);
}

static mx_u64_t blocks_for_size(mx_u64_t size) {
    return libk::div_round_up(size, kBlockSize);
}

// ---------------------------------------------------------------------------
// Enumeration: breadth-first over the live /data subtree so a parent's record
// index is always lower than its children's (parent_slot refers back to an earlier record).
// ---------------------------------------------------------------------------
struct DurableEntry {
    InodeState* inode;
    mx_u32_t parent_slot;
};

// Enumerated /data records (sync) and the replay slot table (load). File-scope
// statics like the staging buffers above: io.fs has a small user stack, and
// each is about a kilobyte.
static DurableEntry g_entries[kDurableMaxRecords];
static InodeState* g_record_slots[kDurableMaxRecords];
static mx_u16_t g_record_types[kDurableMaxRecords];

static mx_u32_t enumerate_data(DurableEntry out[kDurableMaxRecords]) {
    InodeState* root = live_inode(g_data_inode);
    if (!root || root->type != FS_INODE_DIR) {
        return 0;
    }
    mx_u32_t count = 0;
    out[count].inode = root;
    out[count].parent_slot = 0;
    ++count;
    for (mx_u32_t i = 0; i < count; ++i) {
        const mx_u32_t parent = out[i].inode->inode_number;
        for (mx_u32_t s = 0; s < kMaxInodes; ++s) {
            InodeState& in = g_inodes[s];
            if (!in.used || in.inode_number == kRootInode || in.inode_number == parent) {
                continue;
            }
            if (in.parent_inode != parent) {
                continue;
            }
            if (count >= kDurableMaxRecords) {
                return count;
            }
            out[count].inode = &in;
            out[count].parent_slot = i;
            ++count;
        }
    }
    return count;
}

// Copy one logical data block of a file into `out` (zeros for a hole).
static void read_logical_block(const InodeState& inode, mx_u64_t logical_block, mx_u8_t* out) {
    const ExtentState* extent = find_extent_const(inode, logical_block);
    if (extent && extent->phys_block < kMaxPhysBlocks) {
        libk::copy_exact(libk::as_bytes(out, kBlockSize), libk::as_bytes(g_data_blocks[extent->phys_block]));
    } else {
        libk::zero(out, kBlockSize);
    }
}

// Populate the record staging block for one inode. `data_crc32` is left zero
// for the caller to fill once the data run is known; the record CRC is always
// (re)computed over the whole block by finalize_record().
static DurableRecord& fill_record(const DurableEntry& entry) {
    auto& rec = *reinterpret_cast<DurableRecord*>(g_record_block);
    libk::zero(rec);
    const InodeState& inode = *entry.inode;
    rec.magic = kDurableRecordMagic;
    rec.type = inode.type;
    rec.inode_class = inode.inode_class;
    rec.parent_slot = entry.parent_slot;
    rec.size = (inode.type == FS_INODE_FILE) ? inode.size : 0;
    rec.data_blocks = (inode.type == FS_INODE_FILE) ? blocks_for_size(inode.size) : 0;
    const mx_u32_t name_len = str_len(inode.name, kMaxName);
    rec.name_len = name_len;
    copy_name(rec.name, inode.name, static_cast<mx_u8_t>(name_len));
    return rec;
}

static void finalize_record(DurableRecord& rec) {
    rec.record_crc32 = 0;
    rec.record_crc32 = libk::crc32_ieee(libk::as_bytes(reinterpret_cast<const mx_u8_t*>(&rec), sizeof(rec)));
}

static bool write_header(mx_u32_t state, mx_u32_t content_blocks) {
    auto& header = *reinterpret_cast<DurableHeader*>(g_header_block);
    libk::zero(header);
    header.magic = kDurableHeaderMagic;
    header.version = kDurableVersion;
    header.state = state;
    header.generation = g_generation;
    header.content_blocks = content_blocks;
    header.header_crc32 = 0;
    header.header_crc32 = libk::crc32_ieee(libk::as_bytes(reinterpret_cast<const mx_u8_t*>(&header), sizeof(header)));
    return region_write_block(kDurableHeaderBlock, g_header_block);
}

// ---------------------------------------------------------------------------
// Sync: verify invariants, serialise /data, flush.
// ---------------------------------------------------------------------------
mx_i32_t durable_sync() {
    const mx_i32_t rc = verify_refcount_index();
    if (rc != MX_OK) {
        return rc;
    }
    if (!durable_available()) {
        mochi::log_raw_marker("io.fs: sync: no durable backing; /data is volatile\n");
        return static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED);
    }

    const mx_u32_t count = enumerate_data(g_entries);
    if (count == 0) return MX_ERR_FAULT;

    // g_entries[0] is the /data root itself and is implicit on disk
    // (parent_slot 0); only its descendants get records.
    mx_u64_t total = (count > 0 ? static_cast<mx_u64_t>(count - 1) : 0) * kDurableRecordMetaBlocks;
    for (mx_u32_t i = 1; i < count; ++i) {
        if (g_entries[i].inode->type == FS_INODE_FILE) {
            if (!libk::checked_add(total, blocks_for_size(g_entries[i].inode->size), &total)) {
                return MX_ERR_OVERFLOW;
            }
        }
    }
    if (total > g_region_blocks - kDurableHeaderBlocks || total > ~mx_u32_t{0}) {
        mochi::log_raw_marker("io.fs: sync: durable region full\n");
        return static_cast<mx_i32_t>(MX_ERR_NOMEM);
    }

    // Mark the region dirty before touching records so a torn sync is rejected
    // by the loader instead of being replayed.
    if (!write_header(kDurableStateDirty, static_cast<mx_u32_t>(total))) {
        return static_cast<mx_i32_t>(MX_ERR_FAULT);
    }

    const mx_i32_t dirty_flush = disk_flush();
    if (dirty_flush != MX_OK) return dirty_flush;

    mx_u64_t rel = kDurableFirstRecordBlock;
    for (mx_u32_t i = 1; i < count; ++i) {
        const InodeState& inode = *g_entries[i].inode;
        DurableRecord& rec = fill_record(g_entries[i]);
        if (inode.type == FS_INODE_FILE) {
            mx_u32_t crc = libk::kCrc32IeeeInit;
            for (mx_u64_t d = 0; d < rec.data_blocks; ++d) {
                read_logical_block(inode, d, g_data_stage);
                crc = libk::crc32_ieee_update(crc, libk::as_bytes(g_data_stage, kBlockSize));
                if (!region_write_block(rel + kDurableRecordMetaBlocks + d, g_data_stage)) {
                    return static_cast<mx_i32_t>(MX_ERR_FAULT);
                }
            }
            rec.data_crc32 = ~crc;
            finalize_record(rec);
            if (!region_write_block(rel, g_record_block)) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
            rel += kDurableRecordMetaBlocks + rec.data_blocks;
        } else {
            finalize_record(rec);
            if (!region_write_block(rel, g_record_block)) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
            rel += kDurableRecordMetaBlocks;
        }
    }

    const mx_i32_t records_flush = disk_flush();
    if (records_flush != MX_OK) return records_flush;
    if (!write_header(kDurableStateCommitted, static_cast<mx_u32_t>(total))) {
        return static_cast<mx_i32_t>(MX_ERR_FAULT);
    }
    const mx_i32_t flush = disk_flush();
    if (flush != MX_OK) {
        return flush;
    }
    g_metadata_dirty = false;
    return MX_OK;
}

// ---------------------------------------------------------------------------
// Load: validate the whole region, then replay it into the in-memory /data.
// ---------------------------------------------------------------------------
// Validate the record staged in g_record_block. Zeros the CRC field in place and
// restores it, so no copy of the block is needed.
static bool validate_record(mx_u64_t rel, mx_u64_t end) {
    auto& rec = *reinterpret_cast<DurableRecord*>(g_record_block);
    if (rec.magic != kDurableRecordMagic) {
        return false;
    }
    if (rec.name_len == 0 || rec.name_len > kMaxName) {
        return false;
    }
    if (libk::str::length_n(rec.name, sizeof(rec.name)) != rec.name_len ||
        (rec.inode_class != FS_CLASS_USER_DATA && rec.inode_class != FS_CLASS_CONTENT_STORE)) {
        return false;
    }
    if (rec.type != FS_INODE_FILE && rec.type != FS_INODE_DIR) {
        return false;
    }
    const mx_u32_t stored_crc = rec.record_crc32;
    rec.record_crc32 = 0;
    const mx_u32_t actual_crc = libk::crc32_ieee(libk::as_bytes(g_record_block, sizeof(DurableRecord)));
    rec.record_crc32 = stored_crc;
    if (actual_crc != stored_crc) {
        return false;
    }
    if (rel >= end || rec.data_blocks > end - rel - kDurableRecordMetaBlocks) {
        return false;
    }
    if (rec.type == FS_INODE_FILE) {
        if (rec.data_blocks != blocks_for_size(rec.size)) {
            return false;
        }
        mx_u32_t crc = libk::kCrc32IeeeInit;
        for (mx_u64_t d = 0; d < rec.data_blocks; ++d) {
            if (!region_read_block(rel + kDurableRecordMetaBlocks + d, g_data_stage)) {
                return false;
            }
            crc = libk::crc32_ieee_update(crc, libk::as_bytes(g_data_stage, kBlockSize));
        }
        if (~crc != rec.data_crc32) {
            return false;
        }
    } else if (rec.data_blocks != 0 || rec.size != 0) {
        return false;
    }
    return true;
}

bool durable_load() {
    if (!durable_available()) {
        return false;
    }
    if (!region_read_block(kDurableHeaderBlock, g_header_block)) {
        return false;
    }
    auto& header = *reinterpret_cast<DurableHeader*>(g_header_block);
    if (header.magic != kDurableHeaderMagic || header.version != kDurableVersion ||
        header.state != kDurableStateCommitted) {
        return false;
    }
    const mx_u32_t stored_crc = header.header_crc32;
    header.header_crc32 = 0;
    const mx_u32_t actual_crc = libk::crc32_ieee(libk::as_bytes(g_header_block, sizeof(DurableHeader)));
    header.header_crc32 = stored_crc;
    if (actual_crc != stored_crc) {
        return false;
    }
    if (static_cast<mx_u64_t>(header.content_blocks) > g_region_blocks - kDurableHeaderBlocks) {
        return false;
    }
    const mx_u64_t end = kDurableHeaderBlocks + header.content_blocks;

    // Pass 1: validate before importing anything, so corruption never leaves a
    // partially rebuilt /data tree in memory.
    mx_u64_t rel = kDurableFirstRecordBlock;
    mx_u32_t record_count = 0;
    while (rel < end) {
        if (!region_read_block(rel, g_record_block)) {
            return false;
        }
        if (!validate_record(rel, end)) {
            return false;
        }
        auto& rec = *reinterpret_cast<const DurableRecord*>(g_record_block);
        if (record_count >= kDurableMaxRecords - 1 || rec.parent_slot > record_count ||
            (rec.parent_slot && g_record_types[rec.parent_slot - 1] != FS_INODE_DIR)) {
            return false;
        }
        g_record_types[record_count++] = rec.type;
        rel += kDurableRecordMetaBlocks + rec.data_blocks;
    }
    if (rel != end) {
        return false;
    }

    // Pass 2 may still fail due to I/O or exhausted memory. Track allocations
    // immediately, and roll back only the inodes created by this replay.
    libk::zero(g_record_slots);
    InodeState* root = live_inode(g_data_inode);
    if (!root || root->type != FS_INODE_DIR) return false;
    const mx_u64_t generation_before = g_generation;
    const bool dirty_before = g_metadata_dirty;
    mx_u32_t slot = 0;
    rel = kDurableFirstRecordBlock;
    bool complete = true;
    while (rel < end) {
        if (!region_read_block(rel, g_record_block) || !validate_record(rel, end)) {
            complete = false;
            break;
        }
        const auto& rec = *reinterpret_cast<const DurableRecord*>(g_record_block);
        if (slot >= record_count || rec.parent_slot > slot) {
            complete = false;
            break;
        }
        InodeState* parent = rec.parent_slot ? g_record_slots[rec.parent_slot - 1] : root;
        if (!parent || parent->type != FS_INODE_DIR ||
            find_live_child(parent->inode_number, rec.name, rec.name_len)) {
            complete = false;
            break;
        }
        // Replay writes are internal; restore immutable content-store status
        // only after all of a file's blocks have been loaded.
        InodeState* inode = alloc_inode(rec.type, FS_CLASS_USER_DATA, parent->inode_number,
                                       rec.name, rec.name_len);
        if (!inode) {
            complete = false;
            break;
        }
        g_record_slots[slot++] = inode;
        for (mx_u64_t d = 0; d < rec.data_blocks; ++d) {
            if (!region_read_block(rel + kDurableRecordMetaBlocks + d, g_data_stage)) {
                complete = false;
                break;
            }
            // Sparse runs stay sparse, so replay uses the same extent and
            // physical-block quotas as the original in-memory file.
            if (!libk::is_zero(libk::as_bytes(g_data_stage)) &&
                write_bytes_unbounded(*inode, d * kBlockSize, g_data_stage, kBlockSize) != MX_OK) {
                complete = false;
                break;
            }
        }
        if (!complete) break;
        inode->size = rec.size;
        inode->inode_class = rec.inode_class;
        rel += kDurableRecordMetaBlocks + rec.data_blocks;
    }
    if (!complete || slot != record_count) {
        while (slot) {
            InodeState& inode = *g_record_slots[--slot];
            release_inode_extents(inode);
            libk::zero(inode);
        }
        g_generation = generation_before;
        g_metadata_dirty = dirty_before;
        return false;
    }
    g_metadata_dirty = false;
    return true;
}

void handle_sync(const mochi::fs::Server& fs, const RingSlot& slot) {
    const mx_i32_t status = durable_sync();
    send_status_only(fs, slot, FS_SYNC_REPLY, status);
}
