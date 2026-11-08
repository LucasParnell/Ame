#include "mochi/boot.h"
#include "mochi/managed_arena.h"
#include "mochi/memory.h"
#include "mochi/isolated_heap.h"
#include "midix/abi/image.h"
#include "midix/abi/vm.h"
#include "midix/format/bundle.h"
#include "midix/format/integrity.h"
#include "libk/bytes.h"
#include "libk/str.h"

static CapHandle supervisor;
static uint32_t cpu_count;
static MxBootInfo* boot_info;
static midix::format::Bundle bundle;

// Presence of this value in a spawned BootInfo turns a second copy of this
// image into the VM-owner worker instead of another supervising test.
static constexpr uint64_t kModeVmWorker = 1;
static constexpr uint64_t kModeSecurityWorker = 2;

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
    sys_debug_puts(text, libk::str::length(text));
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
static Child spawn(const char* name, uint64_t principal = MX_POLICY_PRINCIPAL_NATIVE_SANDBOXED) {
    uint64_t size = 0;
    const uint8_t* data = object_bytes(name, &size);
    check(data && size, "test image absent");
    static const char process_name[32] = "test.child";
    auto result = sys_spawn(data, size, principal, supervisor,
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

// Bounded, deterministic hostile syscalls; no fuzzing or stress loops.
static void security_worker(MxBootInfo* boot) {
    const auto delegated = mochi::boot::require_handle(boot, "supervisor");
    const auto archive = mochi::boot::require_handle(boot, "boot.initrd");
    check(!sys_map_page(archive, 0x20000000, RIGHT_READ), "hostile child archive");
    bundle.data = reinterpret_cast<const uint8_t*>(0x20000000);
    bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
    (void)libk::copy_n(libk::as_bytes(&bundle.header, sizeof(bundle.header)),
                     libk::as_bytes(bundle.data, sizeof(bundle.header)), sizeof(bundle.header));
    uint64_t size = 0;
    const auto* image = object_bytes("test.busy", &size);
    check(image && size, "hostile child valid image");
    const bool trusted = mochi::boot::find(boot, "mode")->aux1 != 0;
    auto result = sys_spawn(image, size, MX_POLICY_PRINCIPAL_NATIVE_TRUSTED,
                            delegated, MX_SPAWN_SUSPENDED, nullptr);
    check(result.value == MX_ERR_PERM, trusted ? "trusted app needs spawn authority" :
                                                "sandbox cannot spawn trusted principal");
    check(sys_cap_dup(archive, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP) == MX_ERR_PERM,
          "hostile child cannot amplify capability rights");
    check(sys_map_page(archive, 0x28000000, RIGHT_READ | RIGHT_WRITE) == MX_ERR_PERM,
          "hostile child cannot write read-only archive");
    check(sys_cap_grant(archive, archive, RIGHT_READ) == MX_ERR_PERM,
          "hostile child cannot substitute arena for process authority");
    check(sys_unmap_page(0xffff800040080000ull) == MX_ERR_INVALID,
          "hostile child cannot unmap kernel image");
    sys_exit(0);
    fail("security worker returned");
}

static void settle(long objects, long pages);
static void security_authority() {
    const long objects = sys_debug_counter(200), pages = sys_debug_counter(201);
    for (unsigned pass = 0; pass < 2; ++pass) {
        const bool trusted = pass != 0;
        auto child = spawn("init", trusted ? MX_POLICY_PRINCIPAL_NATIVE_TRUSTED :
                                             MX_POLICY_PRINCIPAL_NATIVE_SANDBOXED);
        auto arena = static_cast<CapHandle>(sys_arena_create(4096, 0));
        check(arena && !sys_map_page(arena, 0x30000000, RIGHT_READ | RIGHT_WRITE),
              "security boot arena");
        auto* boot = reinterpret_cast<MxBootInfo*>(0x30000000);
        libk::fill(libk::as_bytes(boot, sizeof(*boot)), 0);
        check(sys_cap_grant(child.process, arena, RIGHT_READ | RIGHT_MAP) == (1ull << 32),
              "security bootstrap handle");
        boot->magic = MX_BOOT_MAGIC;
        boot->version = MX_BOOT_VERSION;
        boot->entry_count = 3;
        boot->entries[0].type = MX_BOOT_ENTRY_VALUE;
        (void)libk::copy_n(libk::as_bytes(boot->entries[0].name, 5), libk::as_bytes("mode", 5), 5);
        boot->entries[0].aux0 = kModeSecurityWorker;
        boot->entries[0].aux1 = trusted;
        const auto delegated = sys_supervisor_delegate(supervisor, child.process);
        check(delegated > 0, "delegate child supervisor");
        boot->entries[1].type = MX_BOOT_ENTRY_HANDLE;
        (void)libk::copy_n(libk::as_bytes(boot->entries[1].name, 11), libk::as_bytes("supervisor", 11), 11);
        boot->entries[1].handle = sys_cap_grant(child.process, delegated,
                                               trusted ? RIGHT_READ : RIGHT_SPAWN);
        auto& archive = boot->entries[2];
        archive.type = MX_BOOT_ENTRY_HANDLE;
        (void)libk::copy_n(libk::as_bytes(archive.name, 12), libk::as_bytes("boot.initrd", 12), 12);
        archive.handle = sys_cap_grant(child.process, mochi::boot::require_handle(boot_info, "boot.initrd"),
                                       RIGHT_READ | RIGHT_MAP | RIGHT_DUP);
        archive.aux0 = mochi::boot::find(boot_info, "boot.initrd")->aux0;
        check(static_cast<int64_t>(archive.handle) > 0, "security archive grant");
        check(boot->entries[1].handle && !sys_thread_resume(child.thread), "security child start");
        await_exit(child.identity, 0);
        sys_cap_revoke(delegated);
        sys_unmap_page(0x30000000);
        sys_cap_revoke(arena);
    }
    settle(objects, pages);
    print("security authority regression passed\n");
}
static void print_hex(uint64_t number) {
    char text[19] = "0x0000000000000000";
    for (unsigned i = 0; i < 16; ++i) text[17 - i] = "0123456789abcdef"[(number >> (i * 4)) & 15];
    print(text);
}
static void settle(long objects, long pages) {
    const uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (mx_clock_ns() < deadline) {
        if (sys_debug_counter(200) == objects && sys_debug_counter(201) == pages) {
            return;
        }
        sys_yield();
    }
    print("resources expected/actual objects: "); print_hex(objects); print("/"); print_hex(sys_debug_counter(200));
    print(" pages: "); print_hex(pages); print("/"); print_hex(sys_debug_counter(201)); print("\n");
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

    libk::fill(libk::as_bytes(buffer, kImageSize), 0);

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

    (void)libk::copy_n(libk::as_bytes(buffer, kHeaderSize), libk::as_bytes(&header, kHeaderSize), kHeaderSize);
    (void)libk::copy_n(libk::as_bytes(buffer + kHeaderSize, sizeof(segments)), libk::as_bytes(segments, sizeof(segments)), sizeof(segments));

    const uint32_t crc = integrity::crc32c_zero_field(
        buffer, kImageSize, __builtin_offsetof(MxImageHeader, image_crc32c), sizeof(uint32_t));
    (void)libk::copy_n(libk::as_bytes(buffer + __builtin_offsetof(MxImageHeader, image_crc32c), sizeof(crc)), libk::as_bytes(&crc, sizeof(crc)), sizeof(crc));
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
    libk::fill(libk::as_bytes(child_boot, sizeof(*child_boot)), 0);
    check(sys_cap_grant(child.process, boot_arena, RIGHT_READ | RIGHT_MAP) == (1ull << 32),
          "vm child bootstrap handle");
    child_boot->magic = MX_BOOT_MAGIC;
    child_boot->version = MX_BOOT_VERSION;
    child_boot->entry_count = 4;

    auto& root_entry = child_boot->entries[0];
    root_entry.type = MX_BOOT_ENTRY_HANDLE;
    (void)libk::copy_n(libk::as_bytes(root_entry.name, 8), libk::as_bytes("vm.root", 8), 8);
    root_entry.handle = sys_cap_grant(child.process,
                                      mochi::boot::require_handle(boot_info, "vm.root"),
                                      RIGHT_VM_CREATE);
    auto& parent_entry = child_boot->entries[1];
    parent_entry.type = MX_BOOT_ENTRY_HANDLE;
    (void)libk::copy_n(libk::as_bytes(parent_entry.name, 7), libk::as_bytes("parent", 7), 7);
    parent_entry.handle = sys_cap_grant(child.process,
                                        mochi::boot::require_handle(boot_info, "self"),
                                        RIGHT_READ | RIGHT_SPAWN);
    auto& result_entry = child_boot->entries[2];
    result_entry.type = MX_BOOT_ENTRY_ARENA;
    (void)libk::copy_n(libk::as_bytes(result_entry.name, 7), libk::as_bytes("result", 7), 7);
    result_entry.handle =
        sys_cap_grant(child.process, result_arena, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    auto& mode_entry = child_boot->entries[3];
    mode_entry.type = MX_BOOT_ENTRY_VALUE;
    (void)libk::copy_n(libk::as_bytes(mode_entry.name, 5), libk::as_bytes("mode", 5), 5);
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

template <class F> static void arena_call(F call, const char* message) {
    const auto deadline = mx_clock_ns() + 3000000000ull;
    do {
        const auto result = call();
        if (!result) return;
        check(result == MX_ERR_BUSY, message);
        sys_yield();
    } while (mx_clock_ns() < deadline);
    print("arena retry timeout: "); print(message); print("\n");
    fail(message);
}
static uint64_t arena_writer_stop, arena_writer_done;
static void arena_writer(uint64_t address) {
    auto* counter = reinterpret_cast<volatile uint64_t*>(address);
    uint64_t value = *counter;
    while (!__atomic_load_n(&arena_writer_stop, __ATOMIC_ACQUIRE)) *counter = ++value;
    __atomic_store_n(&arena_writer_done, 1ull, __ATOMIC_RELEASE);
    sys_thread_exit(0);
    for (;;) sys_yield();
}
struct alignas(64) SecureNode { uint64_t value; explicit SecureNode(uint64_t v) : value(v) {} };
static mochi::memory::IsolatedHeap<SecureNode> nodes;
static void normal_memory() {
    const uint64_t invalid_addresses[]{0ull, 0x00480000ull, 0x0ffff000ull, 0x40000000ull,
                                   0x40080000ull, 0xffff800040080000ull, 0xfffffffffffff000ull};
    for (const auto address : invalid_addresses)
        check(sys_unmap_page(address) == MX_ERR_INVALID, "unmap rejects privileged VA");
    check(sys_unmap_page(0x10000001) == MX_ERR_INVALID, "unmap rejects unaligned VA");
    MxArenaCreate contract{}; CapHandle primary = 0;
    check(!mx_process_arena(&contract, &primary) && contract.type_id && contract.size == 65536,
          "manifest primary arena");
    MxCapInfo info{};
    check(!sys_cap_info(primary, &info, sizeof(info)) && info.base == contract.cage_base, "heap logical address isolation");
    auto* data = static_cast<uint8_t*>(calloc(100, 16)); check(data, "calloc");
    for (unsigned i = 0; i < 1600; ++i) check(data[i] == 0, "calloc zero");
    for (unsigned i = 0; i < 1600; ++i) data[i] = i;
    auto* larger = static_cast<uint8_t*>(realloc(data, 200000)); check(larger == data, "tail realloc without copy");
    for (unsigned i = 0; i < 1600; ++i) check(larger[i] == uint8_t(i), "realloc contents");
    free(larger);
    auto* aligned = aligned_alloc(4096, 8192);
    check(aligned && !(reinterpret_cast<uint64_t>(aligned) & 4095), "aligned allocation"); free(aligned);
    auto* object = new SecureNode(47); check(object->value == 47, "normal new"); delete object;
    const auto before = mochi::memory::statistics();
    for (unsigned i = 0; i < 10000; ++i) { auto* p = malloc(64); check(p, "malloc local"); free(p); }
    const auto after = mochi::memory::statistics();
    check(after.growths == before.growths && after.live == before.live &&
          after.allocations >= before.allocations + 10000, "heap fast path no growth syscalls");
    MxArenaCreate typed{4096, 4096, 0x39001000, 65536, 0, 0, 0};
    check(!nodes.initialize(typed), "typed isoheap");
    auto* node = nodes.create(81); check(node && node->value == 81, "isoheap constructor");
    check(!nodes.destroy(reinterpret_cast<SecureNode*>(contract.cage_base)), "isoheap foreign pointer");
    check(nodes.destroy(node) && !nodes.destroy(node), "isoheap destruction and stale pointer");
    typed.type_id = mochi::memory::IsolatedHeap<SecureNode>::type_id() ^ 1;
    CapHandle foreign = 0;
    check(mx_arena_managed_create(typed, &foreign) == MX_ERR_POLICY, "kernel type cage enforcement");
    print("ASLR fixture entry/stack/heap: ");
    print_hex(reinterpret_cast<uint64_t>(&normal_memory)); print(" ");
    print_hex(reinterpret_cast<uint64_t>(&primary)); print(" "); print_hex(contract.cage_base); print("\n");
    sys_cap_revoke(primary);
    print("manifest heap: malloc/realloc/new, local fast path and typed isolation passed\n");
}
static void el2_roundtrip() {
    const long objects = sys_debug_counter(200), pages = sys_debug_counter(201);
    const auto root = mochi::boot::require_handle(boot_info, "vm.root");
    const auto vm = sys_vm_create(root, 0); check(vm > 0, "EL2 roundtrip VM");
    const auto memory = sys_vm_memory_create(vm, 4096, 0); check(memory > 0, "EL2 roundtrip RAM");
    const uint32_t code[]{0x52800100, 0x72b08000, 0xd4000002, 0x14000000}; // PSCI SYSTEM_OFF via HVC
    check(!sys_vm_memory_write(memory, 0, code, sizeof(code)) &&
          !sys_vm_memory_map(vm, memory, 0x40000000, 4096, RIGHT_READ | RIGHT_EXEC), "EL2 roundtrip code");
    const auto cpu = sys_vcpu_create(vm, 0); check(cpu > 0, "EL2 roundtrip vCPU");
    check(!sys_vcpu_set_entry(cpu, 0x40000000, 0, 0), "EL2 roundtrip entry");
    MxVcpuRunBudget budget{10000000, 0, 8, 0}; MxVcpuRunResult result{};
    auto status = sys_vcpu_run(cpu, &budget, &result);
    if (status != MX_ERR_NOT_IMPLEMENTED) {
        check(!status, "EL2 roundtrip run");
        const auto deadline = mx_clock_ns() + 1000000000;
        while (result.exit_reason != 5 && mx_clock_ns() < deadline) {
            sys_yield(); check(!sys_vcpu_run(cpu, &budget, &result), "EL2 roundtrip resume");
        }
        check(result.exit_reason == 5, "EL2 roundtrip shutdown");
        print("EL2 guest HVC: restored randomized TTBR1 host\n");
    }
    check(!sys_vm_destroy(vm), "EL2 roundtrip destroy");
    sys_cap_revoke(vm);
    settle(objects, pages);
}
static void managed_arenas() {
    constexpr uint64_t base = 0x33000000, alias = 0x33100000, clone_va = 0x33200000;
    MxArenaCreate request{8192, 16384, base, 0x1000000, 0x414d4d, 0, 0};
    CapHandle arena = 0, clone = 0;
    check(!mx_arena_managed_create(request, &arena), "AMM create");
    arena_call([&] { return sys_map_page(arena, base, RIGHT_READ | RIGHT_WRITE); }, "AMM map");
    arena_call([&] { return sys_map_page(arena, alias, RIGHT_READ | RIGHT_WRITE); }, "AMM alias");
    auto* data = reinterpret_cast<volatile uint64_t*>(base);
    auto* other = reinterpret_cast<volatile uint64_t*>(alias);
    data[0] = 11; data[512] = 22;
    arena_call([&] { return mx_arena_resize(arena, 16384); }, "AMM grow");
    check(other[0] == 11 && other[1024] == 0, "AMM grow retains backing");
    arena_call([&] { return mx_arena_resize(arena, 8192); }, "AMM shrink");
    arena_call([&] { return mx_arena_clone(arena, &clone); }, "AMM clone");
    arena_call([&] { return sys_map_page(clone, clone_va, RIGHT_READ | RIGHT_WRITE); }, "AMM clone map");
    auto* copy = reinterpret_cast<volatile uint64_t*>(clone_va);
    copy[0] = 99;
    check(data[0] == 11 && copy[0] == 99, "AMM EL0 COW");
    // The kernel must COW a destination before copying syscall output into it.
    check(!mx_arena_inspect(arena, reinterpret_cast<MxArenaInfo*>(base + 4096)), "AMM kernel COW");
    check(copy[512] == 22, "AMM kernel COW retains clone");
    const uint32_t order[]{1, 0};
    arena_call([&] { return mx_arena_reorder(arena, order, 2); }, "AMM reorder");
    check(data[512] == 11, "AMM reorder aliases");
    arena_call([&] { return mx_arena_reorder(arena, order, 2); }, "AMM reorder restore");

    auto worker = sys_thread_create(0, reinterpret_cast<uint64_t>(arena_writer), base,
                                     MX_THREAD_SUSPENDED);
    check(worker > 0, "AMM writer create");
    check(!sys_thread_set_affinity(worker, 1ull << (cpu_count - 1)) && !sys_thread_resume(worker),
          "AMM writer resume");
    for (uint32_t round = 0; round < 8; ++round) {
        arena_call([&] { return mx_arena_precopy(arena); }, "AMM precopy");
        sys_yield();
        const auto before = data[0];
        arena_call([&] { return mx_arena_commit(arena); }, "AMM commit");
        check(data[0] >= before && other[0] >= before, "AMM migration lost concurrent stores");
        check(copy[0] == 99, "AMM migration changed clone");
    }
    __atomic_store_n(&arena_writer_stop, 1ull, __ATOMIC_RELEASE);
    const auto deadline = mx_clock_ns() + 3000000000ull;
    while (!__atomic_load_n(&arena_writer_done, __ATOMIC_ACQUIRE) && mx_clock_ns() < deadline)
        sys_yield();
    check(__atomic_load_n(&arena_writer_done, __ATOMIC_ACQUIRE), "AMM writer stop");
    sys_cap_revoke(worker);
    // A guest attachment owns an external generation lease until all stage-2
    // mappings and attachment references have retired.
    const auto root = mochi::boot::require_handle(boot_info, "vm.root");
    const auto vm = sys_vm_create(root, 0);
    check(vm > 0, "AMM guest VM");
    const auto memory = sys_vm_memory_create(vm, 65536, 0);
    check(memory > 0 && !sys_vm_memory_map(vm, memory, 0x40000000, 65536, RIGHT_READ | RIGHT_WRITE),
          "AMM guest stage2");
    const auto read_only = sys_cap_dup(arena, RIGHT_READ | RIGHT_MAP);
    check(read_only > 0, "AMM read-only guest source");
    check(sys_vm_shared_memory_attach(vm, read_only, 0x49000000, 8192,
                                      RIGHT_READ | RIGHT_WRITE, 0) == MX_ERR_PERM,
          "guest attach cannot amplify arena capability rights");
    const auto read_shared = sys_vm_shared_memory_attach(vm, read_only, 0x49000000,
                                                         8192, RIGHT_READ, 0);
    check(read_shared > 0, "read-only guest attachment");
    check(sys_vm_shared_memory_register_dma(read_shared, 3) == MX_ERR_PERM,
          "DMA cannot amplify read-only guest attachment");
    check(!sys_vm_shared_memory_register_dma(read_shared, 1), "read-only guest DMA registration");
    sys_cap_revoke(read_shared);
    sys_cap_revoke(read_only);
    const auto shared = sys_vm_shared_memory_attach(vm, arena, 0x48000000, 8192,
                                                    RIGHT_READ | RIGHT_WRITE, 0);
    check(shared > 0 && mx_arena_resize(arena, 4096) == MX_ERR_BUSY, "AMM guest pin");
    check(!sys_vm_destroy(vm), "AMM guest retirement");
    sys_cap_revoke(shared);
    arena_call([&] { return mx_arena_realloc(arena, base, base + 4096, 8192); }, "AMM realloc");
    check(*reinterpret_cast<volatile uint64_t*>(base + 4096) == other[0], "AMM realloc contents");
    MxArenaInfo info{};
    check(!mx_arena_inspect(arena, &info) && info.retired_generation && info.zero_copy_changes >= 4,
          "AMM generations");
    check(!mx_arena_pin(arena, info.generation) && mx_arena_precopy(arena) == MX_ERR_BUSY,
          "AMM pin exclusion");
    check(!mx_arena_unpin(arena, info.generation), "AMM unpin");
    arena_call([&] { return mx_arena_resize(arena, 24576); }, "AMM automatic growth");
    check(!mx_arena_inspect(arena, &info) && info.capacity == 24576, "AMM growth capacity");
    arena_call([&] { return mx_arena_resize(arena, 8192); }, "AMM trim size");
    arena_call([&] { return mx_arena_trim(arena); }, "AMM trim");
    arena_call([&] { return mx_arena_policy(arena, MX_ARENA_COMPACT_IDLE); }, "AMM read sampling");
    check(*reinterpret_cast<volatile uint64_t*>(base + 4096) == other[0], "AMM EL0 access flag");
    arena_call([&] { return mx_arena_unmap_cap(arena, base + 4096); }, "AMM detach source");
    arena_call([&] { return mx_arena_unmap_cap(arena, alias); }, "AMM detach alias");
    arena_call([&] { return mx_arena_unmap_cap(clone, clone_va); }, "AMM detach clone");
    sys_cap_revoke(arena); sys_cap_revoke(clone);
    print("arena migration: EL0 COW, resizing, realloc and concurrent writer passed\n");
}

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    boot_info = boot;
    const auto* mode = mochi::boot::find(boot, "mode");
    if (mode && mode->type == MX_BOOT_ENTRY_VALUE && mode->aux0 == kModeVmWorker) {
        vm_owner_worker(boot);
    }
    if (mode && mode->type == MX_BOOT_ENTRY_VALUE && mode->aux0 == kModeSecurityWorker) {
        security_worker(boot);
    }
    supervisor = mochi::boot::require_handle(boot, "supervisor");
    cpu_count = mochi::boot::find(boot, "cpu.count")->aux0;
    auto archive = mochi::boot::require_handle(boot, "boot.initrd");
    check(!sys_map_page(archive, 0x20000000, RIGHT_READ), "bundle mapping");
    bundle.data = reinterpret_cast<const uint8_t*>(0x20000000);
    bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
    (void)libk::copy_n(libk::as_bytes(&bundle.header, sizeof(bundle.header)), libk::as_bytes(bundle.data, sizeof(bundle.header)), sizeof(bundle.header));
    print("lifecycle regression started\n");
    security_authority();

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
    libk::fill(libk::as_bytes(child_boot, sizeof(*child_boot)), 0);
    check(sys_cap_grant(child.process, boot_arena, RIGHT_READ | RIGHT_MAP) == (1ull << 32),
          "bootstrap handle");
    child_boot->magic = MX_BOOT_MAGIC;
    child_boot->version = MX_BOOT_VERSION;
    child_boot->entry_count = 2;
    auto& parent = child_boot->entries[0];
    parent.type = MX_BOOT_ENTRY_HANDLE;
    (void)libk::copy_n(libk::as_bytes(parent.name, 7), libk::as_bytes("parent", 7), 7);
    parent.handle = sys_cap_grant(child.process, mochi::boot::require_handle(boot, "self"),
                                  RIGHT_READ | RIGHT_SPAWN);
    auto& result = child_boot->entries[1];
    result.type = MX_BOOT_ENTRY_ARENA;
    (void)libk::copy_n(libk::as_bytes(result.name, 7), libk::as_bytes("result", 7), 7);
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
    el2_roundtrip();
    managed_arenas();
    normal_memory();
    print("lifecycle regression passed\n");
    while (true) {
        sys_yield();
    }
}
