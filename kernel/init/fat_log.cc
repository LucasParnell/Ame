// kernel/init/fat_log.cc — Minimal FAT32 boot log writer.
//
// Locates /ame/log/boot.log on the microSD FAT32 partition (SDMMC1) using
// 8.3 name matching, accumulates kernel output in a 64 KB DRAM buffer, and
// writes it to the file's data sectors at flush() time.
//
// FAT32 layout cross-referenced against:
//   Microsoft FAT32 File System Specification rev 1.03
//   Linux fs/fat/fatent.c, fs/fat/dir.c
//   U-Boot fs/fat/fat.c
//
// Setup required on host PC before first use:
//   mkdir -p <SD>/ame/log
//   dd if=/dev/zero of=<SD>/ame/log/boot.log bs=512 count=128
// The kernel writes at most 128 sectors (64 KB).  If the file is smaller,
// output is silently truncated.

#include "anko/fat_log.h"
#include "anko/console.h"
#include "anko/sdmmc.h"
#include "anko/types.h"
#include "libk/string.h"

namespace fat_log {

namespace {

// ---------------------------------------------------------------------------
// Log ring buffer (64 KB, in .bss)
// ---------------------------------------------------------------------------
constexpr usize kBufSize = 64u * 1024u;
static char g_buf[kBufSize];
static u32  g_len = 0;
static bool g_ready = false;      // SDMMC + file located OK
static u32  g_file_lba  = 0;      // first LBA of boot.log data
static u32  g_file_secs = 0;      // number of writable sectors in the file

// Scratch sector buffer (512 bytes, re-used for MBR/BPB/dir reads).
static u8 g_sec[512];

// ---------------------------------------------------------------------------
// FAT32 helpers — all values little-endian
// ---------------------------------------------------------------------------
static inline u16 le16(const u8* p) {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8u);
}
static inline u32 le32(const u8* p) {
    return static_cast<u32>(p[0])        |
           (static_cast<u32>(p[1]) << 8u) |
           (static_cast<u32>(p[2]) << 16u)|
           (static_cast<u32>(p[3]) << 24u);
}

// FAT32 BPB fields (parsed once)
static u32 g_part_lba   = 0; // partition start LBA (from MBR)
static u32 g_fat_lba    = 0; // first FAT sector LBA
static u32 g_data_lba   = 0; // first data sector LBA (cluster 2 base)
static u32 g_spc        = 0; // sectors per cluster
static u32 g_fat_secs   = 0; // sectors per FAT copy

// Cluster → LBA.
static u32 cluster_to_lba(u32 cluster) {
    return g_data_lba + (cluster - 2u) * g_spc;
}

// Follow the FAT32 chain from `cluster`; return next cluster (0 = done).
static u32 fat_next(u32 cluster) {
    const u32 fat_sector_off = cluster / 128u;  // 128 FAT32 entries per 512 B sector
    const u32 fat_sector_lba = g_fat_lba + fat_sector_off;
    if (!drivers::sdmmc::read_sector(fat_sector_lba, g_sec)) return 0u;
    const u32 entry = le32(g_sec + (cluster % 128u) * 4u) & 0x0FFFFFFFu;
    return (entry >= 0x0FFFFFF8u) ? 0u : entry; // 0x0FFFFFF8..F = end-of-chain
}

// ---------------------------------------------------------------------------
// 8.3 directory entry matching (case-insensitive via uppercase comparison).
// dir_entry points to the 32-byte directory entry.
// name8: 8 chars space-padded uppercase.  ext3: 3 chars space-padded uppercase.
// ---------------------------------------------------------------------------
static bool match_83(const u8* e, const char* name8, const char* ext3) {
    for (int i = 0; i < 8; ++i)
        if (e[i] != static_cast<u8>(name8[i])) return false;
    for (int i = 0; i < 3; ++i)
        if (e[8 + i] != static_cast<u8>(ext3[i])) return false;
    return true;
}

static u32 dir_entry_cluster(const u8* e) {
    return (static_cast<u32>(le16(e + 20u)) << 16u) | le16(e + 26u);
}

// Scan a directory (starting cluster) for a 8.3 name.
// Returns the first cluster of the found entry (dir or file), or 0.
// If `is_dir` != nullptr, sets it to true for directory entries.
// If `file_size` != nullptr, fills size in bytes.
static u32 dir_find(u32 start_cluster, const char* name8, const char* ext3,
                    bool* is_dir = nullptr, u32* file_size = nullptr) {
    u32 cluster = start_cluster;
    while (cluster != 0u) {
        const u32 lba = cluster_to_lba(cluster);
        for (u32 s = 0u; s < g_spc; ++s) {
            if (!drivers::sdmmc::read_sector(lba + s, g_sec)) return 0u;
            for (u32 i = 0u; i < 512u; i += 32u) {
                const u8* e = g_sec + i;
                if (e[0] == 0x00u) return 0u;  // end of directory
                if (e[0] == 0xE5u) continue;   // deleted entry
                if (e[11] == 0x0Fu) continue;  // LFN entry, skip
                if (match_83(e, name8, ext3)) {
                    if (is_dir)    *is_dir    = (e[11] & 0x10u) != 0u;
                    if (file_size) *file_size = le32(e + 28u);
                    return dir_entry_cluster(e);
                }
            }
        }
        cluster = fat_next(cluster);
    }
    return 0u;
}

// ---------------------------------------------------------------------------
// Secondary console sink — appends every character to g_buf.
// ---------------------------------------------------------------------------
static void sink(char c) {
    if (g_len < kBufSize) g_buf[g_len++] = c;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init() {
    // 1. Initialise SDMMC1.
    if (!drivers::sdmmc::init()) return false;

    // 2. Read MBR (LBA 0) — find first FAT32 partition (type 0x0B or 0x0C).
    if (!drivers::sdmmc::read_sector(0u, g_sec)) return false;
    if (le16(g_sec + 510u) != 0xAA55u) return false; // bad MBR signature

    g_part_lba = 0u;
    for (u32 i = 0u; i < 4u; ++i) {
        const u8* pe = g_sec + 0x1BEu + i * 16u;
        const u8  type = pe[4];
        if (type == 0x0Bu || type == 0x0Cu || type == 0x0Eu) {
            g_part_lba = le32(pe + 8u);
            break;
        }
    }
    if (g_part_lba == 0u) return false;

    // 3. Read FAT32 BPB (Volume Boot Record at partition start).
    if (!drivers::sdmmc::read_sector(g_part_lba, g_sec)) return false;
    if (g_sec[510] != 0x55u || g_sec[511] != 0xAAu) return false;

    const u16 bytes_per_sector = le16(g_sec + 0x0Bu);
    if (bytes_per_sector != 512u) return false; // only 512-byte sectors supported

    g_spc           = g_sec[0x0D];                       // sectors per cluster
    const u16 rsvd  = le16(g_sec + 0x0Eu);               // reserved sectors
    const u8  nfats = g_sec[0x10];                        // number of FATs (usually 2)
    g_fat_secs      = le32(g_sec + 0x24u);                // FAT size in sectors (FAT32)
    const u32 root_cluster = le32(g_sec + 0x2Cu);         // root dir cluster (usually 2)

    if (g_spc == 0u || g_fat_secs == 0u || root_cluster < 2u) return false;

    g_fat_lba  = g_part_lba + rsvd;
    g_data_lba = g_fat_lba + static_cast<u32>(nfats) * g_fat_secs;

    // 4. Navigate /ame/log/boot.log using 8.3 names (space-padded, uppercase).
    const u32 ame_cluster = dir_find(root_cluster, "AME     ", "   ");
    if (ame_cluster == 0u) return false;

    const u32 log_cluster = dir_find(ame_cluster, "LOG     ", "   ");
    if (log_cluster == 0u) return false;

    u32 file_sz = 0u;
    const u32 file_cluster = dir_find(log_cluster, "BOOT    ", "LOG", nullptr, &file_sz);
    if (file_cluster == 0u || file_sz == 0u) return false;

    g_file_lba  = cluster_to_lba(file_cluster);
    g_file_secs = file_sz / 512u;
    if (g_file_secs == 0u) return false;

    // Cap at kBufSize worth of sectors.
    if (g_file_secs > kBufSize / 512u) g_file_secs = kBufSize / 512u;

    g_ready = true;

    // 5. Install console secondary sink so all subsequent putchar output is
    //    captured in g_buf (UART continues to work in parallel).
    console::set_secondary_sink(sink);

    return true;
}

void flush() {
    if (!g_ready || g_len == 0u) return;

    // Round up to sector boundary; zero-fill the tail of the last sector.
    const u32 sectors_needed = (g_len + 511u) / 512u;
    const u32 sectors_write  = sectors_needed < g_file_secs ? sectors_needed : g_file_secs;

    for (u32 s = 0u; s < sectors_write; ++s) {
        u8 sector_buf[512];
        const u32 off = s * 512u;
        const u32 remain = g_len > off ? g_len - off : 0u;
        const u32 copy = remain < 512u ? remain : 512u;
        libk::memcpy(sector_buf, g_buf + off, copy);
        if (copy < 512u) libk::memset(sector_buf + copy, 0, 512u - copy);
        drivers::sdmmc::write_sector(g_file_lba + s, sector_buf);
    }
}

void write(const char* s) {
    if (!s) return;
    while (*s) sink(*s++);
}

} // namespace fat_log
