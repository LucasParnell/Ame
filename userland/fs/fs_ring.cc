// userland/fs/fs_ring.cc — io.fs boot wiring, per-client session loop, dispatch.
//
// io.fs serves one SPSC request/reply ring pair and one doorbell pair per
// client. The directory of client slots is built by init; every slot carries its
// own rings, doorbells, and generation. The service never shares state between
// the rings themselves — it only shares the inode/handle tables, and handles are
// tagged with the session generation so a token can never cross a client restart.
//
// Liveness: each request is answered within mochi::kSessionReplyTimeoutNs. If the
// peer has died (its process cap is revoked, so sys_cap_identify fails) or has
// stopped draining its reply ring, the bounded send expires, the session is
// dropped, and the other clients keep running. That is the defined return path
// for both peer death mid-request and a stalled peer.

#include "fs_internal.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "mochi/sessions.h"



static mochi::ServerSessions g_sessions;

static void handle_slot(const mochi::fs::Server& fs, const RingSlot& slot) {
    switch (slot.type) {
    case FS_OPEN:
        handle_open(fs, slot);
        break;
    case FS_CLOSE:
        handle_close(fs, slot);
        break;
    case FS_READ:
        handle_read(fs, slot);
        break;
    case FS_WRITE:
        handle_write(fs, slot);
        break;
    case FS_READDIR:
        handle_readdir(fs, slot);
        break;
    case FS_MKDIR:
        handle_mkdir(fs, slot);
        break;
    case FS_UNLINK:
        handle_unlink(fs, slot);
        break;
    case FS_RENAME:
        handle_rename(fs, slot);
        break;
    case FS_PROMOTE:
        handle_promote(fs, slot);
        break;
    case FS_SNAPSHOT:
        handle_snapshot(fs, slot);
        break;
    case FS_SNAPSHOT_MNT:
        handle_snapshot_mount(fs, slot);
        break;
    case FS_SNAPSHOT_DROP:
        handle_snapshot_drop(fs, slot);
        break;
    case FS_SYNC:
        handle_sync(fs, slot);
        break;
    case FS_PREFETCH:
        break;
    default:
        (void)fs.send_error(slot, static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL));
        break;
    }
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "io.fs");

    register_service_arenas();
    format_memory_fs();

    if (!g_sessions.map(boot)) {
        mochi::boot::invalid_boot();
    }
    fs_disk_init(boot);

    // Replay the durable /data region, if the mount exposed a safe one.
    const bool data_durable = durable_available();
    const bool data_loaded = durable_load();

    for (mx_u32_t i = 0; i < g_sessions.directory->count; ++i) {
        g_sessions.refresh(i, close_session);
    }
    // Root is read-only; /tmp is volatile; /data is durable only when a safe
    // reserved disk region is mounted (see fs_internal.h and fs_durable.cc).
    if (!data_durable) {
        mochi::log_raw_marker("io.fs: read-only root, volatile /tmp, non-durable /data\n");
    } else if (data_loaded) {
        mochi::log_raw_marker("io.fs: read-only root, volatile /tmp, durable /data replayed\n");
    } else {
        mochi::log_raw_marker("io.fs: read-only root, volatile /tmp, durable /data empty\n");
    }
    mochi::boot::ready(boot);
    while (true) {
        bool progress = false;
        for (mx_u32_t i = 0; i < g_sessions.directory->count; ++i) {
            if (!g_sessions.refresh(i, close_session)) {
                continue;
            }
            g_session = i;
            g_session_generation = g_sessions.generation_of(i);
            const mochi::fs::Server fs{g_sessions.endpoints[i],
                                       mx_clock_ns() + mochi::kSessionReplyTimeoutNs};
            RingSlot slot{};
            // Bound each client's work so a full ring cannot starve its peers.
            for (mx_u32_t n = 0; n < 8 && fs.try_recv(&slot); ++n) {
                handle_slot(fs, slot);
                progress = true;
                if (!fs.reply_ok) {
                    break;
                }
            }
            if (!fs.reply_ok) {
                // The client is gone or stalled: drop its session. Only init
                // can revive the slot, by publishing a new generation, so io.fs
                // never wedges on an unresponsive peer.
                g_sessions.drop(i, close_session);
            }
        }
        if (!progress) {
            mx_yield();
        }
    }
}
