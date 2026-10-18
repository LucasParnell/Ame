// userland/profiler/main.cc
//
// Small fs proxy for init. init keeps supervision (and therefore the right to
// read child counters and set child budgets); this service only touches the
// profile file, because init has no io.fs session.
//
// Transport is one shared page each way plus two doorbells. File I/O is chunked
// inline reads/writes (<= 32 / <= 24 bytes) so the arena grant path never runs
// during boot - that path is what failed first time round.
#include "mochi/boot.h"
#include "mochi/fs.h"
#include "mochi/syscall.h"
#include "midix/abi/protocols/fs.h"

namespace {

constexpr mx_vaddr_t kLoadVa = 0x1a000000;
constexpr mx_vaddr_t kStoreVa = 0x1a010000;
constexpr mx_u32_t kOpIdle = 0, kOpLoad = 1, kOpStore = 2;
constexpr mx_u32_t kMaxProfile = 1024;
constexpr mx_u32_t kRootInode = 1;
constexpr mx_u32_t kReadChunk = sizeof(FsReadReply::inline_data);   // 32
constexpr mx_u32_t kWriteChunk = FS_INLINE_WRITE_MAX;               // 24

struct Mailbox {
    mx_u32_t op;
    mx_u32_t len;
    mx_u8_t data[kMaxProfile];
};

Mailbox* g_load = nullptr;
Mailbox* g_store = nullptr;
CapHandle g_req = 0, g_ack = 0;
mochi::fs::Client g_fs;

void log(const char* text, mx_u32_t value) {
    sys_debug_puts("ipc.profiler: ", 14);
    sys_debug_puts(text, libk::str::length(text));
    sys_debug_puts("=", 1);
    char digits[12];
    mx_u32_t n = 0;
    if (!value) digits[n++] = '0';
    while (value) { digits[n++] = static_cast<char>('0' + value % 10); value /= 10; }
    char out[12];
    for (mx_u32_t i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    sys_debug_puts(out, n);
    sys_debug_puts("\n", 1);
}

bool data_dir(FsOpenReply* dir) {
    if (!g_fs.wait_open(kRootInode, "data", FS_OPEN_READ, dir)) { log("dir.no_reply", 0); return false; }
    if (dir->status != MX_OK) { log("dir.status", static_cast<mx_u32_t>(dir->status)); return false; }
    return dir->type == FS_INODE_DIR;
}

// Inline-only read: each request is at most 32 bytes, which the reply carries
// in inline_data, so no reply arena is ever granted or mapped.
mx_u32_t load_file() {
    FsOpenReply dir{};
    if (!data_dir(&dir)) return 0;
    FsOpenReply file{};
    if (!g_fs.wait_open(dir.handle, "ipc-profile", FS_OPEN_READ, &file) || file.status != MX_OK) {
        log("open.status", static_cast<mx_u32_t>(file.status));
        return 0;
    }
    mx_u32_t total = 0;
    while (total < kMaxProfile) {
        FsReadReply reply{};
        const mx_u32_t want = kMaxProfile - total < kReadChunk ? kMaxProfile - total : kReadChunk;
        if (!g_fs.try_read(file.handle, total, want, &reply) || reply.status != MX_OK) {
            log("read.fail", total);
            break;
        }
        if (!reply.length_read) break;
        for (mx_u32_t i = 0; i < reply.length_read; ++i) g_load->data[total + i] = reply.inline_data[i];
        total += reply.length_read;
    }
    (void)g_fs.try_close(file.handle);
    return total;
}

// Inline-only write, same reasoning: <= 24 bytes per request.
bool store_file(mx_u32_t length) {
    FsOpenReply dir{};
    if (!data_dir(&dir)) return false;
    FsOpenReply file{};
    const mx_u32_t flags = FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNC;
    if (!g_fs.wait_open(dir.handle, "ipc-profile", flags, &file) || file.status != MX_OK) return false;
    bool ok = true;
    for (mx_u32_t off = 0; off < length && ok; off += kWriteChunk) {
        const mx_u32_t chunk = length - off < kWriteChunk ? length - off : kWriteChunk;
        FsWriteReply reply{};
        ok = g_fs.try_write(file.handle, off, g_store->data + off, chunk, &reply) &&
             reply.status == MX_OK && reply.bytes_written == chunk;
    }
    (void)g_fs.try_close(file.handle);
    return ok;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    const CapHandle load_cap = mochi::boot::require_handle(boot, "prof.load");
    const CapHandle store_cap = mochi::boot::require_handle(boot, "prof.store");
    if (mx_map_page(load_cap, kLoadVa, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) mx_exit(1);
    if (mx_map_page(store_cap, kStoreVa, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) mx_exit(1);
    g_load = reinterpret_cast<Mailbox*>(kLoadVa);
    g_store = reinterpret_cast<Mailbox*>(kStoreVa);
    g_req = mochi::boot::require_handle(boot, "prof.req");
    g_ack = mochi::boot::require_handle(boot, "prof.ack");

    g_fs.ep = mochi::boot::map_endpoint(boot,
        {"fs.cmd.ring", 0x1a020000, "fs.cmd.doorbell"},
        {"fs.reply.ring", 0x1a030000, "fs.reply.doorbell"});
    if (!g_fs.valid()) {
        log("fs.unavailable", 1);
        for (;;) sys_yield();
    }
    if (const auto* ready = mochi::boot::find(boot, "service.ready"))
        (void)mx_doorbell_notify(ready->handle);
    log("ready", 1);

    for (;;) {
        // Check the mailbox before snapshotting the counter: a request posted
        // just before the snapshot would otherwise be waited past forever.
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (g_store->op == kOpIdle) {
            const mx_u64_t counter = mx_doorbell_read(g_req);
            // Block, do not spin: init always notifies after posting the op, so
            // the counter is the only wake source and this cannot miss a
            // request. The spin cost a permanent slice of one CPU at idle.
            if (g_store->op == kOpIdle) (void)mx_doorbell_wait(g_req, counter);
            continue;
        }
        const mx_u32_t op = g_store->op;
        if (op == kOpLoad) {
            const mx_u32_t length = load_file();
            g_load->len = length;
            log("loaded", length);
        } else if (op == kOpStore) {
            const mx_u32_t length = g_store->len > kMaxProfile ? kMaxProfile : g_store->len;
            log("stored", store_file(length) ? length : 0);
        }
        __atomic_store_n(&g_store->op, kOpIdle, __ATOMIC_RELEASE);
        (void)mx_doorbell_notify(g_ack);
    }
}
