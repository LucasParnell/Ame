// Exercises the production session and resource owner against actual Ame
// processes and the physical allocator, without depending on a USB controller.
#include "../../userland/apps/resources.h"
#include "midix/format/bundle.h"

namespace {
midix::format::Bundle bundle;
mochi::apps::Session<mochi::apps::Resources>* session;
void check(bool value, const char* what) {
    if (value) return;
    sys_debug_puts("APP STREAM TEST FAILED: ", 24);
    sys_debug_puts(what, libk::str::length(what));
    sys_debug_puts("\n", 1);
    for (;;) mx_yield();
}
const mx_u8_t* image(const char* name, mx_u64_t& size) {
    MxBundleObject object{};
    check(midix::format::find_object(bundle, name, object), "object lookup");
    size = object.size;
    return bundle.data + object.offset;
}
mx_status_t request(mx_u16_t type, libk::const_bytes body = {}) {
    mx_u8_t payload[64]{};
    libk::write_le<mx_u64_t>(libk::as_bytes(payload, 8), session->id);
    libk::copy_exact(libk::as_bytes(payload + 8, body.size), body);
    return session->request(type, libk::as_bytes(payload, body.size + 8), 17, mx_clock_ns());
}
void upload(const mx_u8_t* data, mx_u64_t size) {
    mx_u8_t begin[8]{};
    libk::write_le<mx_u32_t>(libk::as_bytes(begin, 4), size);
    libk::write_le<mx_u32_t>(libk::as_bytes(begin + 4, 4), 1000);
    check(request(midix::apps::kBegin, libk::as_bytes(begin)) == MX_OK, "begin upload");
    for (mx_u32_t off = 0; off < size;) {
        const mx_u32_t count = size - off < 52 ? size - off : 52;
        mx_u8_t bytes[56]{};
        libk::write_le<mx_u32_t>(libk::as_bytes(bytes, 4), off);
        libk::copy_exact(libk::as_bytes(bytes + 4, count), libk::as_bytes(data + off, count));
        check(request(midix::apps::kData, libk::as_bytes(bytes, count + 4)) == MX_OK, "upload data");
        off += count;
    }
}
void settle(mx_i64_t objects, mx_i64_t pages) {
    const auto deadline = mx_clock_ns() + 2'000'000'000ull;
    while (mx_clock_ns() < deadline) {
        if (sys_debug_counter(200) == objects && sys_debug_counter(201) == pages) return;
        mx_yield();
    }
    check(false, "objects or pages leaked");
}
}
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    auto arena = mochi::make_state(0x31000000, 16384);
    session = arena.alloc<mochi::apps::Session<mochi::apps::Resources>>();
    check(session != nullptr, "test state arena");
    session->resources.supervisor = mochi::boot::require_handle(boot, "supervisor");
    session->resources.cpu_count = mochi::driver::value(boot, "cpu.count", 1);
    session->resources.cpu_mask = mochi::driver::value(boot, "cpu.mask", 1);
    check(sys_map_page(mochi::boot::require_handle(boot, "boot.initrd"), 0x24000000, RIGHT_READ) == 0,
          "bundle map");
    bundle.data = reinterpret_cast<const mx_u8_t*>(0x24000000);
    bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
    libk::copy_exact(libk::as_bytes(&bundle.header, sizeof(bundle.header)),
                    libk::as_bytes(bundle.data, sizeof(bundle.header)));
    mx_u64_t size = 0;
    const auto* test = image("app.stream-test", size);
    const auto objects = sys_debug_counter(200), pages = sys_debug_counter(201);
    check(session->start(mx_clock_ns()) == MX_OK, "start session");
    for (mx_u32_t cycle = 0; cycle < 10; ++cycle) {
        upload(test, size);
        check(request(midix::apps::kRun) == MX_OK, "launch app");
        check(session->resources.upload_count == 0, "upload arenas retained after launch");
        const auto deadline = mx_clock_ns() + 2'000'000'000ull;
        while (session->state != midix::apps::Ready && mx_clock_ns() < deadline) {
            session->tick(mx_clock_ns()); mx_yield();
        }
        check(session->state == midix::apps::Ready && session->exit_code == 0, "app exit status");
        settle(objects, pages);
    }
    // Upload rejection must reclaim memory without even creating a child.
    mx_u8_t invalid[128]{};
    upload(invalid, sizeof(invalid));
    check(request(midix::apps::kRun) == MX_ERR_BAD_IMAGE, "bad image accepted");
    settle(objects, pages);
    // Abort a real busy process and await all remote cores before reuse.
    const auto* busy = image("test.busy", size);
    upload(busy, size);
    check(request(midix::apps::kRun) == MX_OK, "launch busy app");
    session->cancel(true);
    const auto deadline = mx_clock_ns() + 2'000'000'000ull;
    while (session->state != midix::apps::Closed && mx_clock_ns() < deadline) {
        session->tick(mx_clock_ns()); mx_yield();
    }
    check(session->state == midix::apps::Closed, "cancel did not finish");
    settle(objects, pages);
    // Delegated supervisors have separate exit streams; ephemeral children
    // cannot outlive them or strand a PID after a supervisor crash.
    for (mx_u32_t cycle = 0; cycle < 4; ++cycle) {
        const char parent_name[32] = "test.stream-supervisor";
        const char child_name[32] = "test.stream-child";
        auto parent = sys_spawn(busy, size, 2, session->resources.supervisor,
                                MX_SPAWN_SUSPENDED, parent_name);
        check(parent.value > 0 && parent.detail > 0, "spawn supervisor");
        check(sys_supervisor_delegate(0, parent.value) < 0, "ambient delegation accepted");
        const auto delegated = sys_supervisor_delegate(session->resources.supervisor, parent.value);
        check(delegated > 0, "delegate supervision");
        auto child = sys_spawn(busy, size, 2, delegated,
            MX_SPAWN_SUSPENDED | MX_SPAWN_KILL_WITH_SUPERVISOR, child_name);
        check(child.value > 0 && child.detail > 0, "spawn supervised child");
        check(sys_supervisor_delegate(session->resources.supervisor, child.value) < 0,
              "delegation crossed supervisor boundary");
        check(sys_thread_resume(parent.detail) == 0 && sys_thread_resume(child.detail) == 0,
              "resume supervisor and child");
        check(sys_kill(parent.value) == 0, "kill supervisor");
        const auto wait_until = mx_clock_ns() + 2'000'000'000ull;
        bool reaped = false;
        while (mx_clock_ns() < wait_until) {
            const auto event = sys_supervisor_poll(session->resources.supervisor);
            if (event.value > 0) { reaped = true; break; }
            mx_yield();
        }
        check(reaped, "supervisor exit not acknowledged");
        check(sys_cap_identify(child.value) < 0 && sys_cap_identify(child.detail) < 0,
              "app survived supervisor death");
        sys_cap_revoke(delegated);
        settle(objects, pages);
    }
    // Newly allocated storage must not retain a prior session's scratch data.
    for (mx_u32_t cycle = 0; cycle < 4; ++cycle) {
        const auto cap = sys_arena_create(16384, 0);
        check(cap > 0 && sys_map_page(cap, 0x32000000, RIGHT_READ | RIGHT_WRITE) == 0, "scratch map");
        check(libk::is_zero(libk::as_bytes(reinterpret_cast<void*>(0x32000000), 16384)), "dirty freed pages");
        libk::fill(libk::as_bytes(reinterpret_cast<void*>(0x32000000), 16384), 0xa5);
        for (mx_u64_t off = 0; off < 16384; off += 4096) sys_unmap_page(0x32000000 + off);
        sys_cap_revoke(cap);
        settle(objects, pages);
    }
    sys_debug_puts("app streaming kernel regression passed\n", 39);
    for (;;) mx_yield();
}
