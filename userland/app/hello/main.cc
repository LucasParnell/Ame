// userland/app/hello/main.cc — example Mochi application (service app.hello).
//
// The smallest useful userland program.  It discovers everything it may touch
// from BootInfo (mochi/boot.h) instead of fixed capability-slot folklore, logs
// through the manifest's log.ring (mochi/boot_log.h + mochi/log.h), and drives
// the sessions that the manifest `uses` field wires up: it resolves a peer
// through the nameservice and enumerates the filesystem root.
//
// It is registered with mochi_service(app.hello ...) in userland/CMakeLists.txt
// and listed in userland/manifest.v0, so its startup is observable in the boot
// log as `[I] app.hello: example application ready`.

#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/endpoint.h"
#include "mochi/log.h"
#include "mochi/nameservice.h"
#include "mochi/syscall.h"
#include "midix/abi/protocols/fs.h"
#include "midix/abi/protocols/nameservice.h"

constexpr mx_vaddr_t kVaLogRing = 0x1800'0000ull;
constexpr mx_vaddr_t kVaNsTx    = 0x2A000000ull;
constexpr mx_vaddr_t kVaNsRx    = 0x2A010000ull;
constexpr mx_vaddr_t kVaFsTx    = 0x2A020000ull;
constexpr mx_vaddr_t kVaFsRx    = 0x2A030000ull;

// io.fs exposes the root directory through the well-known handle 1.
constexpr fs_handle_t kRootHandle = 1;

// Bounded waits: an example must never wedge the boot if a peer is slow.
constexpr uint32_t kWaitSpins = 200000;

static RingPage* map_ring(const MxBootInfo* boot, const char* name, mx_vaddr_t va,
                          uint32_t* order) {
    const MxBootEntry* entry = mochi::boot::find_type(boot, name, MX_BOOT_ENTRY_RING);
    if (!entry || entry->handle == MX_NULL_HANDLE) return nullptr;
    if (mx_map_page(entry->handle, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) return nullptr;
    if (order) *order = entry->aux0 ? static_cast<uint32_t>(entry->aux0) : MX_DEFAULT_RING_ORDER;
    return reinterpret_cast<RingPage*>(va);
}

// Build an SPSC endpoint from the BootInfo names a manifest session grants.
static bool wire(const MxBootInfo* boot,
                 const char* tx_name, mx_vaddr_t tx_va,
                 const char* rx_name, mx_vaddr_t rx_va,
                 const char* tx_db_name, const char* rx_db_name,
                 mochi::SpscEndpoint* out) {
    uint32_t order = MX_DEFAULT_RING_ORDER;
    RingPage* tx = map_ring(boot, tx_name, tx_va, &order);
    RingPage* rx = map_ring(boot, rx_name, rx_va, nullptr);
    const mx_handle_t tx_db = mochi::boot::handle(boot, tx_db_name);
    const mx_handle_t rx_db = mochi::boot::handle(boot, rx_db_name);
    if (!tx || !rx || tx_db == MX_NULL_HANDLE || rx_db == MX_NULL_HANDLE) return false;
    *out = mochi::SpscEndpoint{tx, rx, order, tx_db, rx_db};
    return out->valid();
}

static bool nameservice_resolve(const mochi::SpscEndpoint& ns, const char* name) {
    if (!mochi::nameservice::try_resolve_request(ns, name)) return false;
    RingSlot slot{};
    for (uint32_t spins = 0; spins < kWaitSpins; ++spins) {
        if (ns.try_recv(&slot)) {
            if (slot.type == MSG_NS_RESOLVE_REPLY) return true;
            if (slot.type == MSG_NS_ERROR) return false;
        }
        sys_yield();
    }
    return false;
}

static uint32_t fs_root_entry_count(const mochi::SpscEndpoint& fs) {
    FsReaddirRequest req{};
    req.handle = kRootHandle;
    if (!fs.send_spinning(FS_READDIR, &req, sizeof(req))) return 0;
    RingSlot slot{};
    for (uint32_t spins = 0; spins < kWaitSpins; ++spins) {
        if (fs.try_recv(&slot)) {
            if (slot.type == FS_READDIR_REPLY && slot.length >= sizeof(FsReaddirReply)) {
                FsReaddirReply reply{};
                for (uint32_t i = 0; i < sizeof(reply); ++i) {
                    reinterpret_cast<uint8_t*>(&reply)[i] = slot.payload[i];
                }
                return reply.status == MX_OK ? reply.entry_count : 0;
            }
            if (slot.type == FS_ERROR) return 0;
        }
        sys_yield();
    }
    return 0;
}

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "app.hello");

    // Report readiness before any optional work so a slow peer can never stall
    // init's dependency ordering.
    mochi::boot::ready(boot);
    mochi::log_raw_marker("example application ready\n");

    // Declared `uses` session: nameservice.  Resolve a peer, then log whether
    // the round trip succeeded.  Failure is reported, not fatal.
    mochi::SpscEndpoint ns{};
    const bool ns_wired = wire(boot, "ns.tx", kVaNsTx, "ns.rx", kVaNsRx,
                               "ns.server.db", "ns.reply.db", &ns);
    mochi::log_raw_marker(ns_wired && nameservice_resolve(ns, "io.fs")
        ? "nameservice: resolved io.fs\n"
        : "nameservice: session unavailable\n");

    // Declared `uses` session: io.fs.  Enumerate the root directory.
    mochi::SpscEndpoint fs_session{};
    if (wire(boot, "fs.cmd.ring", kVaFsTx, "fs.reply.ring", kVaFsRx,
             "fs.cmd.doorbell", "fs.reply.doorbell", &fs_session)) {
        mochi::log_counter("metric.app.hello.root.entries", fs_root_entry_count(fs_session));
    } else {
        mochi::log_raw_marker("fs: session unavailable\n");
    }

    mochi::log_raw_marker("done\n");
    while (true) sys_yield();
}
