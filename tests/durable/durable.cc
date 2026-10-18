// tests/durable/durable.cc — durability persistence fixture (service
// app.durable).
//
// Run as an ordinary userland service that declares `uses: ["fs"]`. It is
// driven by scripts/test/durable-persistence.py across two QEMU boots that share
// one writable disk image:
//
//   boot 1  writes /data/persist.txt, calls FS_SYNC, prints
//           "durability: write phase complete", then idles (the runner stops it)
//   boot 2  opens /data/persist.txt, reads it back, and asserts the exact bytes
//           survived, printing "durability regression passed"
//
// The marker strings are the only contract with the runner. A missing /data or a
// non-durable backend prints "durability: unavailable" and never claims success.

#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/endpoint.h"
#include "mochi/fs.h"
#include "mochi/syscall.h"
#include "midix/abi/protocols/fs.h"

constexpr mx_vaddr_t kVaLogRing = 0x1800'0000ull;
constexpr mx_vaddr_t kVaFsTx    = 0x2A020000ull;
constexpr mx_vaddr_t kVaFsRx    = 0x2A030000ull;

// io.fs exposes the root directory through the well-known handle 1.
constexpr fs_handle_t kRootHandle = 1;

constexpr uint32_t kWaitSpins = 400000;

// Exactly 32 bytes: one 24-byte write plus one 8-byte write, read back in one
// FS_INLINE_READ_MAX-sized read.
static const char kPayload[] = "MIDIX-DURABLE-PAYLOAD-0123456789";
constexpr uint32_t kPayloadLen = sizeof(kPayload) - 1; // 32
static_assert(kPayloadLen == 32, "payload must be 32 bytes");
static_assert(kPayloadLen <= FS_INLINE_READ_MAX, "payload must fit one inline read");

static void put(const char* text) {
    uint32_t n = 0;
    while (text[n]) ++n;
    sys_debug_puts(text, n);
}

static void putln(const char* text) {
    put(text);
    put("\n");
}

static bool bytes_equal(const unsigned char* a, const char* b, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        if (a[i] != static_cast<unsigned char>(b[i])) return false;
    }
    return true;
}

static RingPage* map_ring(const MxBootInfo* boot, const char* name, mx_vaddr_t va,
                          uint32_t* order) {
    const MxBootEntry* entry = mochi::boot::find_type(boot, name, MX_BOOT_ENTRY_RING);
    if (!entry || entry->handle == MX_NULL_HANDLE) return nullptr;
    if (mx_map_page(entry->handle, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) return nullptr;
    if (order) *order = entry->aux0 ? static_cast<uint32_t>(entry->aux0) : MX_DEFAULT_RING_ORDER;
    return reinterpret_cast<RingPage*>(va);
}

static bool wire_fs(const MxBootInfo* boot, mochi::fs::Client* out) {
    uint32_t order = MX_DEFAULT_RING_ORDER;
    RingPage* tx = map_ring(boot, "fs.cmd.ring", kVaFsTx, &order);
    RingPage* rx = map_ring(boot, "fs.reply.ring", kVaFsRx, nullptr);
    const mx_handle_t tx_db = mochi::boot::handle(boot, "fs.cmd.doorbell");
    const mx_handle_t rx_db = mochi::boot::handle(boot, "fs.reply.doorbell");
    if (!tx || !rx || tx_db == MX_NULL_HANDLE || rx_db == MX_NULL_HANDLE) return false;
    out->ep = mochi::SpscEndpoint{tx, rx, order, tx_db, rx_db};
    return out->valid();
}

[[noreturn]] static void idle_forever() {
    while (true) sys_yield();
}

[[noreturn]] static void fail(const char* message) {
    put("TEST FAILED: ");
    putln(message);
    putln("durability regression failed");
    idle_forever();
}

alignas(4096) static unsigned char large_buffer[FS_TRANSFER_MAX];
constexpr uint32_t kLargeBytes = 3 * 1024 * 1024;
static unsigned char pattern(uint64_t at) { return static_cast<unsigned char>(at * 17 + (at >> 16) + 3); }
static void release_view(mx_handle_t cap, mx_vaddr_t address) {
    (void)sys_unmap_page(address);
    (void)sys_cap_revoke(cap);
}
static void large_volume(mochi::fs::Client& fs, fs_handle_t parent, bool verify_only) {
    FsOpenReply file{};
    const auto flags = verify_only ? FS_OPEN_READ : FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNC;
    if (!fs.wait_open(parent, "full-volume.bin", flags, &file) || file.status != MX_OK) fail("large volume open");
    if (!verify_only) {
        for (uint32_t offset = 0; offset < kLargeBytes; offset += sizeof(large_buffer)) {
            for (uint32_t i = 0; i < sizeof(large_buffer); ++i) large_buffer[i] = pattern(offset + i);
            FsWriteReply result{};
            if (!fs.try_write(file.handle, offset, large_buffer, sizeof(large_buffer), &result) ||
                result.status != MX_OK || result.bytes_written != sizeof(large_buffer)) fail("64 KiB arena write");
        }
        // Two independently pinned immutable views exhaust this client's cache
        // quota. Revoking a cap alone must not permit reuse of a live mapping.
        FsReadReply old{}, current{}, busy{};
        if (!fs.try_read(file.handle, 0, 4096, &old) || old.status != MX_OK || !old.arena_cap) fail("arena read");
        if (mx_map_page(old.arena_cap, 0x2f000000, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_ERR_PERM) fail("read arena writable");
        if (mx_map_page(old.arena_cap, 0x2f000000, MX_RIGHT_READ) != MX_OK) fail("read arena map");
        const unsigned char changed = 0xee;
        FsWriteReply write{};
        if (!fs.try_write(file.handle, 0, &changed, 1, &write) || write.status != MX_OK) fail("CoW update");
        if (*reinterpret_cast<const volatile unsigned char*>(0x2f000000) != pattern(0)) fail("pinned read view changed");
        if (!fs.try_read(file.handle, 0, 4096, &current) || current.status != MX_OK || !current.arena_cap) fail("fresh read view");
        if (mx_map_page(current.arena_cap, 0x2f001000, MX_RIGHT_READ) != MX_OK ||
            *reinterpret_cast<const volatile unsigned char*>(0x2f001000) != changed) fail("fresh read view stale");
        if (!fs.try_read(file.handle, 4096, 4096, &busy) || busy.status != MX_ERR_BUSY) fail("read view quota");
        (void)sys_cap_revoke(old.arena_cap);
        if (!fs.try_read(file.handle, 4096, 4096, &busy) || busy.status != MX_ERR_BUSY) fail("mapped view prematurely reused");
        (void)sys_unmap_page(0x2f000000);
        if (!fs.try_read(file.handle, 4096, 4096, &busy) || busy.status != MX_OK || !busy.arena_cap) fail("released view not reused");
        (void)sys_cap_revoke(busy.arena_cap);
        release_view(current.arena_cap, 0x2f001000);
        const auto original = pattern(0);
        if (!fs.try_write(file.handle, 0, &original, 1, &write) || write.status != MX_OK) fail("restore first byte");
        mx_i32_t status = MX_ERR_FAULT;
        if (!fs.try_sync(&status) || status != MX_OK) fail("large volume sync");
        putln("durability: 3 MiB written; immutable arena lifetimes verified");
    }
    for (uint32_t offset = 0; offset < kLargeBytes; offset += sizeof(large_buffer)) {
        uint32_t received = 0;
        if (!fs.read_bytes(file.handle, offset, large_buffer, sizeof(large_buffer), &received) ||
            received != sizeof(large_buffer)) fail("large arena read-back");
        for (uint32_t i = 0; i < received; ++i) if (large_buffer[i] != pattern(offset + i)) fail("large read-back bytes");
    }
    (void)fs.try_close(file.handle);
    putln("durability: 3 MiB exact read-back passed");
}

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "app.durable");
    // Declare readiness before any optional work so a slow peer can never stall
    // init's dependency ordering.
    mochi::boot::ready(boot);

    mochi::fs::Client fs{};
    for (uint32_t spins = 0; spins < kWaitSpins && !wire_fs(boot, &fs); ++spins) {
        sys_yield();
    }
    if (fs.valid() && !fs.map_write_arena(boot)) fail("write arena unavailable");
    if (!fs.valid()) {
        putln("durability: unavailable (no io.fs session)");
        idle_forever();
    }

    // Resolve /data.
    FsOpenReply data{};
    for (uint32_t spins = 0; spins < kWaitSpins; ++spins) {
        if (fs.wait_open(kRootHandle, "data", FS_OPEN_READ, &data)) break;
        sys_yield();
    }
    if (data.status != MX_OK) {
        putln("durability: unavailable (no /data volume)");
        idle_forever();
    }

    // Phase 2: if the file is already there, verify it byte for byte.
    FsOpenReply existing{};
    if (fs.wait_open(data.handle, "persist.txt", FS_OPEN_READ, &existing) &&
        existing.status == MX_OK) {
        FsReadReply read{};
        if (!fs.try_read(existing.handle, 0, kPayloadLen, &read) ||
            read.status != MX_OK || read.length_read != kPayloadLen) {
            fail("read of persisted file failed");
        }
        if (!bytes_equal(read.inline_data, kPayload, kPayloadLen)) {
            fail("persisted bytes differ from what was written");
        }
        (void)fs.try_close(existing.handle);
        putln("durability: read-back OK");
        large_volume(fs, data.handle, true);
        (void)fs.try_close(data.handle);
        putln("durability regression passed");
        idle_forever();
    }

    // Phase 1: create the file, write the payload, and sync.
    FsOpenReply created{};
    if (!fs.wait_open(data.handle, "persist.txt",
                      FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNC,
                      &created) ||
        created.status != MX_OK) {
        fail("create of /data/persist.txt failed");
    }
    FsWriteReply write{};
    if (!fs.try_write(created.handle, 0, kPayload, 24, &write) ||
        write.status != MX_OK || write.bytes_written != 24) {
        fail("first write failed");
    }
    if (!fs.try_write(created.handle, 24, kPayload + 24, 8, &write) ||
        write.status != MX_OK || write.bytes_written != 8) {
        fail("second write failed");
    }
    mx_i32_t sync_status = MX_ERR_NOT_IMPLEMENTED;
    if (!fs.try_sync(&sync_status)) {
        fail("sync request failed");
    }
    if (sync_status != MX_OK) {
        putln("durability: unavailable (sync reports a non-durable backend)");
        (void)fs.try_close(created.handle);
        (void)fs.try_close(data.handle);
        idle_forever();
    }

    // Read the bytes back through the service to confirm the write landed.
    FsReadReply read{};
    if (!fs.try_read(created.handle, 0, kPayloadLen, &read) ||
        read.status != MX_OK || read.length_read != kPayloadLen ||
        !bytes_equal(read.inline_data, kPayload, kPayloadLen)) {
        fail("in-memory read-back after write failed");
    }
    large_volume(fs, data.handle, false);
    (void)fs.try_close(created.handle);
    (void)fs.try_close(data.handle);
    putln("durability: wrote and synced 32 bytes to /data/persist.txt");
    putln("durability: write phase complete");
    idle_forever();
}
