#include "mochi/boot.h"
#include "midix/abi/image.h"
#include "midix/format/bundle.h"
#include "midix/format/integrity.h"
#include "libk/string.h"

static CapHandle supervisor;
static uint32_t cpu_count;
static MxBootInfo* boot_info;
static midix::format::Bundle bundle;

// Presence of this value in a spawned BootInfo turns a second copy of this
// image into the VM-owner worker instead of another supervising test.
static constexpr uint64_t kModeVmWorker = 1;

// Scratch arena the parent uses to materialise a synthetic failing image.
static constexpr uint64_t kFixtureArenaVa = 0x32000000ull;
static constexpr uint64_t kFixtureArenaSize = 0x2000ull; // two pages: the image is 8 KiB
static constexpr uint64_t kFixtureCodeVa = 0x14000000ull; // child executable segment
// Deliberately adjacent to the code segment so the partial map shares its
// first level-3 table with an already-mapped page; unwinding must prune only
// the tables that became empty.
static constexpr uint64_t kFixtureDataVa = 0x14001000ull; // child oversized RW segment
static constexpr uint64_t kFixtureDataPages = 2048;       // > sandbox page quota (1024)

// VM debug-counter ids (kernel/include/anko/vm.h: VmDebugCounter) mirrored for
// the userland image, which does not include kernel headers.
static constexpr uint64_t kCounterVmCreated = 33;
static constexpr uint64_t kCounterVmDestroyed = 34;
// IRQ delivery selftest status (anko/syscall.h: DBG_IRQ_SELFTEST).
static constexpr uint64_t kCounterIrqSelftest = 69;

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
static const uint8_t* object_bytes(const char* name, uint64_t* size) {
    for (uint32_t i = 0; i < bundle.header.object_count; ++i) {
        MxBundleObject object{};
        const char* object_name = nullptr;
        check(midix::format::object(bundle, i, object, object_name), "test object lookup");
        if (mochi::boot::streq(object_name, name)) {
            *size = object.size;
            return bundle.data + object.offset;
        }
    }
    return nullptr;
}
static Child spawn(const char* name) {
    uint64_t size = 0;
    const uint8_t* data = object_bytes(name, &size);
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

// Build a syntactically valid MXI1 image that nevertheless cannot be loaded by
// a sandboxed principal: its second segment needs far more pages than the
// 1024-page quota. The loader must fail part-way through that segment's map and
// unwind every backing page and page table it already built.
static uint64_t build_oversized_image(uint8_t* buffer) {
    constexpr uint64_t kHeaderSize = sizeof(MxImageHeader);
    constexpr uint64_t kSegmentSize = sizeof(MxImageSegment);
    constexpr uint64_t kCodeOffset = 4096;
    constexpr uint64_t kCodeSize = 4096;
    constexpr uint64_t kImageSize = kCodeOffset + kCodeSize;

    libk::memset(buffer, 0, kImageSize);

    // One executable page. It never runs: loading fails on the oversized
    // zero-fill segment that follows. `b .` keeps it valid AArch64 regardless.
    auto* code = reinterpret_cast<uint32_t*>(buffer + kCodeOffset);
    code[0] = 0x14000000u;

    MxImageHeader header{};
    header.magic = MX_IMAGE_MAGIC;
    header.version = MX_IMAGE_VERSION;
    header.header_size = kHeaderSize;
    header.kind = MX_IMAGE_KIND_EXEC;
    header.arch = MX_IMAGE_ARCH_AARCH64;
    header.abi_version = MX_IMAGE_ABI_VERSION;
    header.flags = MX_IMAGE_FLAG_STATIC;
    header.entry_va = kFixtureCodeVa;
    header.stack_size = 4096;
    header.image_size = kImageSize;
    header.segment_table_offset = kHeaderSize;
    header.segment_count = 2;
    header.checksum_algorithm = MX_CHECKSUM_CRC32C;

    MxImageSegment segments[2]{};
    segments[0].file_offset = kCodeOffset;
    segments[0].file_size = kCodeSize;
    segments[0].memory_size = kCodeSize;
    segments[0].virtual_address = kFixtureCodeVa;
    segments[0].alignment = 4096;
    segments[0].rights = MX_RIGHT_READ | MX_RIGHT_EXEC;
    segments[1].file_offset = kCodeOffset + kCodeSize;
    segments[1].file_size = 0;
    segments[1].memory_size = kFixtureDataPages * 4096;
    segments[1].virtual_address = kFixtureDataVa;
    segments[1].alignment = 4096;
    segments[1].rights = MX_RIGHT_READ | MX_RIGHT_WRITE;
    segments[1].flags = MX_IMAGE_SEG_ZERO_FILL;
    static_assert(kSegmentSize == 64);

    libk::memcpy(buffer, &header, kHeaderSize);
    libk::memcpy(buffer + kHeaderSize, segments, sizeof(segments));

    const uint32_t crc = integrity::crc32c_zero_field(
        buffer, kImageSize, __builtin_offsetof(MxImageHeader, image_crc32c), sizeof(uint32_t));
    libk::memcpy(buffer + __builtin_offsetof(MxImageHeader, image_crc32c), &crc, sizeof(crc));
    return kImageSize;
}

// (1) A spawn whose image cannot be fully mapped must fail cleanly: the caller
// observes an error, no capability is created, and every page charged while
// building the partial map returns to the allocator.
static void page_table_failure_mid_map(long objects, long pages) {
    auto arena = static_cast<CapHandle>(sys_arena_create(kFixtureArenaSize, 0));
    check(arena > 0, "fixture arena");
    check(!sys_map_page(arena, kFixtureArenaVa, RIGHT_READ | RIGHT_WRITE), "fixture arena map");
    auto* image = reinterpret_cast<uint8_t*>(kFixtureArenaVa);
    const uint64_t image_size = build_oversized_image(image);

    static const char process_name[32] = "test.oversized";
    auto result = sys_spawn(image, image_size, MX_POLICY_PRINCIPAL_NATIVE_SANDBOXED, supervisor,
                            MX_SPAWN_SUSPENDED, process_name);
    check(result.value < 0 && result.detail == 0, "oversized spawn must fail");

    sys_unmap_page(kFixtureArenaVa);
    sys_unmap_page(kFixtureArenaVa + 4096);
    sys_cap_revoke(arena);
    settle(objects, pages);
    print("page-table allocation failure mid-map: unwound\n");
}

// Populate a VM with the object kinds that carry their own lifetime: guest
// memory (backing pages + stage-2), a vCPU, and a paravirt channel whose ring
// and doorbell endpoints are handed out as separate capabilities. Returns 0 on
// success, otherwise a negative stage tag.
static long populate_vm(CapHandle vm, CapHandle* out_endpoint) {
    auto memory = sys_vm_memory_create(vm, 64 * 1024, 0);
    if (memory <= 0) return -2;
    if (sys_vm_memory_map(vm, static_cast<CapHandle>(memory), 0x40000000ull, 64 * 1024,
                          RIGHT_READ | RIGHT_WRITE) != 0) {
        return -3;
    }
    auto vcpu = sys_vcpu_create(vm, 0);
    if (vcpu <= 0) return -4;
    if (sys_vcpu_set_entry(static_cast<CapHandle>(vcpu), 0x40000000ull, 0, 0) != 0) return -5;
    auto channel = sys_vm_channel_create(vm, 6, 0);
    if (channel <= 0) return -6;
    // Guest-visible rings must reject overlap/invalid geometry and must be
    // reclaimed after both explicit VM destruction and owner death below.
    const auto ch = static_cast<CapHandle>(channel);
    const auto cpu = static_cast<CapHandle>(vcpu);
    const auto inspect = sys_cap_dup(ch, RIGHT_VM_CHANNEL_INSPECT);
    if (inspect <= 0 ||
        sys_vm_channel_map(inspect, cpu, 0x48000000, 0x20000, 48) >= 0 ||
        sys_vm_channel_notify(inspect) >= 0 ||
        sys_vm_channel_endpoint(inspect, 0, RIGHT_READ | RIGHT_MAP) > 0 ||
        sys_vm_channel_endpoint(inspect, 1, RIGHT_WRITE | RIGHT_MAP) > 0) return -11;
    sys_cap_revoke(inspect);
    if (sys_vm_channel_map(ch, cpu, 0x40000000, 0x20000, 48) >= 0 ||
        sys_vm_channel_map(ch, cpu, 0x48000001, 0x20000, 48) >= 0 ||
        sys_vm_channel_map(ch, cpu, 0x48000000, 0x1000, 48) >= 0 ||
        sys_vm_channel_map(ch, cpu, 0x48000000, 0x20000, 16) >= 0 ||
        sys_vm_channel_notify(ch) >= 0) return -9;
    if (sys_vm_channel_map(ch, cpu, 0x48000000, 0x20000, 48) != 0 ||
        sys_vm_channel_map(ch, cpu, 0x49000000, 0x20000, 49) >= 0 ||
        sys_vm_channel_notify(ch) != 0) return -10;
    auto ring = sys_vm_channel_endpoint(static_cast<CapHandle>(channel), 0,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
    if (ring <= 0) return -7;
    if (sys_vm_channel_endpoint(static_cast<CapHandle>(channel), 2,
                                RIGHT_NOTIFY | RIGHT_WAIT) <= 0) {
        return -8;
    }
    if (out_endpoint) *out_endpoint = static_cast<CapHandle>(ring);
    return 0;
}

// (2) A supervised child owns a fully populated VM — including a channel
// endpoint capability granted out to the live parent. When the owner dies the
// VM, its vCPUs, guest memory, channel, endpoints and every observed capability
// must be released exactly once.
static void vm_owner_death(long objects, long pages) {
    const long created_before = sys_debug_counter(kCounterVmCreated);
    const long destroyed_before = sys_debug_counter(kCounterVmDestroyed);

    auto child = spawn("init");

    auto boot_arena = static_cast<CapHandle>(sys_arena_create(4096, 0));
    auto result_arena = static_cast<CapHandle>(sys_arena_create(4096, 0));
    check(boot_arena > 0 && result_arena > 0, "vm child arenas");
    check(!sys_map_page(boot_arena, 0x30000000, RIGHT_READ | RIGHT_WRITE), "vm child boot map");
    check(!sys_map_page(result_arena, 0x30010000, RIGHT_READ | RIGHT_WRITE), "vm child result map");
    auto* child_boot = reinterpret_cast<MxBootInfo*>(0x30000000);
    libk::memset(child_boot, 0, sizeof(*child_boot));
    check(sys_cap_grant(child.process, boot_arena, RIGHT_READ | RIGHT_MAP) == (1ull << 32),
          "vm child bootstrap handle");
    child_boot->magic = MX_BOOT_MAGIC;
    child_boot->version = MX_BOOT_VERSION;
    child_boot->entry_count = 4;

    auto& root_entry = child_boot->entries[0];
    root_entry.type = MX_BOOT_ENTRY_HANDLE;
    libk::memcpy(root_entry.name, "vm.root", 8);
    root_entry.handle = sys_cap_grant(child.process,
                                      mochi::boot::require_handle(boot_info, "vm.root"),
                                      RIGHT_VM_CREATE);
    auto& parent_entry = child_boot->entries[1];
    parent_entry.type = MX_BOOT_ENTRY_HANDLE;
    libk::memcpy(parent_entry.name, "parent", 7);
    parent_entry.handle = sys_cap_grant(child.process,
                                        mochi::boot::require_handle(boot_info, "self"),
                                        RIGHT_READ | RIGHT_SPAWN);
    auto& result_entry = child_boot->entries[2];
    result_entry.type = MX_BOOT_ENTRY_ARENA;
    libk::memcpy(result_entry.name, "result", 7);
    result_entry.handle =
        sys_cap_grant(child.process, result_arena, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    auto& mode_entry = child_boot->entries[3];
    mode_entry.type = MX_BOOT_ENTRY_VALUE;
    libk::memcpy(mode_entry.name, "mode", 5);
    mode_entry.aux0 = kModeVmWorker;

    check(root_entry.handle && parent_entry.handle && result_entry.handle &&
              !sys_thread_resume(child.thread),
          "vm child start");
    await_exit(child.identity, 0);
    const auto granted = *reinterpret_cast<volatile uint64_t*>(0x30010000);
    check(granted, "owner published endpoint");

    // The exit event is published before the last thread's storage teardown, so
    // the VM is retired on the owner's CPU. settle() waits for every object and
    // page the owner held to have been released, which happens only after
    // destroy_owned_process() has run.
    sys_unmap_page(0x30010000);
    sys_unmap_page(0x30000000);
    sys_cap_revoke(result_arena);
    sys_cap_revoke(boot_arena);
    settle(objects, pages);
    check(sys_debug_counter(kCounterVmCreated) == created_before + 1, "vm created exactly once");
    check(sys_debug_counter(kCounterVmDestroyed) == destroyed_before + 1,
          "vm reclaimed exactly once");
    check(sys_cap_identify(static_cast<CapHandle>(granted)) < 0,
          "channel endpoint revoked on owner death");
    print("vm owner death: reclaimed exactly once\n");
}

// (2b) An explicit SYS_VM_DESTROY by a live owner must reclaim the same object
// graph, including capabilities the owner itself still holds, and leave no
// stale handle or backing page behind.
static void vm_explicit_destroy(long objects, long pages) {
    const long created_before = sys_debug_counter(kCounterVmCreated);
    const long destroyed_before = sys_debug_counter(kCounterVmDestroyed);
    const CapHandle root = mochi::boot::require_handle(boot_info, "vm.root");

    auto vm = sys_vm_create(root, 0);
    check(vm > 0, "owner vm create");
    const auto vmh = static_cast<CapHandle>(vm);
    CapHandle endpoint = 0;
    check(populate_vm(vmh, &endpoint) == 0, "owner vm populate");
    const auto foreign_vm = sys_vm_create(root, 0);
    check(foreign_vm > 0, "channel foreign vm create");
    const auto foreign_cpu = sys_vcpu_create(foreign_vm, 0);
    const auto isolated_channel = sys_vm_channel_create(vmh, 6, 0);
    check(foreign_cpu > 0 && isolated_channel > 0, "channel isolation fixtures");
    check(sys_vm_channel_map(isolated_channel, foreign_cpu, 0x49000000, 0x20000, 49) < 0,
          "channel rejects another VM's vCPU");
    check(!sys_vm_destroy(foreign_vm), "channel foreign vm destroy");
    check(!sys_vm_destroy(vmh), "owner vm destroy");
    settle(objects, pages);
    check(sys_debug_counter(kCounterVmCreated) == created_before + 2, "explicit vm counted");
    check(sys_debug_counter(kCounterVmDestroyed) == destroyed_before + 2,
          "explicit vm reclaimed");
    check(sys_cap_identify(vmh) < 0 && sys_cap_identify(endpoint) < 0,
          "destroyed vm and endpoint capabilities are stale");
    print("explicit vm destroy: reclaimed once\n");
}

// (3) Appliance channel lifecycle (Phase K): a real boot generation is
// published and a stale generation is rejected; a full ring back-pressures
// with a distinct status instead of spinning or dropping; and a clean channel
// reset accepts a fresh generation. All of this runs with no hot-path
// allocation and no vCPU ever entering the guest.
static constexpr uint64_t kChannelRingOrder = 6;
static constexpr uint64_t kChannelRingSlots = 1ull << kChannelRingOrder;
static constexpr uint64_t kChannelRingVa = 0x30040000ull;

static void channel_lifecycle(long objects, long pages) {
    const CapHandle root = mochi::boot::require_handle(boot_info, "vm.root");
    const long created_before = sys_debug_counter(kCounterVmCreated);
    const long destroyed_before = sys_debug_counter(kCounterVmDestroyed);

    auto vm = sys_vm_create(root, 0);
    check(vm > 0, "channel vm create");
    const auto vmh = static_cast<CapHandle>(vm);
    auto memory = sys_vm_memory_create(vmh, 64 * 1024, 0);
    check(memory > 0 &&
              !sys_vm_memory_map(vmh, static_cast<CapHandle>(memory), 0x40000000ull, 64 * 1024,
                                 RIGHT_READ | RIGHT_WRITE),
          "channel vm memory");
    auto vcpu = sys_vcpu_create(vmh, 0);
    check(vcpu > 0 && !sys_vcpu_set_entry(static_cast<CapHandle>(vcpu), 0x40000000ull, 0, 0),
          "channel vm vcpu");
    const auto cpuh = static_cast<CapHandle>(vcpu);

    // Generation 1: the appliance establishes its channel and maps it.
    auto c1 = sys_vm_channel_create(vmh, kChannelRingOrder, 0);
    check(c1 > 0, "channel c1 create");
    const auto c1h = static_cast<CapHandle>(c1);
    check(!sys_vm_channel_map(c1h, cpuh, 0x48000000ull, 0x20000ull, 48), "channel c1 map");
    check(!sys_vm_channel_notify(c1h), "channel c1 notify");

    // Reconnect: a second channel advances the appliance generation, so every
    // reference captured against the first channel is now stale.
    auto c2 = sys_vm_channel_create(vmh, kChannelRingOrder, 0);
    check(c2 > 0, "channel c2 create");
    const auto c2h = static_cast<CapHandle>(c2);
    check(sys_vm_channel_notify(c1h) == MX_ERR_NOT_FOUND, "stale generation rejected (notify)");
    check(sys_vm_channel_map(c1h, cpuh, 0x49000000ull, 0x20000ull, 49) == MX_ERR_NOT_FOUND,
          "stale generation rejected (map)");
    check(sys_vm_channel_endpoint(c1h, 0, RIGHT_READ | RIGHT_MAP) < 0,
          "stale generation rejects a new endpoint");

    // The fresh generation is accepted, including the guest-visible rings.
    check(!sys_vm_channel_map(c2h, cpuh, 0x49000000ull, 0x20000ull, 49), "fresh generation map");
    check(!sys_vm_channel_notify(c2h), "fresh generation notify");

    // Ring back-pressure: fill the host->guest ring through its mapped endpoint
    // and prove notify returns a distinct status the peer can act on (never a
    // silent drop or a kernel spin).
    auto ring = sys_vm_channel_endpoint(c2h, 0, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);
    check(ring > 0, "channel ring endpoint");
    check(!sys_map_page(static_cast<CapHandle>(ring), kChannelRingVa, RIGHT_READ | RIGHT_WRITE),
          "channel ring map");
    auto* ring_head = reinterpret_cast<volatile uint64_t*>(kChannelRingVa);
    auto* ring_tail = reinterpret_cast<volatile uint64_t*>(kChannelRingVa + 0x40);
    *ring_tail = 0;
    *ring_head = kChannelRingSlots; // head - tail == capacity -> full
    check(sys_vm_channel_notify(c2h) == MX_ERR_BUSY, "full ring back-pressures");
    *ring_tail = kChannelRingSlots; // consumer drains: not full any more
    check(!sys_vm_channel_notify(c2h), "drained ring accepts a publish");

    // A clean reset accepts a fresh generation again; the previous channel is
    // now the stale one.
    auto c3 = sys_vm_channel_create(vmh, kChannelRingOrder, 0);
    check(c3 > 0, "channel c3 create");
    const auto c3h = static_cast<CapHandle>(c3);
    check(sys_vm_channel_notify(c2h) == MX_ERR_NOT_FOUND, "superseded generation rejected");
    check(!sys_vm_channel_map(c3h, cpuh, 0x48800000ull, 0x20000ull, 50), "reset generation map");
    check(!sys_vm_channel_notify(c3h), "reset generation notify");

    // A ring mapping is a multi-page arena entry; every page must be unmapped
    // before the retained backing object is released (mm::arena_unmap_page
    // only unregisters when the whole entry is unmapped).
    sys_unmap_page(kChannelRingVa);
    sys_unmap_page(kChannelRingVa + 4096);
    sys_cap_revoke(static_cast<CapHandle>(ring));
    check(!sys_vm_destroy(vmh), "channel vm destroy");
    settle(objects, pages);
    check(sys_debug_counter(kCounterVmCreated) == created_before + 1, "channel vm created once");
    check(sys_debug_counter(kCounterVmDestroyed) == destroyed_before + 1,
          "channel vm reclaimed once");
    check(sys_cap_identify(c1h) < 0 && sys_cap_identify(c2h) < 0 && sys_cap_identify(c3h) < 0,
          "channel capabilities stale after teardown");

    // A full appliance restart (destroy + create) mints a fresh generation that
    // a brand-new channel accepts — the GPU service reconnect path.
    auto vm2 = sys_vm_create(root, 0);
    check(vm2 > 0, "restart vm create");
    const auto vm2h = static_cast<CapHandle>(vm2);
    auto mem2 = sys_vm_memory_create(vm2h, 64 * 1024, 0);
    check(mem2 > 0 && !sys_vm_memory_map(vm2h, static_cast<CapHandle>(mem2), 0x40000000ull,
                                         64 * 1024, RIGHT_READ | RIGHT_WRITE),
          "restart vm memory");
    auto cpu2 = sys_vcpu_create(vm2h, 0);
    check(cpu2 > 0 && !sys_vcpu_set_entry(static_cast<CapHandle>(cpu2), 0x40000000ull, 0, 0),
          "restart vm vcpu");
    auto rc1 = sys_vm_channel_create(vm2h, kChannelRingOrder, 0);
    check(rc1 > 0, "restart channel create");
    check(!sys_vm_channel_map(static_cast<CapHandle>(rc1), static_cast<CapHandle>(cpu2),
                              0x48000000ull, 0x20000ull, 48),
          "restart channel map");
    check(!sys_vm_channel_notify(static_cast<CapHandle>(rc1)), "restart channel notify");
    check(!sys_vm_destroy(vm2h), "restart vm destroy");
    settle(objects, pages);
    check(sys_debug_counter(kCounterVmCreated) == created_before + 2, "restart counted");
    check(sys_debug_counter(kCounterVmDestroyed) == destroyed_before + 2, "restart reclaimed");
    print("channel boot generation: stale rejected, full ring back-pressures, reset accepted\n");
}

// Child behaviour: build a VM with memory, a vCPU and a channel, publish one
// channel endpoint capability into the live parent, then exit so the owning
// process dies with live VM objects.
[[noreturn]] static void vm_owner_worker(MxBootInfo* boot) {
    const CapHandle root = mochi::boot::require_handle(boot, "vm.root");
    const CapHandle parent = mochi::boot::require_handle(boot, "parent");
    const CapHandle result = mochi::boot::require_handle(boot, "result");
    long status = 0;
    auto vm = sys_vm_create(root, 0);
    if (vm <= 0) {
        status = -1;
    } else {
        CapHandle endpoint = 0;
        status = populate_vm(static_cast<CapHandle>(vm), &endpoint);
        if (status == 0) {
            if (sys_map_page(result, 0x30010000, RIGHT_READ | RIGHT_WRITE) != 0) {
                status = -9;
            } else {
                auto shared = sys_cap_grant(parent, endpoint,
                                            RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
                if (shared <= 0) {
                    status = -10;
                } else {
                    *reinterpret_cast<volatile uint64_t*>(0x30010000) =
                        static_cast<uint64_t>(shared);
                }
            }
        }
    }
    sys_exit(status);
    while (true) {
        sys_yield();
    }
}

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    boot_info = boot;
    const auto* mode = mochi::boot::find(boot, "mode");
    if (mode && mode->type == MX_BOOT_ENTRY_VALUE && mode->aux0 == kModeVmWorker) {
        vm_owner_worker(boot);
    }
    supervisor = mochi::boot::require_handle(boot, "supervisor");
    cpu_count = mochi::boot::find(boot, "cpu.count")->aux0;
    auto archive = mochi::boot::require_handle(boot, "boot.initrd");
    check(!sys_map_page(archive, 0x20000000, RIGHT_READ), "bundle mapping");
    bundle.data = reinterpret_cast<const uint8_t*>(0x20000000);
    bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
    libk::memcpy(&bundle.header, bundle.data, sizeof(bundle.header));
    print("lifecycle regression started\n");

    // The kernel raised a spare SPI at boot and delivered it to a doorbell;
    // this confirms real interrupt delivery, not just the binding plumbing.
    check(sys_debug_counter(kCounterIrqSelftest) == 1, "irq delivery selftest");

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

    page_table_failure_mid_map(objects, pages);
    vm_owner_death(objects, pages);
    vm_explicit_destroy(objects, pages);
    channel_lifecycle(objects, pages);
    print("lifecycle regression passed\n");
    while (true) {
        sys_yield();
    }
}
