// kernel/init/boot_disk.cc - read-only full-disk boot loader.
//
// This is intentionally small: it only knows enough virtio-blk, GPT, and AmeFS
// to fetch /ame/boot/system.mxb before init exists.

#include "anko/init.h"
#include "anko/log.h"
#include "anko/mm.h"
#include "anko/platform.h"
#include "libk/string.h"

namespace kernel::init {

namespace {

constexpr u32 kSectorSize = 512;
constexpr u32 kFsBlockSize = 4096;
constexpr u32 kSectorsPerFsBlock = kFsBlockSize / kSectorSize;
constexpr u32 kQueueSize = 8;
constexpr u32 kMaxTransferSectors = 8;
constexpr u32 kMaxBtreeEntries = 63;
constexpr u32 kMaxBtreeDepth = 8;
constexpr u64 kMaxBundleSize = 32ull * 1024ull * 1024ull;

constexpr u32 kVirtioMagicValue = 0x74726976u;
constexpr u32 kVirtioBlkDeviceId = 2;
constexpr u32 kVirtioMmioSlotSize = 0x200;
constexpr u32 kVirtioWaitSpins = 10'000'000;

constexpr u32 VIRTIO_MMIO_MAGIC = 0x000;
constexpr u32 VIRTIO_MMIO_VERSION = 0x004;
constexpr u32 VIRTIO_MMIO_DEVICE_ID = 0x008;
constexpr u32 VIRTIO_MMIO_HOST_FEATURES = 0x010;
constexpr u32 VIRTIO_MMIO_HOST_FEATURES_SEL = 0x014;
constexpr u32 VIRTIO_MMIO_GUEST_FEATURES = 0x020;
constexpr u32 VIRTIO_MMIO_GUEST_FEATURES_SEL = 0x024;
constexpr u32 VIRTIO_MMIO_GUEST_PAGE_SIZE = 0x028;
constexpr u32 VIRTIO_MMIO_QUEUE_SEL = 0x030;
constexpr u32 VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034;
constexpr u32 VIRTIO_MMIO_QUEUE_NUM = 0x038;
constexpr u32 VIRTIO_MMIO_QUEUE_ALIGN = 0x03C;
constexpr u32 VIRTIO_MMIO_QUEUE_PFN = 0x040;
constexpr u32 VIRTIO_MMIO_QUEUE_READY = 0x044;
constexpr u32 VIRTIO_MMIO_QUEUE_NOTIFY = 0x050;
constexpr u32 VIRTIO_MMIO_INTERRUPT_STATUS = 0x060;
constexpr u32 VIRTIO_MMIO_INTERRUPT_ACK = 0x064;
constexpr u32 VIRTIO_MMIO_STATUS = 0x070;
constexpr u32 VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080;
constexpr u32 VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090;
constexpr u32 VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0A0;
constexpr u32 VIRTIO_MMIO_CONFIG = 0x100;

constexpr u32 VIRTIO_BLK_F_FLUSH = 1u << 9;
constexpr u32 VIRTIO_F_VERSION_1_BIT = 0;
constexpr u32 VIRTIO_STATUS_ACKNOWLEDGE = 1u;
constexpr u32 VIRTIO_STATUS_DRIVER = 2u;
constexpr u32 VIRTIO_STATUS_DRIVER_OK = 4u;
constexpr u32 VIRTIO_STATUS_FEATURES_OK = 8u;
constexpr u32 VIRTIO_STATUS_FAILED = 128u;
constexpr u16 VRING_DESC_F_NEXT = 1u;
constexpr u16 VRING_DESC_F_WRITE = 2u;
constexpr u32 VIRTIO_BLK_T_IN = 0;
constexpr u8 VIRTIO_BLK_S_OK = 0;

constexpr u32 GPT_HEADER_LBA = 1;
constexpr u32 GPT_ENTRY_SIZE = 128;
constexpr u32 GPT_NAME_OFFSET = 56;
constexpr u32 GPT_NAME_BYTES = 72;

constexpr u32 AMEFS_MAGIC = 0xFEED'FACEu;
constexpr u32 AMEFS_VERSION = 1;
constexpr u16 AMEFS_INODE_FILE = 1;
constexpr u16 AMEFS_INODE_DIR = 2;
constexpr u16 BTREE_INTERNAL = 1;
constexpr u16 BTREE_LEAF = 2;

struct VringDesc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};
static_assert(sizeof(VringDesc) == 16);

struct VringAvail {
    u16 flags;
    u16 idx;
    u16 ring[kQueueSize];
};

struct VringUsedElem {
    u32 id;
    u32 len;
};
static_assert(sizeof(VringUsedElem) == 8);

struct VringUsed {
    u16 flags;
    u16 idx;
    VringUsedElem ring[kQueueSize];
};

struct VirtioBlkHeader {
    u32 type;
    u32 reserved;
    u64 sector;
};
static_assert(sizeof(VirtioBlkHeader) == 16);

struct RootPartition {
    u64 first_lba;
    u64 last_lba;
};

struct FsState {
    u64 root_lba;
    u64 root_sectors;
    u64 total_blocks;
    u64 inode_root;
};

struct InodeMeta {
    u16 type;
    u64 size;
    u64 root_block;
};

struct DirChild {
    u32 inode;
    u16 type;
};

static volatile u8* g_mmio = nullptr;
static bool g_legacy_transport = false;
static u8* g_dma_base = nullptr;
static u64 g_dma_phys = 0;
static u64 g_dma_size = 0;
static u64 g_dma_bump = 0;
static VringDesc* g_desc = nullptr;
static VringAvail* g_avail = nullptr;
static VringUsed* g_used = nullptr;
static VirtioBlkHeader* g_req_hdr = nullptr;
static u8* g_req_status = nullptr;
static u8* g_data = nullptr;
static u64 g_data_offset = 0;
static u16 g_avail_idx = 0;
static u16 g_used_idx = 0;
static u64 g_capacity_sectors = 0;

alignas(4096) static u8 g_block[kFsBlockSize];
alignas(4096) static u8 g_block2[kFsBlockSize];
alignas(512) static u8 g_sector[kSectorSize];

u16 le16(const void* p) {
    const auto* b = static_cast<const u8*>(p);
    return static_cast<u16>(b[0]) | (static_cast<u16>(b[1]) << 8);
}

u32 le32(const void* p) {
    const auto* b = static_cast<const u8*>(p);
    return static_cast<u32>(b[0]) |
           (static_cast<u32>(b[1]) << 8) |
           (static_cast<u32>(b[2]) << 16) |
           (static_cast<u32>(b[3]) << 24);
}

u64 le64(const void* p) {
    return static_cast<u64>(le32(p)) |
           (static_cast<u64>(le32(static_cast<const u8*>(p) + 4)) << 32);
}

u64 align_up_u64(u64 value, u64 align) {
    return (value + align - 1u) & ~(align - 1u);
}

u32 crc32_ieee(const u8* data, usize len) {
    u32 crc = 0xFFFF'FFFFu;
    for (usize i = 0; i < len; ++i) {
        crc ^= data[i];
        for (u32 bit = 0; bit < 8; ++bit) {
            const u32 mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB8'8320u & mask);
        }
    }
    return ~crc;
}

u32 name_hash(const char* name) {
    return crc32_ieee(reinterpret_cast<const u8*>(name), libk::strlen(name));
}

void barrier() {
    asm volatile("dsb sy" ::: "memory");
}

volatile u32* reg32(u32 off) {
    return reinterpret_cast<volatile u32*>(g_mmio + off);
}

u32 mmio_read32(u32 off) {
    barrier();
    return *reg32(off);
}

void mmio_write32(u32 off, u32 value) {
    *reg32(off) = value;
    barrier();
}

void mmio_write64_pair(u32 low_off, u64 value) {
    mmio_write32(low_off, static_cast<u32>(value));
    mmio_write32(low_off + 4, static_cast<u32>(value >> 32));
}

u32 virtio_read_features(u32 selector) {
    mmio_write32(VIRTIO_MMIO_HOST_FEATURES_SEL, selector);
    return mmio_read32(VIRTIO_MMIO_HOST_FEATURES);
}

void virtio_write_features(u32 selector, u32 features) {
    mmio_write32(VIRTIO_MMIO_GUEST_FEATURES_SEL, selector);
    mmio_write32(VIRTIO_MMIO_GUEST_FEATURES, features);
}

void virtio_add_status(u32 status) {
    mmio_write32(VIRTIO_MMIO_STATUS, mmio_read32(VIRTIO_MMIO_STATUS) | status);
}

void virtio_fail() {
    if (g_mmio) {
        mmio_write32(VIRTIO_MMIO_STATUS,
                     mmio_read32(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
    }
}

u64 virtio_read_config64(u32 offset) {
    const u32 lo = mmio_read32(VIRTIO_MMIO_CONFIG + offset);
    const u32 hi = mmio_read32(VIRTIO_MMIO_CONFIG + offset + 4);
    return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
}

void* dma_alloc(u64 size, u64 align) {
    const u64 off = align_up_u64(g_dma_bump, align);
    if (off + size > g_dma_size) return nullptr;
    void* ptr = g_dma_base + off;
    g_dma_bump = off + size;
    libk::memset(ptr, 0, static_cast<usize>(size));
    return ptr;
}

u64 dma_phys_of(const void* ptr) {
    return g_dma_phys + (reinterpret_cast<u64>(ptr) -
                         reinterpret_cast<u64>(g_dma_base));
}

bool virtio_find_blk_mmio() {
    for (u32 off = 0; off < platform::kVirtioMmioSize; off += kVirtioMmioSlotSize) {
        auto* regs = reinterpret_cast<volatile u32*>(platform::kVirtioBlkMmioBase + off);
        if (regs[VIRTIO_MMIO_MAGIC / sizeof(u32)] == kVirtioMagicValue &&
            regs[VIRTIO_MMIO_DEVICE_ID / sizeof(u32)] == kVirtioBlkDeviceId) {
            g_mmio = reinterpret_cast<volatile u8*>(platform::kVirtioBlkMmioBase + off);
            return true;
        }
    }
    return false;
}

bool virtio_queue_init() {
    mmio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    if (!g_legacy_transport && mmio_read32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        mmio_write32(VIRTIO_MMIO_QUEUE_READY, 0);
    }

    const u32 queue_max = mmio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < kQueueSize) return false;

    if (g_legacy_transport) {
        u8* queue = static_cast<u8*>(dma_alloc(8192, 4096));
        if (!queue) return false;
        g_desc = reinterpret_cast<VringDesc*>(queue);
        g_avail = reinterpret_cast<VringAvail*>(queue + sizeof(VringDesc) * kQueueSize);
        g_used = reinterpret_cast<VringUsed*>(queue + 4096);
        mmio_write32(VIRTIO_MMIO_QUEUE_NUM, kQueueSize);
        mmio_write32(VIRTIO_MMIO_QUEUE_ALIGN, 4096);
        mmio_write32(VIRTIO_MMIO_QUEUE_PFN, static_cast<u32>(dma_phys_of(queue) >> 12));
    } else {
        g_desc = static_cast<VringDesc*>(dma_alloc(sizeof(VringDesc) * kQueueSize, 16));
        g_avail = static_cast<VringAvail*>(dma_alloc(sizeof(VringAvail), 16));
        g_used = static_cast<VringUsed*>(dma_alloc(sizeof(VringUsed), 16));
        if (!g_desc || !g_avail || !g_used) return false;
        mmio_write32(VIRTIO_MMIO_QUEUE_NUM, kQueueSize);
        mmio_write64_pair(VIRTIO_MMIO_QUEUE_DESC_LOW, dma_phys_of(g_desc));
        mmio_write64_pair(VIRTIO_MMIO_QUEUE_DRIVER_LOW, dma_phys_of(g_avail));
        mmio_write64_pair(VIRTIO_MMIO_QUEUE_DEVICE_LOW, dma_phys_of(g_used));
        mmio_write32(VIRTIO_MMIO_QUEUE_READY, 1);
    }

    g_req_hdr = static_cast<VirtioBlkHeader*>(dma_alloc(sizeof(VirtioBlkHeader), 16));
    g_req_status = static_cast<u8*>(dma_alloc(1, 1));
    g_data = static_cast<u8*>(dma_alloc(kMaxTransferSectors * kSectorSize, kSectorSize));
    if (!g_req_hdr || !g_req_status || !g_data) return false;

    g_data_offset = reinterpret_cast<u64>(g_data) - reinterpret_cast<u64>(g_dma_base);
    g_avail_idx = 0;
    g_used_idx = 0;
    g_avail->flags = 1u;
    g_avail->idx = 0;
    g_used->flags = 0;
    g_used->idx = 0;
    return true;
}

bool virtio_init() {
    if (!virtio_find_blk_mmio()) {
        ANKO_LOG_WARN("kernel/bootdisk", "virtio-blk mmio not found");
        return false;
    }

    const u32 version = mmio_read32(VIRTIO_MMIO_VERSION);
    if (version == 1) {
        g_legacy_transport = true;
    } else if (version == 2) {
        g_legacy_transport = false;
    } else {
        ANKO_LOGF_WARN("kernel/bootdisk", "unsupported virtio version=%u", version);
        return false;
    }

    constexpr usize kDmaPages = 4;
    g_dma_phys = mm::alloc_pages_contiguous(kDmaPages);
    if (!g_dma_phys) return false;
    g_dma_base = reinterpret_cast<u8*>(g_dma_phys);
    g_dma_size = kDmaPages * PAGE_SIZE;
    g_dma_bump = 0;
    libk::memset(g_dma_base, 0, static_cast<usize>(g_dma_size));

    mmio_write32(VIRTIO_MMIO_STATUS, 0);
    for (u32 spin = 0; spin < 1024; ++spin) {
        if (mmio_read32(VIRTIO_MMIO_STATUS) == 0) break;
    }

    virtio_add_status(VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_add_status(VIRTIO_STATUS_DRIVER);
    if (g_legacy_transport) {
        mmio_write32(VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);
    }

    const u32 dev_features0 = virtio_read_features(0);
    virtio_write_features(0, dev_features0 & VIRTIO_BLK_F_FLUSH);

    if (!g_legacy_transport) {
        const u32 dev_features1 = virtio_read_features(1);
        if ((dev_features1 & (1u << VIRTIO_F_VERSION_1_BIT)) == 0) {
            virtio_fail();
            return false;
        }
        virtio_write_features(1, (1u << VIRTIO_F_VERSION_1_BIT));
        virtio_add_status(VIRTIO_STATUS_FEATURES_OK);
        if ((mmio_read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
            virtio_fail();
            return false;
        }
    }

    if (!virtio_queue_init()) {
        virtio_fail();
        return false;
    }

    virtio_add_status(VIRTIO_STATUS_DRIVER_OK);
    g_capacity_sectors = virtio_read_config64(0);
    return g_capacity_sectors != 0;
}

bool virtio_submit_read(u64 sector, u32 sector_count) {
    if (!g_desc || !g_avail || !g_used || !g_req_hdr || !g_req_status || !g_data) return false;
    if (sector_count == 0 || sector_count > kMaxTransferSectors) return false;
    if (sector > g_capacity_sectors || sector_count > g_capacity_sectors - sector) return false;

    const u32 byte_count = sector_count * kSectorSize;
    libk::memset(g_data, 0, byte_count);

    g_req_hdr->type = VIRTIO_BLK_T_IN;
    g_req_hdr->reserved = 0;
    g_req_hdr->sector = sector;
    *g_req_status = 0xFFu;

    g_desc[0].addr = dma_phys_of(g_req_hdr);
    g_desc[0].len = sizeof(VirtioBlkHeader);
    g_desc[0].flags = VRING_DESC_F_NEXT;
    g_desc[0].next = 1;

    g_desc[1].addr = g_dma_phys + g_data_offset;
    g_desc[1].len = byte_count;
    g_desc[1].flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;
    g_desc[1].next = 2;

    g_desc[2].addr = dma_phys_of(g_req_status);
    g_desc[2].len = 1;
    g_desc[2].flags = VRING_DESC_F_WRITE;
    g_desc[2].next = 0;

    barrier();
    g_avail->ring[g_avail_idx % kQueueSize] = 0;
    barrier();
    ++g_avail_idx;
    g_avail->idx = g_avail_idx;
    barrier();
    mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    for (u32 spin = 0; spin < kVirtioWaitSpins; ++spin) {
        barrier();
        if (g_used->idx != g_used_idx) break;
    }
    barrier();
    if (g_used->idx == g_used_idx) return false;

    const VringUsedElem& used = g_used->ring[g_used_idx % kQueueSize];
    ++g_used_idx;
    (void)mmio_read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    mmio_write32(VIRTIO_MMIO_INTERRUPT_ACK, 0x3);
    barrier();

    return used.id == 0 && *g_req_status == VIRTIO_BLK_S_OK;
}

bool disk_read_sectors(u64 sector, u32 sector_count, void* out) {
    auto* dst = static_cast<u8*>(out);
    u32 done = 0;
    while (done < sector_count) {
        u32 chunk = sector_count - done;
        if (chunk > kMaxTransferSectors) chunk = kMaxTransferSectors;
        if (!virtio_submit_read(sector + done, chunk)) return false;
        libk::memcpy(dst + static_cast<usize>(done) * kSectorSize,
                     g_data,
                     static_cast<usize>(chunk) * kSectorSize);
        done += chunk;
    }
    return true;
}

bool gpt_name_eq(const u8* utf16le, const char* ascii) {
    u32 i = 0;
    for (; ascii[i]; ++i) {
        if (i * 2 + 1 >= GPT_NAME_BYTES) return false;
        if (utf16le[i * 2] != static_cast<u8>(ascii[i]) || utf16le[i * 2 + 1] != 0) {
            return false;
        }
    }
    if (i * 2 + 1 >= GPT_NAME_BYTES) return true;
    return utf16le[i * 2] == 0 && utf16le[i * 2 + 1] == 0;
}

bool load_gpt_root_partition(RootPartition* out) {
    if (!out) return false;
    if (!disk_read_sectors(GPT_HEADER_LBA, 1, g_sector)) return false;
    if (libk::memcmp(g_sector, "EFI PART", 8) != 0) return false;

    const u32 header_size = le32(g_sector + 12);
    const u32 header_crc = le32(g_sector + 16);
    if (header_size < 92 || header_size > kSectorSize) return false;
    u8 header_copy[kSectorSize];
    libk::memcpy(header_copy, g_sector, kSectorSize);
    header_copy[16] = header_copy[17] = header_copy[18] = header_copy[19] = 0;
    if (crc32_ieee(header_copy, header_size) != header_crc) {
        ANKO_LOG_WARN("kernel/bootdisk", "GPT header CRC mismatch");
        return false;
    }

    const u64 entries_lba = le64(g_sector + 72);
    const u32 entry_count = le32(g_sector + 80);
    const u32 entry_size = le32(g_sector + 84);
    const u32 entries_crc = le32(g_sector + 88);
    if (entry_size < GPT_ENTRY_SIZE || entry_size > 512 || entry_count == 0) return false;

    const u64 entries_size64 = static_cast<u64>(entry_count) * entry_size;
    if (entries_size64 == 0 || entries_size64 > 64ull * 1024ull) return false;
    const u32 entries_size = static_cast<u32>(entries_size64);
    const u32 entries_sectors = static_cast<u32>(align_up_u64(entries_size, kSectorSize) /
                                                 kSectorSize);
    u8* entries = static_cast<u8*>(mm::heap_alloc(entries_sectors * kSectorSize, 16));
    if (!entries) return false;
    if (!disk_read_sectors(entries_lba, entries_sectors, entries)) return false;
    if (crc32_ieee(entries, entries_size) != entries_crc) {
        ANKO_LOG_WARN("kernel/bootdisk", "GPT entry CRC mismatch");
        return false;
    }

    for (u32 i = 0; i < entry_count; ++i) {
        const u8* entry = entries + static_cast<usize>(i) * entry_size;
        bool empty = true;
        for (u32 b = 0; b < 16; ++b) empty = empty && entry[b] == 0;
        if (empty) continue;

        if (!gpt_name_eq(entry + GPT_NAME_OFFSET, "AMEROOT")) continue;
        const u64 first = le64(entry + 32);
        const u64 last = le64(entry + 40);
        if (first == 0 || last < first || last >= g_capacity_sectors) return false;
        out->first_lba = first;
        out->last_lba = last;
        return true;
    }

    return false;
}

bool fs_read_block(const FsState& fs, u64 block, u8* out) {
    if (!out || block >= fs.total_blocks) return false;
    const u64 sector = fs.root_lba + block * kSectorsPerFsBlock;
    if (sector < fs.root_lba || sector + kSectorsPerFsBlock > fs.root_lba + fs.root_sectors) {
        return false;
    }
    return disk_read_sectors(sector, kSectorsPerFsBlock, out);
}

bool amefs_mount(const RootPartition& part, FsState* fs) {
    if (!fs) return false;
    fs->root_lba = part.first_lba;
    fs->root_sectors = part.last_lba - part.first_lba + 1;
    fs->total_blocks = fs->root_sectors / kSectorsPerFsBlock;
    fs->inode_root = 0;
    if (fs->total_blocks < 4) return false;
    if (!fs_read_block(*fs, 0, g_block)) return false;

    if (le32(g_block) != AMEFS_MAGIC ||
        le32(g_block + 4) != AMEFS_VERSION ||
        le64(g_block + 16) != kFsBlockSize) {
        return false;
    }

    const u64 total_blocks = le64(g_block + 8);
    const u64 inode_root = le64(g_block + 24);
    if (total_blocks == 0 || total_blocks > fs->total_blocks || inode_root >= total_blocks) {
        return false;
    }
    fs->total_blocks = total_blocks;
    fs->inode_root = inode_root;

    if (!fs_read_block(*fs, 1, g_block2)) return false;
    const u32 checkpoint_crc = le32(g_block2 + 56);
    if (crc32_ieee(g_block2, 56) != checkpoint_crc) {
        ANKO_LOG_WARN("kernel/bootdisk", "AmeFS checkpoint CRC mismatch");
        return false;
    }
    const u64 checkpoint_inode_root = le64(g_block2 + 8);
    if (checkpoint_inode_root == 0 || checkpoint_inode_root >= fs->total_blocks) return false;
    fs->inode_root = checkpoint_inode_root;
    return true;
}

bool btree_find_inode(const FsState& fs, u64 root_block, u32 inode, InodeMeta* out) {
    if (!out || root_block == 0 || root_block >= fs.total_blocks) return false;
    u64 cur = root_block;
    for (u32 depth = 0; depth < kMaxBtreeDepth; ++depth) {
        if (!fs_read_block(fs, cur, g_block)) return false;
        const u16 node_type = le16(g_block);
        const u16 count = le16(g_block + 2);
        if (count > kMaxBtreeEntries) return false;
        if (node_type == BTREE_LEAF) {
            for (u32 i = 0; i < count; ++i) {
                const u8* e = g_block + 64 + i * 64;
                if (le32(e) != inode) continue;
                out->type = le16(e + 4);
                out->size = le64(e + 8);
                out->root_block = le64(e + 16);
                return true;
            }
            return false;
        }
        if (node_type != BTREE_INTERNAL) return false;
        u64 child = 0;
        for (u32 i = 0; i < count; ++i) {
            const u8* e = g_block + 64 + i * 64;
            if (inode <= le64(e)) {
                child = le64(e + 8);
                break;
            }
        }
        if (child == 0 || child >= fs.total_blocks) return false;
        cur = child;
    }
    return false;
}

bool btree_find_dir_child(const FsState& fs, u64 root_block, u32 parent_inode,
                          const char* name, DirChild* out) {
    if (!out || !name || root_block == 0 || root_block >= fs.total_blocks) return false;
    const u64 key = (static_cast<u64>(parent_inode) << 32) | name_hash(name);
    const u32 name_len = static_cast<u32>(libk::strlen(name));
    u64 cur = root_block;
    for (u32 depth = 0; depth < kMaxBtreeDepth; ++depth) {
        if (!fs_read_block(fs, cur, g_block)) return false;
        const u16 node_type = le16(g_block);
        const u16 count = le16(g_block + 2);
        if (count > kMaxBtreeEntries) return false;
        if (node_type == BTREE_LEAF) {
            for (u32 i = 0; i < count; ++i) {
                const u8* e = g_block + 64 + i * 64;
                if (le64(e) != key) continue;
                const u8 entry_name_len = e[12];
                if (entry_name_len != name_len || entry_name_len > 47) continue;
                if (libk::memcmp(e + 16, name, entry_name_len) != 0) continue;
                out->inode = le32(e + 8);
                out->type = e[13];
                return out->inode != 0;
            }
            return false;
        }
        if (node_type != BTREE_INTERNAL) return false;
        u64 child = 0;
        for (u32 i = 0; i < count; ++i) {
            const u8* e = g_block + 64 + i * 64;
            if (key <= le64(e)) {
                child = le64(e + 8);
                break;
            }
        }
        if (child == 0 || child >= fs.total_blocks) return false;
        cur = child;
    }
    return false;
}

bool resolve_path(const FsState& fs, InodeMeta* out) {
    static constexpr const char* kPath[] = {"ame", "boot", "system.mxb"};
    u32 inode = 1;
    InodeMeta meta{};
    if (!btree_find_inode(fs, fs.inode_root, inode, &meta) || meta.type != AMEFS_INODE_DIR) {
        return false;
    }

    for (u32 i = 0; i < 3; ++i) {
        DirChild child{};
        if (!btree_find_dir_child(fs, meta.root_block, inode, kPath[i], &child)) return false;
        if (!btree_find_inode(fs, fs.inode_root, child.inode, &meta)) return false;
        inode = child.inode;
        if (i < 2 && meta.type != AMEFS_INODE_DIR) return false;
    }

    if (meta.type != AMEFS_INODE_FILE || meta.size == 0 || meta.size > kMaxBundleSize) {
        return false;
    }
    *out = meta;
    return true;
}

bool read_extent_entry(const FsState& fs, const u8* e, u8* dst,
                       u64 file_size, u64 file_blocks, u64* blocks_read) {
    const u64 logical = le64(e);
    const u64 phys = le64(e + 8);
    const u64 length = le64(e + 16);
    const u32 data_crc = le32(e + 36);
    if (length != 1 || logical >= file_blocks || phys >= fs.total_blocks) return false;

    u8* block_dst = dst + static_cast<usize>(logical * kFsBlockSize);
    if (!fs_read_block(fs, phys, block_dst)) return false;
    if (crc32_ieee(block_dst, kFsBlockSize) != data_crc) {
        ANKO_LOG_WARN("kernel/bootdisk", "AmeFS extent CRC mismatch");
        return false;
    }
    (void)file_size;
    ++(*blocks_read);
    return true;
}

bool read_extent_tree(const FsState& fs, u64 root_block, u8* dst,
                      u64 file_size, u64 file_blocks, u64* blocks_read,
                      u32 depth = 0) {
    if (root_block == 0 || root_block >= fs.total_blocks || depth >= kMaxBtreeDepth) {
        return false;
    }
    if (!fs_read_block(fs, root_block, g_block)) return false;
    const u16 node_type = le16(g_block);
    const u16 count = le16(g_block + 2);
    if (count > kMaxBtreeEntries) return false;

    if (node_type == BTREE_LEAF) {
        for (u32 i = 0; i < count; ++i) {
            if (!read_extent_entry(fs, g_block + 64 + i * 64,
                                   dst, file_size, file_blocks, blocks_read)) {
                return false;
            }
        }
        return true;
    }
    if (node_type != BTREE_INTERNAL) return false;

    u64 children[kMaxBtreeEntries]{};
    for (u32 i = 0; i < count; ++i) {
        children[i] = le64(g_block + 64 + i * 64 + 8);
        if (children[i] == 0 || children[i] >= fs.total_blocks) return false;
    }
    for (u32 i = 0; i < count; ++i) {
        if (!read_extent_tree(fs, children[i], dst, file_size, file_blocks,
                              blocks_read, depth + 1)) {
            return false;
        }
    }
    return true;
}

bool read_file_to_pages(const FsState& fs, const InodeMeta& file,
                        PhysAddr* out_base, usize* out_size) {
    if (!out_base || !out_size || file.size == 0 || file.size > kMaxBundleSize) return false;
    const u64 file_blocks = align_up_u64(file.size, kFsBlockSize) / kFsBlockSize;
    const u64 pages = align_up_u64(file.size, PAGE_SIZE) / PAGE_SIZE;
    if (file_blocks == 0 || pages == 0 || pages > (kMaxBundleSize / PAGE_SIZE)) return false;

    const PhysAddr base = mm::alloc_pages_contiguous(static_cast<usize>(pages));
    if (!base) return false;
    auto* dst = reinterpret_cast<u8*>(base);
    libk::memset(dst, 0, static_cast<usize>(pages * PAGE_SIZE));

    u64 blocks_read = 0;
    if (!read_extent_tree(fs, file.root_block, dst, file.size, file_blocks, &blocks_read)) {
        return false;
    }
    if (blocks_read < file_blocks) return false;

    *out_base = base;
    *out_size = static_cast<usize>(file.size);
    return true;
}

} // namespace

bool load_system_bundle_from_disk(PhysAddr* out_base, usize* out_size) {
    if (!out_base || !out_size) return false;
    *out_base = 0;
    *out_size = 0;

    if (!virtio_init()) return false;

    RootPartition root{};
    if (!load_gpt_root_partition(&root)) {
        ANKO_LOG_WARN("kernel/bootdisk", "AMEROOT GPT partition not found");
        return false;
    }

    FsState fs{};
    if (!amefs_mount(root, &fs)) {
        ANKO_LOG_WARN("kernel/bootdisk", "AmeFS mount failed");
        return false;
    }

    InodeMeta bundle{};
    if (!resolve_path(fs, &bundle)) {
        ANKO_LOG_WARN("kernel/bootdisk", "/ame/boot/system.mxb not found");
        return false;
    }

    if (!read_file_to_pages(fs, bundle, out_base, out_size)) {
        ANKO_LOG_WARN("kernel/bootdisk", "system.mxb read failed");
        return false;
    }

    ANKO_LOGF_INFO("kernel/bootdisk",
                   "loaded /ame/boot/system.mxb size=%llu root_lba=%llu",
                   static_cast<unsigned long long>(*out_size),
                   static_cast<unsigned long long>(root.first_lba));
    return true;
}

} // namespace kernel::init
