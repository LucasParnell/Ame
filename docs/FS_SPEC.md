# Ame Filesystem Specification (FS_SPEC)

Companion to [the Ame storage architecture](architecture.md#amefs-and-durable-storage).

---

## 1. Overview

The Ame filesystem runs entirely in userland as the `io.fs` service. The kernel has no visibility into filesystem internals; it provides only the capability primitives (`CAP_MMIO`, `CAP_IRQ`, `CAP_ARENA`, `CAP_DOORBELL`) that the FS service and its block driver consume.

```
[ client service ]  ──ring IPC──>  [ io.fs ]  ──ring IPC──>  [ drv.blk.virtio ]
                                        │                            │
                                   CAP_ARENA                   CAP_MMIO + CAP_IRQ
                                   (zero-copy)                 (virtio-mmio region)
```

No new kernel capability types are required.

### Terminology

| Term | Meaning |
|---|---|
| `fs_handle_t` | `u32` — FS-local open-file handle, managed by `io.fs`, not the kernel |
| `mx_cap_handle_t` | `u64` — kernel capability handle (32-bit index + 32-bit generation) |
| block | 4096-byte unit; all addresses are in blocks unless noted |
| extent | contiguous run of physical blocks |
| generation | monotonic `u64` incremented on metadata/data mapping updates |

### V1 Design Bias

V1 optimizes for fast, boring safety instead of full snapshot/journal machinery.
The model deliberately borrows two proven filesystem lessons:

- **ext4 `data=ordered` shape:** write data first, then publish/flush metadata;
  avoid full data journaling on the hot path.
- **bcachefs indexing shape:** metadata is explicit keyed state (inodes, dirents,
  extents, refcounts), with generations, checksums, and invariant checks; the
  journal/replay layer is an optimization and recovery aid, not the core API.

For V1, snapshots remain a small optional hook for testing CoW/refcount behavior.
They are not required for boot success. The disk store implements ordered CoW
data plus physical metadata redo and recovery (§7); logical delta records and
disk snapshots remain future work.

References used while shaping V1:

- ext4 ordered mode and journaling: https://docs.kernel.org/admin-guide/ext4.html
  and https://docs.kernel.org/filesystems/ext4/journal.html
- bcachefs architecture, keyed metadata, ordered updates, and snapshot shape:
  https://bcachefs.org/Architecture/ and https://bcachefs.org/Snapshots/

---

## 2. Disk Layout

```
Block 0       Superblock
Block 1       Checkpoint record
Blocks 2..K   Reserved log area  (journal_start_block=2, journal_block_count=N)
Blocks K+..   Data area         (first_data_block = journal_start_block + N)
```

The superblock encodes reserved log bounds. The allocator must not touch those
blocks. Metadata redo records occupy this reserved area; file payloads and
metadata home blocks allocate from the remaining volume (§7).

### 2.1 Superblock (block 0, exactly 4096 bytes)

```cpp
struct FsSuperblock {
    u32  magic;                    // 0xFEED'FACE
    u32  version;                  // 1
    u64  total_blocks;
    u64  block_size;               // 4096
    u64  inode_table_root;         // B+tree root for inode table
    u64  extent_bitmap_root;       // B+tree root for free-space bitmap
    u64  phys_refcount_root;       // B+tree root for global extent refcounts
    u64  free_extent_count;        // hint; not authoritative
    u64  snapshot_tree_root;       // B+tree root for snapshot list
    u64  journal_start_block;      // first reserved log block (== 2 for new volumes)
    u64  journal_block_count;      // reserved log block count
    u64  first_data_block;         // first allocatable data block
    u64  current_generation;       // generation assigned to new metadata/data mappings
    u32  flags;                    // FS_FLAG_DIRTY=1 (cleared on clean unmount)
    u32  reserved0;
    u8   uuid[16];
    u8   padding[3976];
};
static_assert(sizeof(FsSuperblock) == 4096);
```

### 2.2 Checkpoint Record (block 1, exactly 4096 bytes)

Written after the committed metadata after-images reach their home blocks.
The journal also contains a complete checkpoint after-image, so a torn block 1
can be recovered (§7).

```cpp
struct FsCheckpoint {
    u64  lsn;                      // last committed journal LSN
    u64  inode_table_root;
    u64  extent_bitmap_root;
    u64  phys_refcount_root;
    u64  snapshot_tree_root;
    u64  current_generation;
    u64  timestamp_ns;
    u32  crc32;                    // CRC32 of fields above
    u32  reserved;
    u8   padding[4032];
};
static_assert(sizeof(FsCheckpoint) == 4096);
```

---

## 3. B+Tree Engine

All metadata (inode table, per-inode extent trees, directory index, free-space bitmap, phys-refcount table, snapshot list) uses the same B+tree node format. Leaf entry layouts are typed per tree.

### 3.1 Node Header (64 bytes, first block of every node)

```cpp
struct BTreeNodeHeader {
    u16  node_type;                // BTREE_INTERNAL=1, BTREE_LEAF=2
    u16  entry_count;              // number of valid entries (0..63)
    u32  flags;                    // reserved
    u64  generation;               // filesystem generation when written
    u64  parent_block;             // 0 if root
    u64  next_sibling;             // next leaf in key order; 0 if none
    u8   reserved[32];
};
static_assert(sizeof(BTreeNodeHeader) == 64);
```

One 4096-byte block holds one header + 63 entries:

```cpp
// full node is one 4096-byte block:
// sizeof(BTreeNodeHeader) + 63 * sizeof(any 64-byte entry) == 64 + 4032 == 4096
```

### 3.2 Internal Node Entry (64 bytes, shared by all trees)

```cpp
struct BTreeInternalEntry {
    u64  max_key;                  // max key in child subtree
    u64  child_block;
    u8   reserved[48];
};
static_assert(sizeof(BTreeInternalEntry) == 64);
```

### 3.3 Typed Leaf Entries

All leaf entries are 64 bytes. The type is determined by which tree the node belongs to.

#### Inode Table Leaf (tree keyed by `inode_number`)

```cpp
struct InodeLeafEntry {
    u32  inode_number;
    u16  type;                     // INODE_FILE=1, INODE_DIR=2, INODE_SYMLINK=3
    u16  inode_class;              // CLASS_CONTENT_STORE=1, CLASS_USER_DATA=2
    u64  size;                     // bytes
    u64  extent_tree_root;         // root block of per-inode extent B+tree
    u32  nlink;
    u32  legacy_mode;              // ignored; new images write zero
    u64  ctime_ns;
    u64  mtime_ns;
    u8   reserved[16];
};
static_assert(sizeof(InodeLeafEntry) == 64);
```

#### Per-Inode Extent Tree Leaf (tree keyed by `logical_block`)

```cpp
struct ExtentLeafEntry {
    u64  logical_block;            // file offset in blocks
    u64  phys_block;               // physical start block
    u64  length_blocks;
    u64  generation;               // generation of the metadata update that created this mapping
    u16  flags;                    // EXTENT_COLD=1, EXTENT_PINNED=2
    u16  reserved;
    u32  data_crc32;               // V1 fast integrity check over one 4096-byte block
    u32  reserved0;
    u8   pad[20];
};
static_assert(sizeof(ExtentLeafEntry) == 64);
```

#### Physical Extent Refcount Tree Leaf (tree keyed by `phys_block`)

This is the **authoritative refcount** for shared physical ranges, including
live inode mappings and user-visible snapshot pins. Per-inode extent entries
store only mappings; `cow_refcount` is stored here, not per-inode. Records are
non-overlapping physical ranges and are split/merged as writes, promotions,
snapshots, and deletes change reference counts.

```cpp
struct PhysExtentLeafEntry {
    u64  phys_block;
    u64  length_blocks;
    u32  refcount;                 // live inode mappings + snapshot pins
    u32  flags;                    // PHYS_COW_PROTECTED=1
    u8   reserved[40];
};
static_assert(sizeof(PhysExtentLeafEntry) == 64);
```

#### Directory Tree Leaf (tree keyed by `compound_key`)

`compound_key = ((u64)parent_inode << 32) | name_hash32`

On key collision (same parent + same hash), scan all matching leaves by `name_len` + `name`.

```cpp
struct DirLeafEntry {
    u64  compound_key;
    u32  child_inode;
    u8   name_len;                 // 1..47
    u8   file_type;                // mirrors inode type; valid for readdir without inode lookup
    u16  reserved;
    char name[48];                 // null-terminated; max 47 usable chars inline
};
static_assert(sizeof(DirLeafEntry) == 64);
```

Names longer than 47 chars: set `DirLeafEntry.name_len = 0xFF` and store the
actual name in a separate name-overflow block referenced from a future inode
extension field. V0 does not implement overflow names; reject names > 47 chars
with `FS_ERR_NAME_TOO_LONG`.

#### Free-Space Bitmap Tree Leaf (tree keyed by `range_start`)

```cpp
struct FreeSpaceLeafEntry {
    u64  range_start;              // first block of this range
    u64  range_end;                // exclusive
    u64  bitmap_block;            // block containing 2 bits/block bitmap
    u8   reserved[40];
};
static_assert(sizeof(FreeSpaceLeafEntry) == 64);
```

Bitmap encoding (2 bits per block): `00`=free, `01`=allocated, `10`=snapshot-ref, `11`=reserved.
Sequential allocation hint: `free_extent_count` in superblock; wrap on EOF.

#### Snapshot Tree Leaf (tree keyed by `snapshot_id`)

```cpp
struct SnapshotLeafEntry {
    u64  snapshot_id;              // monotonic, assigned from current_generation
    u64  timestamp_ns;
    u64  inode_table_root;         // frozen root at snapshot time
    u64  extent_bitmap_root;
    u64  phys_refcount_root;       // global refcount root after snapshot pins
    u64  parent_snapshot_id;       // 0 if first snapshot
    u32  flags;                    // SNAPSHOT_RO_MOUNTABLE=1
    u32  reserved;
    u8   pad[8];
};
static_assert(sizeof(SnapshotLeafEntry) == 64);
```

### 3.4 Metadata CoW Rule

The following rules describe optional snapshot-capable metadata CoW. The current
disk store has no disk snapshots and updates metadata home blocks through redo
transactions (§7); memory snapshots use their own bounded backend.

B+tree node writes obey copy-on-write when a snapshot is active:

- On insert/delete: if `node.generation < fs.current_generation`, allocate a new node block
  and write there; update parent pointer to new block; do not modify the old block in place.
- If `node.generation == fs.current_generation`, modify in place (same generation = no snapshot
  has frozen this node yet).
- On `FS_SNAPSHOT`: freeze the current roots under the current generation, then
  increment `current_generation`. Subsequently modified paths allocate new nodes.

This preserves the frozen roots stored in `SnapshotLeafEntry`.

---

## 4. CoW Semantics

CoW policy is an **inode property**, not a capability property. All capability handles
referencing an inode see the same policy class.

| Class | `inode_class` | Default | Semantics |
|---|---|---|---|
| `content-store` | 1 | CoW-OFF | Written once; read by many; extents shared read-only |
| `user-data` | 2 | CoW-ON | Mutable; extent write triggers CoW when refcount > 1 |

### 4.1 `cow_write_extent` (user-data inodes)

The disk writer always allocates a new 4 KiB data block, including sole-owner
updates. Partial writes preserve and checksum the old bytes; holes read as zero.
It updates the extent mapping and physical range refcounts in the same metadata
transaction. Old blocks remain unavailable for reuse until the replacement
mapping commits. This preserves the previous committed bytes through power loss.
Contiguous data writes batch up to 64 KiB before the journal durability barrier.

### 4.2 `promote(content_store_handle)` → `new_user_data_handle`

```
1. Read source InodeLeafEntry.
2. Allocate new inode number; create InodeLeafEntry with class=CLASS_USER_DATA.
3. For each ExtentLeafEntry in source extent tree:
     Insert matching ExtentLeafEntry into new inode's extent tree.
     Increment/split the matching PhysExtentLeafEntry range for the physical blocks.
4. Insert new InodeLeafEntry into inode table.
5. Publish ordered metadata delta.
6. Return new fs_handle_t to caller.
```

The source inode (content-store) is unchanged. Both inodes share extents until the
user-data inode's first write triggers `cow_write_extent`.

### 4.3 Per-Extent Flag Overrides

`ExtentLeafEntry.flags` can carry `EXTENT_COW=4` to override inode class for individual
extents (e.g., one save-game extent inside a content-store package). V0: not yet implemented;
reserved for V1.

---

## 5. Ring Protocol

### 5.1 Frame Layout (64-byte ring slot)

The FS protocol uses the repo-wide `MxRingSlot` ABI from
`shared/include/midix/abi/ring.h`. `slot.type`, `slot.length`, and `slot.seq`
remain ring metadata; each FS request/reply struct must fit in `payload[48]`.
No FS-specific ring header is defined.

```cpp
using FsFrame = MxRingSlot;        // type, length, seq, payload[48]
static_assert(sizeof(FsFrame) == 64);
static_assert(MX_RING_INLINE_PAYLOAD == 48);
```

### 5.2 Message Types

```cpp
enum FsMsgType : u32 {
    FS_OPEN         = 1,
    FS_OPEN_REPLY   = 2,
    FS_CLOSE        = 3,
    FS_READ         = 4,
    FS_READ_REPLY   = 5,
    FS_WRITE        = 6,
    FS_WRITE_REPLY  = 7,
    FS_READDIR      = 8,
    FS_READDIR_REPLY= 9,
    FS_MKDIR        = 10,
    FS_MKDIR_REPLY  = 11,
    FS_UNLINK       = 12,
    FS_UNLINK_REPLY = 13,
    FS_RENAME       = 14,
    FS_RENAME_REPLY = 15,
    FS_PROMOTE      = 16,
    FS_PROMOTE_REPLY= 17,
    FS_SNAPSHOT     = 18,
    FS_SNAPSHOT_REPLY=19,
    FS_SNAPSHOT_MNT = 20,
    FS_SNAPSHOT_MNT_REPLY=21,
    FS_SNAPSHOT_DROP= 22,
    FS_SYNC         = 23,
    FS_SYNC_REPLY   = 24,
    FS_PREFETCH     = 25,          // advisory; no reply
    FS_ERROR        = 26,          // generic error reply
    FS_CLOSE_REPLY  = 27,          // status-only ack for FS_CLOSE
    FS_SNAPSHOT_DROP_REPLY = 28,   // status-only ack for FS_SNAPSHOT_DROP
};
```

### 5.3 Request/Reply Payloads (all ≤ 48 bytes)

#### Open / Open Reply

```cpp
struct FsOpenRequest {             // 48 bytes
    u32  parent_inode;
    u32  flags;                    // FS_OPEN_READ=1, WRITE=2, CREATE=4, TRUNC=8, LONG_NAME=0x10
    u8   name_len;                 // 0xFF = name is arena cap (LONG_NAME)
    u8   pad[3];
    char name[36];                 // null-terminated; max 35 chars inline
                                   // if LONG_NAME: name[0..7] = mx_cap_handle_t arena_cap,
                                   // name[8..11] = u32 byte length in that arena
};
static_assert(sizeof(FsOpenRequest) == 48);

struct FsOpenReply {               // 48 bytes
    i32  status;                   // 0 = ok; negative = mx_status_t error
    fs_handle_t handle;            // FS-local handle (u32)
    u64  inode_number;
    u64  size;
    u16  type;
    u16  inode_class;
    u32  access;                   // FsAccess for the returned session handle
    u8   reserved[16];
};
static_assert(sizeof(FsOpenReply) == 48);
```

#### Read / Read Reply

```cpp
// FS_INLINE_READ_MAX = 32 bytes
struct FsReadRequest {             // 24 bytes (fits in 48-byte payload, 24 spare)
    fs_handle_t handle;
    u32  flags;                    // FS_READ_ZERO_COPY=1, FS_READ_DIRECT=2
    u64  offset;                   // byte offset
    u32  length;                   // bytes requested
    u32  reserved;
};
static_assert(sizeof(FsReadRequest) == 24);

struct FsReadReply {               // 48 bytes
    i32  status;
    u32  length_read;
    mx_cap_handle_t arena_cap;     // u64; non-zero = zero-copy; client maps and revokes
    u8   inline_data[32];          // used when arena_cap == 0 (length_read ≤ 32)
};
static_assert(sizeof(FsReadReply) == 48);
```

#### Write / Write Reply

```cpp
// FS_INLINE_WRITE_MAX = 24 bytes
struct FsWriteRequest {            // 48 bytes
    fs_handle_t handle;
    u32  flags;                    // FS_WRITE_ZERO_COPY=1
    u64  offset;
    u32  length;
    u32  reserved;
    u8   payload[24];              // if FS_WRITE_ZERO_COPY: token[0..7], byte offset[8..15], zero[16..23]
                                   // else: inline data (length ≤ 24)
};
static_assert(sizeof(FsWriteRequest) == 48);

struct FsWriteReply {              // 8 bytes
    i32  status;
    u32  bytes_written;
};
static_assert(sizeof(FsWriteReply) == 8);
```

#### Readdir / Readdir Reply

The version-2 ABI returns one inline entry per 48-byte reply: signed `status`,
`entry_count`, `next_cookie`, `inode_number` (each 32 bits), `type` (16 bits),
`name_len` and `flags` (8 bits each), then `name[28]`. Flags distinguish an entry,
EOF and a truncated display name. The 16-byte request contains `handle`,
`cookie`, `flags` and `reserved`. See the canonical structs in
`shared/include/midix/abi/protocols/fs.h`; directory names on disk retain 48 bytes.

#### Mkdir / Unlink / Rename

```cpp
struct FsMkdirRequest {            // 48 bytes
    u32  parent_inode;
    u32  flags;                    // FS_INODE_CLASS_CONTENT_STORE=1, _USER_DATA=2
    u32  reserved0;                // zero; inherit directory authority
    u8   name_len;
    u8   pad[3];
    char name[32];
};
static_assert(sizeof(FsMkdirRequest) == 48);

struct FsUnlinkRequest {           // 48 bytes
    u32  parent_inode;
    u32  flags;
    u8   name_len;
    u8   pad[3];
    char name[36];
};
static_assert(sizeof(FsUnlinkRequest) == 48);

struct FsRenameRequest {           // 48 bytes
    u32  old_parent;
    u32  new_parent;
    u8   old_name_len;
    u8   new_name_len;
    u16  flags;
    char old_name[20];
    char new_name[16];
};
static_assert(sizeof(FsRenameRequest) == 48);

// Mkdir/Unlink/Rename reply: u32 status in first 4 bytes of payload
```

#### Promote

```cpp
struct FsPromoteRequest {          // 8 bytes
    fs_handle_t handle;            // handle to content-store inode
    u32  flags;
};
static_assert(sizeof(FsPromoteRequest) == 8);

struct FsPromoteReply {            // 8 bytes
    i32  status;
    fs_handle_t new_handle;        // handle to new user-data inode
};
static_assert(sizeof(FsPromoteReply) == 8);
```

#### Snapshot

```cpp
struct FsSnapshotRequest {         // 16 bytes
    u32  flags;                    // FS_SNAPSHOT_RO=1
    u8   label[12];
};
static_assert(sizeof(FsSnapshotRequest) == 16);

struct FsSnapshotReply {           // 16 bytes
    i32  status;
    u32  reserved;
    u64  snapshot_id;
};
static_assert(sizeof(FsSnapshotReply) == 16);

struct FsSnapshotMntRequest {      // 16 bytes
    u64  snapshot_id;
    u32  flags;
    u32  reserved;
};
static_assert(sizeof(FsSnapshotMntRequest) == 16);

struct FsSnapshotMntReply {        // 8 bytes
    i32  status;
    fs_handle_t dir_handle;        // root dir handle, read-only
};
static_assert(sizeof(FsSnapshotMntReply) == 8);
```

#### Prefetch Hint (fire-and-forget, no reply)

```cpp
struct FsPrefetchHint {            // 24 bytes
    fs_handle_t handle;
    u32  type;                     // PREFETCH_SEQ=1, PREFETCH_RANDOM=2, PREFETCH_WARMUP=3
    u64  offset;
    u32  length;
    u32  reserved;
};
static_assert(sizeof(FsPrefetchHint) == 24);
```

### 5.4 Client/Server Pattern

Mirrors `userland/lib/include/mochi/framebuffer.h`.

```
// userland/lib/include/mochi/fs.h
namespace mochi::fs {

using fs_handle_t = u32;

static constexpr u32 FS_INLINE_READ_MAX  = 32;
static constexpr u32 FS_INLINE_WRITE_MAX = 24;

struct Client {
    SpscEndpoint ep;
    bool try_open(u32 parent, const char* name, u32 flags, FsOpenReply* out);
    bool wait_open(u32 parent, const char* name, u32 flags, FsOpenReply* out);
    bool try_read(fs_handle_t h, u64 offset, u32 len, FsReadReply* out);
    bool try_write(fs_handle_t h, u64 offset, const void* data, u32 len, FsWriteReply* out);
    bool try_promote(fs_handle_t h, FsPromoteReply* out);
    bool try_snapshot(u32 flags, const char* label, FsSnapshotReply* out);
    // ... sync, prefetch, mkdir, unlink, rename, readdir
};

struct Server {
    SpscEndpoint ep;
    bool try_recv(FsFrame* out);
    void send_reply(const FsFrame* req, FsMsgType reply_type, const void* payload, u32 len);
    void send_error(const FsFrame* req, u32 status);
};

}  // namespace mochi::fs
```

---

## 6. Arena Transport and Cache Coherence

Control requests remain 48-byte ring payloads. Reads up to 32 bytes are inline;
larger disk reads return a read-only `CAP_ARENA` view of up to 4 KiB, also selected
explicitly by `FS_READ_ZERO_COPY`. `Client::read_bytes` loops across pages.

`io.fs` has 16 immutable read-view slots, with two retained slots per session.
Identities (owner, inode, offset, committed generation) are separate from page
payloads. A changed generation selects a new view. An arena can be reused only
when kernel object references show that its owning cap and server mapping are
the only references left. Clients must **both unmap and revoke** after use.
Revoking a cap while keeping a mapping continues to pin the old bytes. Session
teardown drops server references; surviving client references retain that old
object and a later session receives a fresh object. Exhausted pinned slots
return `MX_ERR_BUSY`.

Each FS session also receives a separate 64 KiB writable upload arena from init.
`FS_WRITE_ZERO_COPY` encodes the client's granted arena token and byte offset as
two little-endian `u64`s in `FsWriteRequest.payload`, followed by a zero `u64`.
The service validates the pregranted token, request bounds and reserved fields;
it never interprets a caller-local token as authority in its own cap table.
The client owns the upload bytes until request publication and must keep them
stable until the reply. This avoids inline-only 24-byte writes. It is arena
transport, with copies at the storage boundary; direct client DMA is unsupported.
`FS_READ_DIRECT` returns `MX_ERR_NOT_IMPLEMENTED`.

On Anne, ADMA2 uses a separate permanently pinned, contiguous 68 KiB arena:
one descriptor page plus 64 KiB of data. All CPU aliases are Normal
Non-Cacheable. Driver barriers publish descriptors/data before command submission
and consume device writes after completion. Client and filesystem arenas remain
ordinary CPU-coherent write-back memory. Only the block driver sees the DMA bus
address. DMA failures never publish partial reads or retry uncertain writes.

The 32-entry block cache separates tags/ages from aligned payload pages and
invalidates every affected page **before** a write, including partial failures.
A block IPC timeout poisons that connection: an uncorrelated late ACK cannot
allow the shared transfer arena to be reused.

---

## 7. Disk Transactions and Recovery

### 7.1 Volume geometry and working sets

`store.cc` uses disk-resident keyed B+trees for inodes, directories, per-file
extents, physical reference ranges and full-volume free-space bitmap ranges.
The superblock determines capacity. On Anne this is the existing userdata GPT
partition: 13,260,800 × 4,096 bytes (54,316,236,800 bytes, about 50.6 GiB).
The old 1 MiB journal snapshot is no longer the writable capacity limit.
Packaged root handles remain read-only; `/data` redirects to user inode
`0x80000000`. `/tmp` remains a separate volatile memory filesystem.

RAM is bounded independently of volume size: 96 staged metadata pages, eight
tree descent levels and 16 transfer pages. Large file/truncate/promote operations
use bounded transactions. Internal nodes with header flag bit 0 store subtree
entry counts in the first eight reserved bytes of each internal entry; older
nodes without counts remain readable. Directory lookup scans equal hash keys
to resolve collisions by name. Rename prevents directory cycles.

Bitmap states are 0 free, 1 allocated, 2 pending release and 3 emergency metadata
reserve. State 2 exists only in staged transaction state; released blocks cannot
be reused before commit. A 32-block reserve supports metadata reclamation at
ENOSPC and is replenished from freed blocks. Truncate/unlink detach data
atomically, then reclaim extents in bounded batches; persistent orphan markers
resume unfinished cleanup on mount. Promote builds a hidden orphan, increments
shared physical references in batches and publishes only its complete mapping.

Checkpoint padding begins with the `FTX1` extension (little-endian `0x31585446`):
`u32 magic, next_inode, orphan_count, reserve_count`, followed by 32 `u64` reserved
block addresses and a `u32` CRC over the first 336 bytes of the complete
checkpoint. The original checkpoint CRC over its first 56 bytes is retained.
Inode reserved bytes 0..3 store the parent inode; byte 4 marks pending cleanup.
These fields do not change any on-disk structure sizes.

### 7.2 Journal format

The canonical `JournalRecord` and `JournalCommit` are each 4 KiB. Record types
1–7 retain their logical-delta assignments for future use. The implemented
`JRN_METADATA_FRAGMENT=8` stores a physical metadata after-image fragment:

| Offset within `JournalRecord.data` | Field |
| --- | --- |
| 0 | `u32` target block, repeated inside CRC-covered payload |
| 4 | `u16` byte offset (0 or 4056) |
| 6 | `u16` length (4056 or 40) |
| 8 | after-image bytes; unused remainder zero |

Two records represent one metadata page. Each record CRC covers all 4064 data
bytes; the commit batch CRC covers those data fields concatenated in LSN order.
Records start at `journal_start_block`, and the commit occupies the **last**
reserved log block. The checkpoint after-image is the final page in the batch.
This is a bounded, reusable transaction slot rather than a circular logical log.

### 7.3 Durable commit

1. Write new CoW data and metadata fragment records. Flush the device.
2. Write the commit with its LSN range, count and batch CRC. Flush again.
3. Apply metadata after-images to home blocks, write checkpoint block 1, and
   flush before acknowledging the mutation.

File payloads are not journaled. After step 2, redo is sufficient to finish any
torn home writes. Before it, the preceding committed mapping remains valid.
Each successful mutation is committed; `FS_SYNC` performs a further device flush.
Data checksums are verified on reads and partial overwrites, and affected tree,
allocation and refcount invariants are checked during mutations. `FS_SYNC` is
not a whole-volume scrub. Failed or ambiguous writes poison the mount until
fresh recovery; no successful durability reply is fabricated.

### 7.4 Recovery and compatibility

Mount validates superblock geometry, checkpoint CRC/roots and the entire journal
batch before issuing any home write. It checks fragment targets, offsets,
lengths, sequence, record CRCs and the batch CRC. Only a fully committed valid
batch is replayed, idempotently, followed by a flushed checkpoint. A torn
checkpoint can be reconstructed from its journal after-image. Replay also applies
a valid batch whose LSN equals the checkpoint: the checkpoint sector may have persisted before metadata home blocks during their
shared final flush. An incomplete overwritten journal with a valid checkpoint
retains that preceding state. If neither checkpoint nor committed redo is
usable, the mount fails.

A populated legacy `DUR1` snapshot is not overwritten or automatically migrated;
the legacy backend remains available for read/export. Without a writable disk,
`/data` is volatile and `FS_SYNC` reports `MX_ERR_NOT_IMPLEMENTED`. Optional disk
snapshots currently return `MX_ERR_NOT_IMPLEMENTED`; memory snapshots retain
their existing small test implementation. Logical delta replay, disk snapshots,
tiering and direct client DMA remain separate milestones.

---

## 8. Snapshot Subsystem

Snapshots are **not on the V1 critical path**. V1 keeps the message ABI and a
small read-only implementation so CoW/refcount behavior can be exercised, but
boot success and ordinary file I/O must not depend on snapshot creation. The
following snapshot design is prospective for the disk store; its current
handlers explicitly return not-implemented on disk mounts.

### 8.1 User-Visible Snapshots vs. Service-State Preservation

These are **distinct mechanisms**:

| Mechanism | Who triggers | What it does |
|---|---|---|
| `FS_SNAPSHOT` | client | creates mountable read-only FS snapshot stored on disk |
| `pre_suspend` | kernel notify | flushes dirty data, writes checkpoint, registers arenas for kernel native-service snapshot |

`pre_suspend` does **not** automatically create a user-visible FS snapshot. An optional
policy (configurable at boot) may request a snapshot before suspend.

### 8.2 Creating a Snapshot (`FS_SNAPSHOT`)

```
1. Flush all dirty extents (`FS_SYNC` ordered checkpoint).
2. Write FsCheckpoint.
3. Capture the roots to freeze:
     snapshot_id = current_generation
     inode_table_root = current inode_table_root
     extent_bitmap_root = current extent_bitmap_root
4. Walk the frozen inode/extent roots and increment/split PhysExtentLeafEntry
   ranges for every reachable physical extent. V0 performs this synchronously;
   V1 may replace it with a lazy pin index if snapshot latency becomes a problem.
5. Allocate SnapshotLeafEntry with the frozen roots and the updated
   phys_refcount_root.
6. Insert SnapshotLeafEntry into snapshot tree.
7. Publish ordered metadata delta.
8. Increment current_generation for all subsequent metadata/data mappings.
9. Return snapshot_id to caller.
```

Subsequent metadata modifications allocate new B+tree nodes (generation-based CoW),
preserving the frozen roots. Subsequent file writes allocate new physical blocks
when the physical refcount is greater than 1, including refs held by snapshots.

### 8.3 Mounting a Snapshot (`FS_SNAPSHOT_MNT`)

Returns a read-only dir handle rooted at the snapshot's `inode_table_root`. All
lookups through this handle traverse the frozen B+tree. Writes return `FS_ERR_READ_ONLY`.

### 8.4 Dropping a Snapshot (`FS_SNAPSHOT_DROP`)

```
1. Remove SnapshotLeafEntry from snapshot tree.
2. Walk the snapshot's frozen inode/extent roots and decrement/split the matching
   PhysExtentLeafEntry ranges.
3. Free ranges whose refcount reaches 0.
4. Publish ordered metadata delta.
```

This is a cold-path background operation; it may be deferred and batched.

### 8.5 Suspend/Resume Integration

**Arena registration** (at FS startup and after format):
```
sys_arena_register(metadata_arena, ARENA_TAG_FS_METADATA)
sys_arena_register(inode_arena,    ARENA_TAG_FS_INODES)
sys_arena_register(extent_arena,   ARENA_TAG_FS_EXTENTS)
sys_arena_register(snapshot_arena, ARENA_TAG_FS_SNAPSHOTS)
```

**`pre_suspend` handler** (triggered by kernel ring notify):
```
1. Flush dirty extents. Journal commit. BLK_FLUSH.
2. Write FsCheckpoint (block 1).
3. Re-register arenas (kernel validates and captures current contents).
4. Notify kernel: pre_suspend complete.
```

**Flush target enrollment** (at boot, required for snapshot-and-evict OOM policy):
```
sys_arena_register(flush_target_arena, ARENA_TAG_FS_FLUSH)
```

Without a flush target, kernel falls back to kill + supervisor restart from last checkpoint.

**Resume**:
- Kernel restores arena contents from suspend blob.
- FS re-reads FsCheckpoint, validates B+tree roots.
- Replays journal if `FS_FLAG_DIRTY` was set.
- Re-registers arenas, resumes serving requests.

---

## 9. Block Device Driver (`drv.blk.virtio`)

**File:** `userland/drivers/blk/virtio/main.cc`

### 9.1 Virtio-MMIO Registers (ARM, legacy interface)

```cpp
static constexpr u32 VIRTIO_MMIO_MAGIC         = 0x000; // 0x74726976
static constexpr u32 VIRTIO_MMIO_VERSION       = 0x004; // 1 (legacy) or 2
static constexpr u32 VIRTIO_MMIO_DEVICE_ID     = 0x008; // 2 = block
static constexpr u32 VIRTIO_MMIO_STATUS        = 0x070;
static constexpr u32 VIRTIO_MMIO_QUEUE_SEL     = 0x030;
static constexpr u32 VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034;
static constexpr u32 VIRTIO_MMIO_QUEUE_NUM     = 0x038;
static constexpr u32 VIRTIO_MMIO_QUEUE_ALIGN   = 0x03C; // must be 0x1000
static constexpr u32 VIRTIO_MMIO_QUEUE_PFN     = 0x040; // legacy: page frame number
static constexpr u32 VIRTIO_MMIO_QUEUE_NOTIFY  = 0x050;
static constexpr u32 VIRTIO_MMIO_INTERRUPT_STATUS = 0x060;
static constexpr u32 VIRTIO_MMIO_INTERRUPT_ACK = 0x064;
static constexpr u32 VIRTIO_MMIO_HOST_FEATURES = 0x010;
static constexpr u32 VIRTIO_MMIO_GUEST_FEATURES= 0x020;
```

Feature bits:
```cpp
static constexpr u32 VIRTIO_BLK_F_FLUSH  = (1 << 9);
static constexpr u32 VIRTIO_BLK_F_BLK_SIZE = (1 << 6);
```

### 9.2 DMA Queue Setup (wired arena required)

Virtio queues require DMA-visible, physically contiguous memory. The driver allocates
a **wired arena** cap for queue memory:

```
Queue descriptors:  QUEUE_SIZE * 16 bytes
Available ring:     6 + QUEUE_SIZE * 2 bytes
Used ring:          6 + QUEUE_SIZE * 8 bytes
```

The kernel SMMU-registers the wired arena at the driver's stream ID during
`sys_arena_create_wired`. The driver passes the IOVA to virtio via `VIRTIO_MMIO_QUEUE_PFN`.

Memory barriers:
- Before writing `QUEUE_NOTIFY`: `DMB SY` (ensure descriptor + available ring writes visible to device)
- After reading used ring index: `DMB SY` (ensure device writes visible to CPU)

### 9.3 Block Driver Ring Protocol (drv.blk.virtio ↔ io.fs)

Uses the same `MxRingSlot` 64-byte control-ring slot.

```cpp
enum BlkMsgType : u32 {
    BLK_READ    = 1,
    BLK_WRITE   = 2,
    BLK_FLUSH   = 3,
    BLK_REPLY   = 4,
};

struct BlkRequest {                // 40 bytes (fits in 48-byte payload)
    u32  type;                     // BlkMsgType
    u32  flags;
    u64  sector;                   // LBA (512-byte sectors)
    u32  sector_count;             // max 128 (64 KB per request in V0)
    u32  reserved;
    mx_cap_handle_t arena_cap;     // wired DMA arena for READ/WRITE data
    u32  arena_offset;             // byte offset within arena
    u32  reserved2;
};
static_assert(sizeof(BlkRequest) == 40);

struct BlkReply {                  // 8 bytes
    i32  status;                   // 0=ok, 1=io_error, 2=timeout, 3=unsupported
    u32  sectors_transferred;
};
static_assert(sizeof(BlkReply) == 8);
```

`BLK_FLUSH` is only sent if `VIRTIO_BLK_F_FLUSH` was negotiated during feature handshake.

---

## 10. Tiering

### 10.1 Tier Descriptor

```cpp
struct TierDesc {
    u32  tier_id;                  // 0 = hot (NVMe/RAM), 1 = cold (eMMC/HDD)
    u32  flags;
    u64  base_block;
    u64  size_blocks;
    u32  blk_service_handle;       // handle to block driver service for this tier
    u32  reserved;
};
```

Registered at mount time via `FS_MOUNT` extended options (V1). V0: single tier.

### 10.2 Default Placement

- `CLASS_CONTENT_STORE` → cold tier by default
- `CLASS_USER_DATA` → hot tier by default
- Metadata B+tree nodes → hot tier always

Tier migration is background work; no client-visible latency in V0.

---

## 11. Manifest & Service Registration

`userland/system.json` declares `drv.blk.virtio` and `io.fs`, their startup
ordering, and the shared block endpoint's ring and doorbell grants. Board
profiles select the services to package; see [System descriptor](abi.md#system_descriptor).

`io.fs` registers with nameservice as `"io.fs"`. Clients resolve via nameservice to
obtain an `MxEndpointDesc` containing the cmd/reply ring caps and doorbells.

---

## 12. Implementation Phases

| Phase | Deliverable | Success marker |
|---|---|---|
| 1 | On-disk structs + virtio-blk I/O | `BLK read/write OK` |
| 2 | B+tree engine (all typed leaves, CoW node alloc) | B+tree unit tests pass |
| 3 | Inode layer + extent allocator + phys refcount tree | `FS inode CRUD OK` |
| 4 | Directory layer + path resolution (≤47-char names) | `FS mkdir/lookup OK` |
| 5 | CoW engine: `cow_write_extent` + `promote()` | `FS CoW promote OK` |
| 6 | FS ring protocol + service bootstrap + nameservice | `FS service registered OK` |
| 7 | V1 ordered sync: extent checksums, refcount invariant check, BLK_FLUSH if available | `FS V1 smoke OK` |
| 8 | Read cache arena + zero-copy path (arena_cap derivation) | `FS zero-copy OK` |
| 9 | Optional snapshot B+tree + `FS_SNAPSHOT` / `FS_SNAPSHOT_MNT` | `FS snapshot flush OK` |
| 10 | Physical metadata redo + CRC + crash recovery replay (implemented); logical deltas deferred | crash-boundary host tests pass |
| 11 | Suspend/resume: `pre_suspend` + arena registration + flush target | `MIDIX V0 FILESYSTEM OK` |
| 12 | Tiering + prefetch hints | `FS tiering migration OK` |

---

## 13. Implementation Files

| Path | Purpose |
| --- | --- |
| `shared/include/midix/abi/protocols/fs.h` | Disk structs, messages, journal record ABI |
| `userland/lib/include/mochi/fs.h` | Client/server transport and arena helpers |
| `userland/fs/fs_ring.cc` | Bootstrap, per-client sessions and dispatch |
| `userland/fs/store.h` / `store.cc` | Disk trees, allocator, refcounts, transactions and recovery |
| `userland/fs/fs_disk.cc` | Block IPC, geometry, mount and coherent block cache |
| `userland/fs/fs_handles.cc` | Session-owned handles and authority |
| `userland/fs/fs_io.cc` | Immutable read views and per-session upload transport |
| `userland/fs/fs_core.cc` / `fs_snapshot.cc` | Volatile backend and memory snapshots |
| `userland/fs/fs_durable.cc` | Legacy DUR1 checkpoint compatibility |
| `userland/drivers/blk/virtio/main.cc` | Virtio block service |
| `userland/drivers/blk/emmc/main.cc` | Anne partition-scoped eMMC block service |
| `shared/drivers/emmc/sdhci.cc` | Shared PIO/ADMA2 SDHCI transport and flush |
| `kernel/init/boot_caps.cc` | Pinned uncached DMA arena and device authority |

---

## 14. Verification

```sh
python3 -m unittest discover -s tests -p 'test_fs*.py'
python3 -m unittest discover -s tests -p test_emmc.py
python3 scripts/test/durable-persistence.py --build out/verify --timeout 60 --keep
```

Host sanitizer tests cover scalable directory trees, 2.5 MiB file writes,
physical-tail allocation, sparse offsets beyond 50 GiB, shared-extent promotion,
CoW isolation, ENOSPC reclamation and 2,790 sector-torn/reordered power-cut
cases across overwrite, truncate, unlink, rename and promotion.
The two-boot guest fixture writes and reads 3 MiB through actual IPC, and tests
read-only arena rights, generation coherence and cap/mapping lifetime pinning.
On a connected Anne, `fscheck write` writes/syncs/verifies a 3 MiB diagnostic;
`fscheck read` verifies it after reboot. Hardware results are recorded in
[EMMC.md](../boards/huawei-anne/storage.md#emmc).

### Handle access (protocol version 2)

AmeFS has no Unix owner/group/other permission model. Authority comes from the
client's granted filesystem session and its session-owned handles. `FsOpenReply.access`
reports `FS_ACCESS_READ`/`WRITE` for files and `FS_ACCESS_ENUMERATE`/`MODIFY` for
directories. `MODIFY` permits creation, removal, and renaming of children; it does
not make immutable content writable. A copied access mask is not a capability.

Opening a directory retains the parent's mutation restriction. Live `/tmp` and
`/data` are mount authorities supplied at the live root; resolving a snapshot's
root cannot cross into those live mounts. Content-store inodes, disk-backed boot
files, and snapshots never grant mutation access. Existing directory opens retain
their behavior: `FS_OPEN_READ` enumerates a directory and inherits its parent's
mutation authority. This is not an owner-based permission request.

The V1 disk inode and durable checkpoint layouts are unchanged. Their former
mode word is retained as `legacy_mode`, ignored when loading old records, and
written as zero in new records. Runtime inodes no longer carry an unused mode.
Protocol V2 reuses the old open-reply word for handle access and reserves the old
mkdir word (must be zero); clients and server must be rebuilt together.
