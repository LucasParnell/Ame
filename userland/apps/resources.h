#pragma once
#include "session.h"
#include "mochi/driver.h"
#include "mochi/boot_log.h"
#include "midix/debug/usb_input.h"
#include "midix/format/image.h"
#include "midix/abi/protocols/usb_function.h"

namespace mochi::apps {
constexpr mx_u64_t kUploadVa = 0x20000000, kChildBootVa = 0x19000000,
                   kChildLogVa = 0x19010000;
void wipe(void* data, mx_u64_t size) {
    auto* bytes = static_cast<volatile mx_u8_t*>(data);
    while (size--) *bytes++ = 0;
}
struct Resources {
    mx_handle_t supervisor, process, thread, boot_arena, log_ring, log_db;
    mx_handle_t upload[midix::apps::kMaxImage / midix::apps::kArenaBytes];
    mx_u32_t upload_count, cpu_count;
    mx_u64_t upload_bytes, identity, cpu_mask, log_bytes;
    mochi::Ring logs;
    midix::format::ImagePlan plan;
    bool has_child() const { return process != 0; }
    bool allocate(mx_u32_t size) {
        for (mx_u32_t offset = 0; offset < size; offset += midix::apps::kArenaBytes) {
            const mx_u64_t bytes = (size - offset < midix::apps::kArenaBytes ? size - offset : midix::apps::kArenaBytes);
            const auto cap = sys_arena_create(bytes, 0);
            if (cap <= 0) return false;
            upload[upload_count++] = static_cast<mx_handle_t>(cap);
            if (sys_map_page(cap, kUploadVa + offset, RIGHT_READ | RIGHT_WRITE)) return false;
            upload_bytes = offset + ((bytes + 4095) & ~4095ull);
        }
        return true;
    }
    void write(mx_u32_t offset, libk::const_bytes data) {
        libk::copy_exact(libk::as_bytes(reinterpret_cast<void*>(kUploadVa + offset), data.size), data);
    }
    void discard_upload() {
        if (upload_bytes) wipe(reinterpret_cast<void*>(kUploadVa), upload_bytes);
        for (mx_u64_t off = 0; off < upload_bytes; off += 4096) sys_unmap_page(kUploadVa + off);
        for (mx_u32_t i = 0; i < upload_count; ++i) {
            sys_cap_revoke(upload[i]); upload[i] = 0;
        }
        upload_count = 0; upload_bytes = 0;
        libk::zero(plan);
    }
    void drain_logs() {
        if (!logs.valid()) return;
        MxRingSlot slot{};
        for (mx_u32_t i = 0; i < 32 && logs.try_pop(&slot); ++i) {
            if (mochi::message_valid(slot) && slot.type == 1 && slot.length <= MX_RING_INLINE_PAYLOAD) {
                (void)mochi::log_try_push_bytes(mochi::default_log_sink(),
                    reinterpret_cast<const char*>(slot.payload), slot.length);
            }
        }
    }
    void kill() { if (process) (void)sys_kill(process); }
    bool poll_exit(mx_i32_t& code) {
        drain_logs();
        const auto exit = sys_supervisor_poll(supervisor);
        if (exit.value <= 0 || static_cast<mx_u64_t>(exit.value) != identity) return false;
        code = static_cast<mx_i32_t>(exit.detail);
        return true;
    }
    void clear_app() {
        drain_logs();
        if (boot_arena) {
            wipe(reinterpret_cast<void*>(kChildBootVa), 4096);
            sys_unmap_page(kChildBootVa);
            sys_cap_revoke(boot_arena);
        }
        if (log_ring) {
            if (log_bytes) wipe(reinterpret_cast<void*>(kChildLogVa), log_bytes);
            for (mx_u64_t off = 0; off < log_bytes; off += 4096) sys_unmap_page(kChildLogVa + off);
            sys_cap_revoke(log_ring);
        }
        if (log_db) sys_cap_revoke(log_db);
        if (thread) sys_cap_revoke(thread);
        if (process) sys_cap_revoke(process);
        process = thread = boot_arena = log_ring = log_db = identity = log_bytes = 0;
        logs = {};
    }
    mx_status_t launch(mx_u32_t size) {
        const auto* data = reinterpret_cast<const mx_u8_t*>(kUploadVa);
        if (midix::format::parse_image(data, size, plan)) return MX_ERR_BAD_IMAGE;
        const char name[32] = "app.streamed";
        auto status = mx_spawn(data, size, 2, supervisor,
            MX_SPAWN_SUSPENDED | MX_SPAWN_KILL_WITH_SUPERVISOR, name, &process, &thread);
        if (status != MX_OK) return status;
        MxCapInfo info{};
        if (mx_cap_info(process, &info) != MX_OK) return MX_ERR_DEAD;
        identity = info.base;
        const auto b = sys_arena_create(4096, 0), l = sys_ring_create(6, 0), d = sys_doorbell_create();
        boot_arena = b > 0 ? b : 0; log_ring = l > 0 ? l : 0; log_db = d > 0 ? d : 0;
        if (!boot_arena || !log_ring || !log_db) return MX_ERR_NOMEM;
        if (mx_map_page(boot_arena, kChildBootVa, RIGHT_READ | RIGHT_WRITE) != MX_OK) {
            sys_cap_revoke(boot_arena); boot_arena = 0; return MX_ERR_NOMEM;
        }
        if (mx_map_page(log_ring, kChildLogVa, RIGHT_READ | RIGHT_WRITE) != MX_OK) {
            sys_cap_revoke(log_ring); log_ring = 0; return MX_ERR_NOMEM;
        }
        log_bytes = (mx_ring_page_bytes(6) + 4095) & ~4095ull;
        logs = {reinterpret_cast<MxRingPage*>(kChildLogVa), 6, log_db};
        const auto slot0 = sys_cap_grant(process, boot_arena, RIGHT_READ | RIGHT_MAP);
        if (slot0 != static_cast<mx_i64_t>(mochi::boot::kBootInfoCap)) return MX_ERR_BAD_PROTOCOL;
        auto* boot = reinterpret_cast<MxBootInfo*>(kChildBootVa);
        libk::zero(*boot);
        boot->version = MX_BOOT_VERSION;
        auto add = [&](MxBootEntryType type, const char* key, mx_handle_t h, mx_u64_t value = 0) {
            auto& entry = boot->entries[boot->entry_count++];
            entry.type = type; entry.handle = h; entry.aux0 = value;
            libk::str::copy_into(entry.name, sizeof(entry.name), key);
        };
        const auto child_log = sys_ring_share(log_ring, process, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        const auto child_db = sys_doorbell_share(log_db, process, RIGHT_NOTIFY);
        if (child_log <= 0 || child_db <= 0) return MX_ERR_NOMEM;
        add(MX_BOOT_ENTRY_RING, "log.ring", child_log, 6);
        add(MX_BOOT_ENTRY_DOORBELL, "log.doorbell", child_db);
        add(MX_BOOT_ENTRY_DOORBELL, "service.ready", child_db);
        add(MX_BOOT_ENTRY_VALUE, "cpu.count", 0, cpu_count);
        add(MX_BOOT_ENTRY_VALUE, "cpu.mask", 0, cpu_mask);
        __atomic_store_n(&boot->magic, MX_BOOT_MAGIC, __ATOMIC_RELEASE);
        return mx_thread_resume(thread);
    }
};
} // namespace mochi::apps
