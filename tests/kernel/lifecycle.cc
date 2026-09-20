#include "mochi/boot.h"
#include "midix/format/bundle.h"
#include "libk/string.h"

static CapHandle supervisor;
static uint32_t cpu_count;
static midix::format::Bundle bundle;

static void print(const char* text) {
    sys_debug_puts(text, libk::strlen(text));
}
[[noreturn]] static void fail(const char* message) {
    print("TEST FAILED: ");
    print(message);
    print("\n");
    while (true) {
        sys_yield();
    }
}
static void check(bool value, const char* message) {
    if (!value) {
        fail(message);
    }
}

struct Child {
    CapHandle process, thread;
    uint64_t identity;
};
static Child spawn(const char* name) {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    for (uint32_t i = 0; i < bundle.header.object_count; ++i) {
        MxBundleObject object{};
        const char* object_name = nullptr;
        check(midix::format::object(bundle, i, object, object_name), "test object lookup");
        if (mochi::boot::streq(object_name, name)) {
            data = bundle.data + object.offset;
            size = object.size;
            break;
        }
    }
    check(data && size, "test image absent");
    static const char process_name[32] = "test.child";
    auto result = sys_spawn(data, size, MX_POLICY_PRINCIPAL_NATIVE_SANDBOXED, supervisor,
                            MX_SPAWN_SUSPENDED, process_name);
    check(result.value > 0 && result.detail > 0, "spawn");
    Child child{static_cast<CapHandle>(result.value), static_cast<CapHandle>(result.detail), 0};
    MxCapInfo info{};
    check(!sys_cap_info(child.process, &info, sizeof(info)), "process identity");
    child.identity = info.base;
    check(!sys_thread_set_affinity(child.thread, 1ull << (cpu_count - 1)), "remote affinity");
    return child;
}
static void await_exit(uint64_t identity, long expected_status) {
    const uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (mx_clock_ns() < deadline) {
        auto event = sys_supervisor_poll(supervisor);
        if (event.value > 0) {
            check(static_cast<uint64_t>(event.value) == identity && event.detail == expected_status,
                  "exit identity/status");
            return;
        }
        sys_yield();
    }
    fail("termination acknowledgement timeout");
}
static void settle(long objects, long pages) {
    const uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (mx_clock_ns() < deadline) {
        if (sys_debug_counter(200) == objects && sys_debug_counter(201) == pages) {
            return;
        }
        sys_yield();
    }
    fail("resources accumulated");
}

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    supervisor = mochi::boot::require_handle(boot, "supervisor");
    cpu_count = mochi::boot::find(boot, "cpu.count")->aux0;
    auto archive = mochi::boot::require_handle(boot, "boot.initrd");
    check(!sys_map_page(archive, 0x20000000, RIGHT_READ), "bundle mapping");
    bundle.data = reinterpret_cast<const uint8_t*>(0x20000000);
    bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
    libk::memcpy(&bundle.header, bundle.data, sizeof(bundle.header));
    print("lifecycle regression started\n");

    const long objects = sys_debug_counter(200), pages = sys_debug_counter(201);
    uint64_t previous[65]{};
    for (uint32_t cycle = 0; cycle < 100; ++cycle) {
        auto child = spawn("test.busy");
        const uint32_t pid = static_cast<uint32_t>(child.identity);
        check(pid <= 64 && previous[pid] != child.identity, "PID generation reused");
        previous[pid] = child.identity;
        check(!sys_thread_resume(child.thread), "resume");
        for (uint32_t i = 0; i < 8; ++i) {
            sys_yield();
        }
        check(!sys_kill(child.process), "remote kill");
        await_exit(child.identity, -1);
        check(sys_cap_identify(child.process) < 0 && sys_cap_identify(child.thread) < 0,
              "stale process/thread capabilities");
        settle(objects, pages);
    }
    print("100 restart cycles: stable objects and pages\n");

    Child children[8]{};
    for (auto& child : children) {
        child = spawn("test.busy");
        check(!sys_thread_resume(child.thread), "parallel resume");
    }
    for (auto& child : children) {
        check(!sys_kill(child.process), "parallel kill");
    }
    uint32_t seen = 0;
    const uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (seen != 255 && mx_clock_ns() < deadline) {
        auto event = sys_supervisor_poll(supervisor);
        if (event.value > 0) {
            bool found = false;
            for (uint32_t i = 0; i < 8; ++i) {
                if (children[i].identity == static_cast<uint64_t>(event.value)) {
                    check(!(seen & (1u << i)), "duplicate exit event");
                    seen |= 1u << i;
                    found = true;
                }
            }
            check(found, "foreign exit event");
        }
        sys_yield();
    }
    check(seen == 255, "simultaneous exit lost");
    settle(objects, pages);
    print("simultaneous exits: all acknowledged\n");

    static CapHandle handles[1024];
    uint32_t count = 0;
    for (; count < 1024; ++count) {
        auto h = sys_doorbell_create();
        if (h < 0) {
            break;
        }
        handles[count] = h;
    }
    check(count > 900 && count < 1024, "capability exhaustion");
    check(sys_arena_create(4096, 0) < 0 && sys_ring_create(6, 0) < 0, "creation at capacity");
    for (uint32_t i = 0; i < count; ++i) {
        sys_cap_revoke(handles[i]);
    }
    settle(objects, pages);
    print("capability exhaustion: resources released\n");

    auto child = spawn("test.share");
    auto boot_arena = static_cast<CapHandle>(sys_arena_create(4096, 0));
    auto result_arena = static_cast<CapHandle>(sys_arena_create(4096, 0));
    check(!sys_map_page(boot_arena, 0x30000000, RIGHT_READ | RIGHT_WRITE), "child BootInfo map");
    check(!sys_map_page(result_arena, 0x30010000, RIGHT_READ | RIGHT_WRITE), "result map");
    auto* child_boot = reinterpret_cast<MxBootInfo*>(0x30000000);
    libk::memset(child_boot, 0, sizeof(*child_boot));
    check(sys_cap_grant(child.process, boot_arena, RIGHT_READ | RIGHT_MAP) == (1ull << 32),
          "bootstrap handle");
    child_boot->magic = MX_BOOT_MAGIC;
    child_boot->version = MX_BOOT_VERSION;
    child_boot->entry_count = 2;
    auto& parent = child_boot->entries[0];
    parent.type = MX_BOOT_ENTRY_HANDLE;
    libk::memcpy(parent.name, "parent", 7);
    parent.handle = sys_cap_grant(child.process, mochi::boot::require_handle(boot, "self"),
                                  RIGHT_READ | RIGHT_SPAWN);
    auto& result = child_boot->entries[1];
    result.type = MX_BOOT_ENTRY_ARENA;
    libk::memcpy(result.name, "result", 7);
    result.handle =
        sys_cap_grant(child.process, result_arena, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    check(parent.handle && result.handle && !sys_thread_resume(child.thread),
          "shared creator start");
    await_exit(child.identity, 42);
    auto shared = *reinterpret_cast<volatile uint64_t*>(0x30010000);
    check(shared && !sys_map_page(shared, 0x30020000, RIGHT_READ),
          "shared arena after creator exit");
    check(*reinterpret_cast<volatile uint64_t*>(0x30020000) == 0x123456789abcdef0ull,
          "shared contents");
    sys_cap_revoke(shared); // the mapping must keep the backing alive
    check(*reinterpret_cast<volatile uint64_t*>(0x30020000) == 0x123456789abcdef0ull,
          "mapped backing after revoke");
    sys_unmap_page(0x30020000);
    sys_unmap_page(0x30010000);
    sys_unmap_page(0x30000000);
    sys_cap_revoke(result_arena);
    sys_cap_revoke(boot_arena);
    settle(objects, pages);
    print("shared arena: survives creator and capability revocation\n");
    print("lifecycle regression passed\n");
    while (true) {
        sys_yield();
    }
}
