#pragma once
// Public application lifecycle API. Explicit ownership, no heap, no boot work.
// Configure one Process per child slot; wait/cancel before reusing its storage.
#include "libk/atomic.h"
#include "mochi/app.h"
#include "mochi/fs.h"
#include "midix/format/image.h"

namespace mochi::app {
static_assert(midix::apps::kMaxImage == va::app::kMaxImage);
struct SpawnOptions {
    const char* name = "app";
    mx_u32_t principal = MX_POLICY_PRINCIPAL_NATIVE_SANDBOXED;
    bool self_image = true;
    bool capture_stdout = true;
    mx_handle_t output_wake = 0;
};
inline void wipe(void* data, mx_u64_t size) {
    auto* bytes = static_cast<volatile mx_u8_t*>(data);
    while (size--)
        *bytes++ = 0;
}
struct Process {
    mx_handle_t supervisor = 0, process = 0, thread = 0, boot_arena = 0, log_ring = 0, log_db = 0;
    mx_handle_t upload[midix::apps::kMaxImage / midix::apps::kArenaBytes]{};
    mx_u32_t upload_count = 0, cpu_count = 1, slot = 0, image_size = 0;
    mx_u64_t upload_bytes = 0, identity = 0, cpu_mask = 1, log_bytes = 0;
    mochi::Ring logs{};
    midix::format::ImagePlan plan{};
    void (*output)(const char*, mx_u32_t) = nullptr;
    bool boot_mapped = false, resumed = false, exited = false, owns_log_db = false;
    mx_status_t wiring_status = MX_ERR_INVALID;
    mx_i32_t exit_code = 0;
    mx_vaddr_t boot_address() const {
        return va::app::child_boot(slot);
    }
    mx_vaddr_t stdout_address() const {
        return va::app::child_stdout(slot);
    }
    mx_status_t configure(const MxBootInfo* boot, void (*writer)(const char*, mx_u32_t) = nullptr,
                          mx_u32_t child_slot = 0) {
        if (has_child() || upload_count)
            return MX_ERR_BUSY;
        if (!boot || child_slot >= va::app::kMaxChildren)
            return MX_ERR_INVALID;
        supervisor = boot::handle(boot, "apps.supervisor");
        if (!supervisor)
            supervisor = boot::handle(boot, "supervisor");
        if (!supervisor)
            return MX_ERR_PERM;
        const Context context{boot};
        cpu_count = context.cpu_count();
        cpu_mask = context.cpu_mask();
        output = writer;
        slot = child_slot;
        return MX_OK;
    }
    bool has_child() const {
        return process != 0;
    }
    // A narrowly scoped scheduling authority: this doorbell can lend execution
    // only to this retained child thread. It grants no global latency policy.
    mx_status_t handoff_doorbell(mx_handle_t* out) const {
        if (!out || !thread || exited)
            return MX_ERR_INVALID;
        const auto cap = sys_doorbell_create_bound(thread);
        if (cap <= 0)
            return static_cast<mx_status_t>(cap);
        *out = static_cast<mx_handle_t>(cap);
        return MX_OK;
    }
    bool allocate(mx_u32_t size) {
        if (!size || size > midix::apps::kMaxImage || upload_count || upload_bytes)
            return false;
        for (mx_u32_t offset = 0; offset < size; offset += midix::apps::kArenaBytes) {
            const mx_u64_t bytes =
                (size - offset < midix::apps::kArenaBytes ? size - offset
                                                          : midix::apps::kArenaBytes);
            const auto cap = sys_arena_create(bytes, 0);
            if (cap <= 0)
                return false;
            upload[upload_count++] = static_cast<mx_handle_t>(cap);
            if (sys_map_page(cap, va::app::kUpload + offset, RIGHT_READ | RIGHT_WRITE))
                return false;
            upload_bytes = offset + ((bytes + 4095) & ~4095ull);
        }
        image_size = size;
        return true;
    }
    bool write(mx_u32_t offset, libk::const_bytes data) {
        return offset <= image_size && data.size <= image_size - offset &&
               libk::copy_exact(
                   libk::as_bytes(reinterpret_cast<void*>(va::app::kUpload + offset), data.size),
                   data);
    }
    void discard_upload() {
        if (upload_bytes)
            wipe(reinterpret_cast<void*>(va::app::kUpload), upload_bytes);
        for (mx_u64_t off = 0; off < upload_bytes; off += 4096)
            sys_unmap_page(va::app::kUpload + off);
        for (mx_u32_t i = 0; i < upload_count; ++i) {
            sys_cap_revoke(upload[i]);
            upload[i] = 0;
        }
        upload_count = 0;
        upload_bytes = 0;
        image_size = 0;
        libk::zero(plan);
    }
    void drain_logs() {
        if (!logs.valid())
            return;
        MxRingSlot slot{};
        for (mx_u32_t i = 0; i < 32 && logs.try_pop(&slot); ++i) {
            if (mochi::message_valid(slot) && slot.type == 1 &&
                slot.length <= MX_RING_INLINE_PAYLOAD) {
                const auto* bytes = reinterpret_cast<const char*>(slot.payload);
                if (output)
                    output(bytes, slot.length);
                else
                    app::write(bytes, slot.length);
            }
        }
    }
    void kill() {
        if (process)
            (void)sys_kill(process);
    }
    bool accept_exit(const SysPair& event) {
        if (event.value <= 0 || static_cast<mx_u64_t>(event.value) != identity)
            return false;
        exited = true;
        exit_code = static_cast<mx_i32_t>(event.detail);
        return true;
    }
    bool poll_exit(mx_i32_t& code) {
        drain_logs();
        if (!exited)
            accept_exit(sys_supervisor_poll(supervisor));
        if (!exited)
            return false;
        code = exit_code;
        return true;
    }
    // On timeout the child remains owned: callers may keep waiting or cancel.
    mx_status_t wait(mx_i32_t* code, mx_u64_t deadline = 0) {
        if (!has_child() || !code)
            return MX_ERR_INVALID;
        while (!poll_exit(*code)) {
            if (deadline && mx_clock_ns() >= deadline)
                return MX_ERR_WOULD_BLOCK;
            const auto expected = mx_doorbell_read(log_db);
            if (logs.valid() && logs.depth())
                continue;
            if (log_db) {
                auto next = mx_clock_ns() + 1'000'000;
                if (deadline && deadline < next)
                    next = deadline;
                (void)mx_doorbell_wait_until(log_db, expected, next);
            } else
                mx_yield();
        }
        clear_app();
        return MX_OK;
    }
    void cancel_and_wait() {
        if (has_child()) {
            kill();
            mx_i32_t ignored = 0;
            (void)wait(&ignored);
        } else
            clear_app();
        discard_upload();
    }
    mx_status_t load(fs::Client& client, fs_handle_t file, mx_u64_t size) {
        if (has_child() || upload_count)
            return MX_ERR_BUSY;
        if (!size || size > midix::apps::kMaxImage)
            return MX_ERR_BAD_IMAGE;
        if (!allocate(static_cast<mx_u32_t>(size))) {
            discard_upload();
            return MX_ERR_NOMEM;
        }
        for (mx_u32_t offset = 0; offset < size;) {
            FsReadReply reply{};
            const mx_u32_t count =
                size - offset < FS_INLINE_READ_MAX ? size - offset : FS_INLINE_READ_MAX;
            if (!client.try_read(file, offset, count, &reply) || reply.status != MX_OK ||
                reply.arena_cap || !reply.length_read || reply.length_read > count ||
                !write(offset, libk::as_bytes(reply.inline_data, reply.length_read))) {
                discard_upload();
                return MX_ERR_BAD_IMAGE;
            }
            offset += reply.length_read;
        }
        return MX_OK;
    }
    void clear_app() {
        // The child is stopped and the ring holds at most 64 slots. Bound the
        // final drain even if an app corrupted its shared cursor metadata.
        drain_logs();
        drain_logs();
        if (boot_arena) {
            if (boot_mapped) {
                wipe(reinterpret_cast<void*>(boot_address()), 4096);
                sys_unmap_page(boot_address());
            }
            sys_cap_revoke(boot_arena);
        }
        if (log_ring) {
            if (log_bytes)
                wipe(reinterpret_cast<void*>(stdout_address()), log_bytes);
            for (mx_u64_t off = 0; off < log_bytes; off += 4096)
                sys_unmap_page(stdout_address() + off);
            sys_cap_revoke(log_ring);
        }
        if (log_db && owns_log_db)
            sys_cap_revoke(log_db);
        if (thread)
            sys_cap_revoke(thread);
        if (process)
            sys_cap_revoke(process);
        process = thread = boot_arena = log_ring = log_db = identity = log_bytes = 0;
        logs = {};
        boot_mapped = resumed = exited = owns_log_db = false;
        exit_code = 0;
        wiring_status = MX_ERR_INVALID;
    }

  private:
    // Implementation step; use spawn() for rollback on setup failure.
    mx_status_t prepare(Image image, const SpawnOptions& options) {
        if (has_child() || boot_arena || log_ring)
            return MX_ERR_BUSY;
        if (!supervisor || slot >= va::app::kMaxChildren)
            return MX_ERR_PERM;
        auto* data = image.data;
        auto size = image.size;
        if (!data || !size || size > midix::apps::kMaxImage)
            return MX_ERR_BAD_IMAGE;
        if (midix::package::has_magic(data, size)) {
            midix::package::View package{};
            if (!midix::package::parse(data, size, package)) return MX_ERR_BAD_IMAGE;
            const auto executable = package.executable();
            data = executable.data;
            size = executable.size;
        }
        if (midix::format::parse_image(data, size, plan))
            return MX_ERR_BAD_IMAGE;
        char name[32]{};
        libk::str::copy_into(name, sizeof(name), options.name);
        auto status =
            mx_spawn(data, size, options.principal, supervisor,
                     MX_SPAWN_SUSPENDED | MX_SPAWN_KILL_WITH_SUPERVISOR, name, &process, &thread);
        if (status != MX_OK)
            return status;
        MxCapInfo info{};
        if (mx_cap_info(process, &info) != MX_OK)
            return MX_ERR_DEAD;
        identity = info.base;
        const auto b = sys_arena_create(4096, 0);
        boot_arena = b > 0 ? b : 0;
        if (!boot_arena)
            return MX_ERR_NOMEM;
        if (options.capture_stdout) {
            const auto l = sys_ring_create(6, 0);
            const auto d = options.output_wake ? static_cast<mx_i64_t>(options.output_wake)
                                               : sys_doorbell_create();
            log_ring = l > 0 ? l : 0;
            log_db = d > 0 ? d : 0;
            owns_log_db = !options.output_wake;
            if (!log_ring || !log_db)
                return MX_ERR_NOMEM;
        }
        if (mx_map_page(boot_arena, boot_address(), RIGHT_READ | RIGHT_WRITE) != MX_OK) {
            sys_cap_revoke(boot_arena);
            boot_arena = 0;
            return MX_ERR_NOMEM;
        }
        boot_mapped = true;
        if (log_ring &&
            mx_map_page(log_ring, stdout_address(), RIGHT_READ | RIGHT_WRITE) != MX_OK) {
            sys_cap_revoke(log_ring);
            log_ring = 0;
            return MX_ERR_NOMEM;
        }
        if (log_ring) {
            log_bytes = (mx_ring_page_bytes(6) + 4095) & ~4095ull;
            logs = {reinterpret_cast<MxRingPage*>(stdout_address()), 6, log_db};
        }
        const auto slot0 = sys_cap_grant(process, boot_arena, RIGHT_READ | RIGHT_MAP);
        if (slot0 != static_cast<mx_i64_t>(mochi::boot::kBootInfoCap))
            return MX_ERR_BAD_PROTOCOL;
        auto* boot = reinterpret_cast<MxBootInfo*>(boot_address());
        libk::zero(*boot);
        boot->version = MX_BOOT_VERSION;
        auto add = [&](MxBootEntryType type, const char* key, mx_handle_t h, mx_u64_t value = 0) {
            return mochi::app::add(boot, type, key, h, value);
        };
        if (log_ring) {
            const auto child_log =
                sys_ring_share(log_ring, process, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            const auto child_db = sys_doorbell_share(log_db, process, RIGHT_NOTIFY);
            if (child_log <= 0 || child_db <= 0)
                return MX_ERR_NOMEM;
            add(MX_BOOT_ENTRY_RING, "log.ring", child_log, 6);
            add(MX_BOOT_ENTRY_DOORBELL, "log.doorbell", child_db);
            add(MX_BOOT_ENTRY_DOORBELL, "service.ready", child_db);
        }
        add(MX_BOOT_ENTRY_VALUE, "cpu.count", 0, cpu_count);
        add(MX_BOOT_ENTRY_VALUE, "cpu.mask", 0, cpu_mask);
        const auto delegated = sys_supervisor_delegate(supervisor, process);
        if (delegated <= 0)
            return MX_ERR_PERM;
        const auto child_supervisor = sys_cap_grant(process, delegated, RIGHT_READ | RIGHT_SPAWN);
        sys_cap_revoke(delegated);
        if (child_supervisor <= 0)
            return MX_ERR_PERM;
        add(MX_BOOT_ENTRY_HANDLE, "supervisor", child_supervisor);
        const auto self_thread = sys_cap_grant(process, thread, RIGHT_READ | RIGHT_THREAD_CONTROL);
        if (self_thread <= 0)
            return MX_ERR_PERM;
        add(MX_BOOT_ENTRY_HANDLE, "thread.self", self_thread);
        add(MX_BOOT_ENTRY_VALUE, "self.identity", 0, identity);
        if (options.self_image && !publish_image(boot, process, image.data, image.size))
            return MX_ERR_NOMEM;
        wiring_status = MX_OK;
        return MX_OK;
    }

  public:
    // Suspended builder. Setup failure kills/reaps the partial child and drops
    // all mappings. A failed extra grant poisons resume() until cancellation.
    mx_status_t spawn(Image image, const SpawnOptions& options = {}) {
        if (has_child() || boot_arena || log_ring)
            return MX_ERR_BUSY;
        const auto status = prepare(image, options);
        if (status != MX_OK)
            cancel_and_wait();
        return status;
    }
    mx_status_t value(const char* name, mx_u64_t number) {
        if (!boot_mapped || resumed || wiring_status != MX_OK)
            return MX_ERR_INVALID;
        return wiring_status = add(reinterpret_cast<MxBootInfo*>(boot_address()),
                                   MX_BOOT_ENTRY_VALUE, name, 0, number)
                                   ? MX_OK
                                   : MX_ERR_OVERFLOW;
    }
    mx_status_t share_ring(const char* name, mx_handle_t source, mx_u32_t order) {
        if (!boot_mapped || resumed || wiring_status != MX_OK)
            return MX_ERR_INVALID;
        const auto cap = sys_ring_share(source, process, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        if (cap <= 0)
            return wiring_status = cap;
        return wiring_status = add(reinterpret_cast<MxBootInfo*>(boot_address()),
                                   MX_BOOT_ENTRY_RING, name, cap, order)
                                   ? MX_OK
                                   : MX_ERR_OVERFLOW;
    }
    mx_status_t share_arena(const char* name, mx_handle_t source,
                            mx_rights_t rights = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP) {
        if (!boot_mapped || resumed || wiring_status != MX_OK)
            return MX_ERR_INVALID;
        if ((rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_MAP)) || !(rights & RIGHT_MAP))
            return wiring_status = MX_ERR_INVALID;
        MxCapInfo info{};
        auto rc = mx_cap_info(source, &info);
        if (rc != MX_OK)
            return wiring_status = rc;
        if (info.type != MX_CAP_ARENA)
            return wiring_status = MX_ERR_BAD_TYPE;
        const auto cap = sys_cap_grant(process, source, rights);
        if (cap <= 0)
            return wiring_status = static_cast<mx_status_t>(cap);
        return wiring_status = add(reinterpret_cast<MxBootInfo*>(boot_address()),
                                   MX_BOOT_ENTRY_ARENA, name, cap, info.size)
                                   ? MX_OK
                                   : MX_ERR_OVERFLOW;
    }
    mx_status_t share_doorbell(const char* name, mx_handle_t source, mx_rights_t rights) {
        if (!boot_mapped || resumed || wiring_status != MX_OK)
            return MX_ERR_INVALID;
        const auto cap = sys_doorbell_share(source, process, rights);
        if (cap <= 0)
            return wiring_status = cap;
        return wiring_status = add(reinterpret_cast<MxBootInfo*>(boot_address()),
                                   MX_BOOT_ENTRY_DOORBELL, name, cap)
                                   ? MX_OK
                                   : MX_ERR_OVERFLOW;
    }
    mx_status_t resume(mx_u64_t affinity = 0) {
        if (!boot_mapped || resumed || wiring_status != MX_OK)
            return MX_ERR_INVALID;
        if (affinity) {
            const auto status = sys_thread_set_affinity(thread, affinity);
            if (status != MX_OK)
                return status;
        }
        auto* boot = reinterpret_cast<MxBootInfo*>(boot_address());
        libk::atomic::store_release(&boot->magic, MX_BOOT_MAGIC);
        const auto status = mx_thread_resume(thread);
        if (status == MX_OK)
            resumed = true;
        return status;
    }
    mx_status_t launch(mx_u32_t size, const char* name = "app.streamed") {
        if (!size || size > image_size)
            return MX_ERR_BAD_IMAGE;
        SpawnOptions options{};
        options.name = name;
        const auto status =
            spawn({reinterpret_cast<const mx_u8_t*>(va::app::kUpload), size}, options);
        return status == MX_OK ? resume() : status;
    }
};

// A group owns its supervisor's exit stream. Match every event to a child;
// one child's wait must never consume another child's exit silently.
inline mx_status_t wait_all(Process* children, mx_u32_t count, mx_handle_t wake,
                            mx_i32_t* exit_code) {
    if (!children || !count || !wake || !exit_code)
        return MX_ERR_INVALID;
    const auto supervisor = children[0].supervisor;
    for (mx_u32_t i = 0; i < count; ++i)
        if (!children[i].has_child() || children[i].supervisor != supervisor)
            return MX_ERR_INVALID;
    const auto bound = sys_supervisor_bind_doorbell(supervisor, wake);
    if (bound != MX_OK)
        return bound;
    mx_i32_t status = 0;
    for (;;) {
        const auto expected = mx_doorbell_read(wake);
        for (mx_u32_t i = 0; i < count; ++i) {
            if (children[i].logs.valid())
                children[i].logs.publish_sleep(expected);
            children[i].drain_logs();
        }
        for (;;) {
            const auto event = sys_supervisor_poll(supervisor);
            if (event.value <= 0)
                break;
            bool matched = false;
            for (mx_u32_t i = 0; i < count; ++i)
                matched |= children[i].accept_exit(event);
            if (!matched) {
                sys_supervisor_bind_doorbell(supervisor, 0);
                return MX_ERR_BAD_PROTOCOL;
            }
            if (event.detail && !status) {
                status = static_cast<mx_i32_t>(event.detail);
                for (mx_u32_t i = 0; i < count; ++i)
                    if (!children[i].exited)
                        children[i].kill();
            }
        }
        bool complete = true, pending = false;
        for (mx_u32_t i = 0; i < count; ++i) {
            complete &= children[i].exited;
            pending |= children[i].logs.valid() && children[i].logs.depth();
        }
        if (complete)
            break;
        if (!pending)
            (void)mx_doorbell_wait_until(wake, expected, mx_clock_ns() + 1'000'000);
    }
    sys_supervisor_bind_doorbell(supervisor, 0);
    for (mx_u32_t i = 0; i < count; ++i)
        children[i].clear_app();
    *exit_code = status;
    return MX_OK;
}
} // namespace mochi::app
