// userland/init/main.cc — Mochi init process.
//
// Spec ref: §7.1 Init, §7.2 Boot Manifest, §8 V0 Boot Flow steps 11–16
//
// init is the first userspace process. It receives a fixed set of capabilities
// from the kernel (manifest, initrd, supervisor, nameservice placeholder, UART)
// and no other authority.
//
// Responsibilities:
//   1. Parse the boot manifest (static TOML-like structure, not code).
//   2. Spawn services in order: nameservice → io.console → demo.pong → demo.ping.
//   3. Assign principals and pass declared capabilities to each service.
//   4. Register services with nameservice.
//   5. Supervise children: on death, apply restart policy.
//
// init is policy. The kernel does not know service names (spec §11).

#include "mochi/syscall.h"
#include "libk/bytes.h"
#include "libk/str.h"
#include "midix/abi/session.h"
#include "mochi/boot.h"
#include "midix/format/json.h"
#include "midix/format/bundle.h"
#include "midix/format/integrity.h"
using integrity::sha256;
using integrity::bytes_equal;
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/fs.h"
#include "mochi/console_config.h"
#include "midix/abi/dango_guest_layout.h"  // guest layout: VA spans
#include "mochi/svc_registry.h"
#include "mochi/svc_control.h"
#include "midix/abi/boot.h"
#include "midix/abi/service.h"
#include "midix/abi/image.h"
#include "midix/abi/protocols/nameservice.h"
#include "midix/abi/protocols/fs.h"
#include "midix/abi/protocols/gpu_service.h"
#include "supervision.h"

constexpr uint32_t PRINCIPAL_NATIVE_TRUSTED = 1;
constexpr uint32_t PRINCIPAL_NATIVE_SANDBOXED = 2;

constexpr uint32_t kRingOrder = 6;
constexpr uint32_t kFbCmdRingOrder = 10;       // 1024 slots for chatty per-cell tty rendering.
constexpr uint32_t kConsoleMirrorRingOrder = 10; // 48 KiB of UART->TTY mirror backlog.
constexpr uint64_t kVaManifest = 0x2000'0000ull;
// The initrd is mapped as one object starting at kVaInitrd, so every fixed
// mapping above it begins after the layout's initrd span: 64 MiB for the
// appliance initramfs, 384 MiB for the libc rootfs (a 186 MiB initrd used to run
// straight through kVaSvcRegistry and fail the boot).
constexpr uint64_t kVaInitrd = 0x2400'0000ull;
constexpr uint64_t kVaInitLogRing = kVaInitrd + DANGO_INITRD_VA_SPAN;
constexpr uint64_t kVaConsoleConfig  = kVaInitLogRing + 0x2'0000ull;
constexpr uint64_t kVaSvcCtlReqRing = kVaInitLogRing + 0x7'0000ull;
constexpr uint64_t kVaSvcCtlReplyRing = kVaInitLogRing + 0x8'0000ull;
constexpr uint64_t kVaBootInfoBase   = kVaInitLogRing + 0x10'0000'0ull;
constexpr uint64_t kVaSvcRegistry    = kVaInitLogRing + 0x20'0000'0ull;
// Per-service session directories follow the registry (64 KiB each), so they
// move with the layout instead of sitting at a literal that a large initrd
// covers.
constexpr uint64_t kVaSessions       = kVaSvcRegistry + 0x1'0000ull;


static CapHandle kCapManifest, kCapInitrd, kCapSupervisor, kCapUart;
static CapHandle kCapVmRoot, kCapLogDoorbell, kCapFbMemory, kCapXhciMmio, kCapXhciIrq, kCapXhciDma;
static CapHandle kCapVirtioBlkMmio, kCapVirtioBlkDma;
static CapHandle kCapGpuFunction, kCapGpuIrq; // Phase E: appliance GPU authority
constexpr uint32_t kKernelLogRingCount = 4;
static CapHandle kKernelLogRings[kKernelLogRingCount];
static uint64_t g_manifest_size = 0;
static uint64_t g_bundle_size = 0;
static uint32_t g_cpu_count = 1;
static int64_t set_service_affinity(CapHandle thread, uint64_t preferred) {
    const uint64_t online = (1ull << g_cpu_count) - 1;
    return sys_thread_set_affinity(thread, (preferred & online) ? (preferred & online) : 1);
}

constexpr uint32_t kMaxManifestServices = 20;
constexpr uint32_t kManifestScanLimit = 64 * 1024;

static uint8_t g_policy_sha256[MX_SHA256_SIZE]{};
static bool g_policy_sha256_valid = false;

struct ServiceSpec {
    char name[32];
    char image[32];
    char principal[24];
    char restart[16];
    char caps[64];
    const uint8_t* image_data;
    uint64_t image_size;
    CapHandle image_arena;
    CapHandle proc;
    uint64_t identity;
    CapHandle main_thread;
    CapHandle ready_db;
    uint64_t ready_counter;
    uint32_t dependencies;
    uint32_t failures;
    uint64_t retry_at;
    uint32_t uses;
    bool ready;
    bool desired;
    bool restart_pending;
    bool stopping;
    CapHandle log_ring;
    CapHandle boot_arena;
    MxBootInfo* boot_info;
    uint8_t image_sha256[MX_SHA256_SIZE];
    bool image_sha256_valid;
};

struct InitRuntime {
    ServiceSpec* services;
    uint32_t service_count;
    mochi::svc::RegistryPage* svc_registry;
    CapHandle svc_registry_cap;

    CapHandle console_db;
    CapHandle init_log_ring;
    CapHandle console_mirror_ring;
    CapHandle console_mirror_db;
    CapHandle config_arena;

    CapHandle ring_ping_to_pong;
    CapHandle ring_pong_to_ping;
    CapHandle db_pong;
    CapHandle db_ping;
    CapHandle db_ipc_done;
    CapHandle ns_request_ring;
    CapHandle ns_reply_ring;
    CapHandle db_nameservice;
    CapHandle db_ns_reply;

    CapHandle db_dango_done;
    CapHandle db_dango_auth_gate;
    CapHandle db_dango_layout_done;

    CapHandle fb_cmd_ring;
    CapHandle fb_reply_ring;
    CapHandle fb_cmd_db;
    CapHandle fb_reply_db;
    // Phase J: the native GPU service endpoint (`drv.gpu.linux`). Allocated
    // directly rather than through the manifest channel table, because the GPU
    // service is a role hosted by dango.manager, not a spawned service.
    CapHandle gpu_cmd_ring;
    CapHandle gpu_reply_ring;
    CapHandle gpu_cmd_db;
    CapHandle gpu_reply_db;
    CapHandle xhci_hid_ring;
    CapHandle xhci_hid_db;
    CapHandle hid_input_ring;
    CapHandle hid_input_db;
    CapHandle input_tty_ring;
    CapHandle input_tty_db;
    CapHandle blk_req_ring;
    CapHandle blk_reply_ring;
    CapHandle blk_req_db;
    CapHandle blk_reply_db;
    CapHandle fs_cmd_ring;
    CapHandle fs_reply_ring;
    CapHandle fs_cmd_db;
    CapHandle fs_reply_db;
    CapHandle term_input_ring;
    CapHandle term_output_ring;
    CapHandle term_input_db;
    CapHandle term_output_db;

    CapHandle svc_ctl_req_ring;
    CapHandle svc_ctl_reply_ring;
    CapHandle svc_ctl_req_db;
    CapHandle svc_ctl_reply_db;
    mochi::svcctl::Server svcctl;
};

static InitRuntime g_rt{};

static uint32_t slen(const char* s) {
    return static_cast<uint32_t>(libk::str::length(s));
}

static bool streq(const char* a, const char* b) {
    return libk::str::equal(a, b);
}

static bool token_eq(const char* token, const char* literal) {
    return streq(token, literal);
}

static uint32_t parse_manifest_json_services(const char* manifest, ServiceSpec* out) {
    using namespace midix;
    if (!g_manifest_size || g_manifest_size > kManifestScanLimit || manifest[g_manifest_size - 1]) return 0;
    auto doc = json::parse(manifest, g_manifest_size - 1);
    uint64_t schema = 0;
    if (!doc || !json::number(json::member(doc, "schema"), schema) || schema != 2) return 0;
    auto entries = json::member(doc, "services");
    if (!entries || *entries.begin != '[') return 0;
    uint32_t count = 0;
    for (; count < kMaxManifestServices; ++count) {
        auto obj = json::element(entries, count);
        if (!obj) return count;
        auto& svc = out[count];
        svc.desired = true;
        if (!json::string(json::member(obj, "name"), svc.name, sizeof(svc.name)) ||
            !json::string(json::member(obj, "object"), svc.image, sizeof(svc.image)) ||
            !json::string(json::member(obj, "principal"), svc.principal, sizeof(svc.principal)) ||
            !json::string(json::member(obj, "restart"), svc.restart, sizeof(svc.restart))) return 0;
        if ((!streq(svc.principal, "native_trusted") && !streq(svc.principal, "native_sandboxed")) ||
            (!streq(svc.restart, "always") && !streq(svc.restart, "never"))) return 0;
        for (uint32_t i = 0; i < count; ++i) if (streq(out[i].name, svc.name)) return 0;
        auto uses = json::member(obj, "uses");
        for (uint32_t i = 0; ; ++i) {
            auto entry = json::element(uses, i); if (!entry) break;
            char name[32]; if (!json::string(entry, name, sizeof(name))) return 0;
            if (streq(name, "fs")) svc.uses |= 1;
            else if (streq(name, "nameservice")) svc.uses |= 2;
            else return 0;
        }
        auto caps = json::member(obj, "caps");
        if (!caps || *caps.begin != '[') return 0;
        uint32_t pos = 0;
        for (uint32_t i = 0;; ++i) {
            auto cap = json::element(caps, i); if (!cap) break;
            char name[32]; if (!json::string(cap, name, sizeof(name))) return 0;
            if (pos && pos < sizeof(svc.caps)) svc.caps[pos++] = ',';
            for (uint32_t j = 0; name[j]; ++j) {
                if (pos + 1 >= sizeof(svc.caps)) return 0;
                svc.caps[pos++] = name[j];
            }
        }
        if (!pos) svc.caps[pos++] = '-';
        svc.caps[pos] = 0;
    }
    return json::element(entries, count) ? 0 : count;
}

static CapHandle as_cap(int64_t v) {
    if (v <= 0) return kNullCap;
    return static_cast<CapHandle>(v);
}



static void console_log(const char* msg) {
    mochi::log_raw_marker(msg);
}

static const uint8_t* archive_find(const uint8_t* base, const char* wanted,
                                    uint64_t* size, uint8_t hash[MX_SHA256_SIZE]) {
    if (!base || g_bundle_size < sizeof(MxBundleHeader)) return nullptr;
    midix::format::Bundle bundle{};
    bundle.data = base;
    bundle.size = g_bundle_size;
    const auto* src = reinterpret_cast<const uint8_t*>(base);
    (void)libk::copy_n(libk::as_bytes(&bundle.header, sizeof(bundle.header)),
                       libk::as_bytes(src, sizeof(bundle.header)), sizeof(bundle.header));
    if (bundle.header.bundle_size != g_bundle_size || bundle.header.object_count > midix::format::kMaxObjects) return nullptr;
    for (uint32_t i = 1; i < bundle.header.object_count; ++i) {
        MxBundleObject obj; const char* name;
        if (!midix::format::object(bundle, i, obj, name)) return nullptr;
        if (!streq(name, wanted)) continue;
        if (size) *size = obj.size;
        if (hash) for (uint32_t n = 0; n < MX_SHA256_SIZE; ++n) hash[n] = obj.sha256[n];
        return base + obj.offset;
    }
    return nullptr;
}

static uint32_t principal_id(const char* principal) {
    if (token_eq(principal, "native_trusted") || token_eq(principal, "trusted")) {
        return PRINCIPAL_NATIVE_TRUSTED;
    }
    return PRINCIPAL_NATIVE_SANDBOXED;
}

static bool spawn_service(ServiceSpec& spec, uint32_t flags) {
    if (!spec.image_data || spec.image_size == 0) return false;
    SysPair spawned = sys_spawn(spec.image_data, spec.image_size,
                                principal_id(spec.principal),
                                kCapSupervisor,
                                flags,
                                spec.name);
    spec.proc = as_cap(spawned.value);
    spec.main_thread = as_cap(spawned.detail);
    MxCapInfo info{};
    if (spec.proc && sys_cap_info(spec.proc, &info, sizeof(info)) == 0) spec.identity = info.base;
    return spec.proc != kNullCap && spec.main_thread != kNullCap;
}

static ServiceSpec* find_service(ServiceSpec* specs, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (streq(specs[i].name, name)) return &specs[i];
    }
    return nullptr;
}

static uint32_t service_ordinal(const ServiceSpec* svc);

extern "C" [[noreturn]] void fail_halt(const char* marker) {
    sys_debug_puts(marker, slen(marker));
    while (true) {
        sys_yield();
    }
}

static void require_ok(int64_t rc, const char* marker) {
    if (rc < 0) fail_halt(marker);
}

static void boot_copy_name(char* dst, const char* src) {
    uint32_t i = 0;
    if (src) {
        while (i + 1 < MX_BOOT_NAME_MAX && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static bool cap_is_slot_zero(CapHandle h) {
    return (h & 0xFFFF'FFFFull) == 0;
}

static uint64_t load_le64_bytes(const uint8_t* p) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

static void boot_add(ServiceSpec& svc, MxBootEntryType type, const char* name,
                     CapHandle handle, uint64_t aux0 = 0, uint64_t aux1 = 0,
                     uint32_t flags = 0) {
    if (!svc.boot_info) fail_halt("init: bootinfo failed\n");
    if (svc.boot_info->entry_count >= MX_BOOT_MAX_ENTRIES) fail_halt("init: bootinfo full\n");
    MxBootEntry& e = svc.boot_info->entries[svc.boot_info->entry_count++];
    e.type = static_cast<uint32_t>(type);
    e.flags = flags;
    e.handle = handle;
    e.aux0 = aux0;
    e.aux1 = aux1;
    boot_copy_name(e.name, name);
}

static void boot_publish(ServiceSpec& svc) {
    if (!svc.boot_info) fail_halt("init: bootinfo failed\n");
    svc.boot_info->version = MX_BOOT_VERSION;
    __atomic_store_n(&svc.boot_info->magic, MX_BOOT_MAGIC, __ATOMIC_RELEASE);
}

static void boot_init_for_service(ServiceSpec& svc, uint32_t ordinal,
                                  CapHandle nameservice = kNullCap) {
    if (svc.boot_arena == kNullCap || !svc.boot_info) {
        svc.boot_arena = as_cap(sys_arena_create(4096, 0));
        if (svc.boot_arena == kNullCap) fail_halt("init: bootinfo failed\n");
        const uint64_t va = kVaBootInfoBase + (static_cast<uint64_t>(ordinal) * 0x10000ull);
        if (sys_map_page(svc.boot_arena, va, RIGHT_READ | RIGHT_WRITE) != 0) {
            fail_halt("init: bootinfo failed\n");
        }
        svc.boot_info = reinterpret_cast<MxBootInfo*>(va);
    }
    for (uint32_t i = 0; i < sizeof(MxBootInfo); ++i) {
        reinterpret_cast<uint8_t*>(svc.boot_info)[i] = 0;
    }
    svc.boot_info->version = MX_BOOT_VERSION;
    svc.boot_info->entry_count = 0;
    svc.boot_info->self_process = kNullCap; // no ambient self authority in v0
    svc.boot_info->nameservice = nameservice;
    svc.boot_info->supervisor = kNullCap;

    const CapHandle child_boot = as_cap(
        sys_cap_grant(svc.proc, svc.boot_arena, RIGHT_READ | RIGHT_MAP));
    if (child_boot == kNullCap || !cap_is_slot_zero(child_boot)) {
        fail_halt("init: bootinfo slot failed\n");
    }

    if (!svc.ready_db) svc.ready_db = as_cap(sys_doorbell_create());
    if (!svc.ready_db) fail_halt("init: readiness create failed\n");
    svc.ready_counter = sys_doorbell_read(svc.ready_db);
    const CapHandle ready = as_cap(sys_cap_grant(svc.proc, svc.ready_db, RIGHT_NOTIFY));
    if (!ready) fail_halt("init: readiness grant failed\n");
    boot_add(svc, MX_BOOT_ENTRY_DOORBELL, "service.ready", ready);
    boot_add(svc, MX_BOOT_ENTRY_VALUE, "cpu.count", kNullCap, g_cpu_count);
    boot_add(svc, MX_BOOT_ENTRY_VALUE, "launch.id", kNullCap, ordinal, 0);
    if (svc.image_sha256_valid) {
        boot_add(svc, MX_BOOT_ENTRY_VALUE, "image.sha256", kNullCap,
                 load_le64_bytes(svc.image_sha256),
                 load_le64_bytes(svc.image_sha256 + 8));
    }
    if (g_policy_sha256_valid) {
        boot_add(svc, MX_BOOT_ENTRY_VALUE, "policy.sha256", kNullCap,
                 load_le64_bytes(g_policy_sha256),
                 load_le64_bytes(g_policy_sha256 + 8));
    }
}

static CapHandle grant_to(ServiceSpec& svc, CapHandle cap, uint32_t rights,
                          MxBootEntryType type, const char* name,
                          uint64_t aux0 = 0, uint64_t aux1 = 0,
                          uint32_t flags = 0) {
    const CapHandle child = as_cap(sys_cap_grant(svc.proc, cap, rights));
    if (child == kNullCap) fail_halt("init: grant failed\n");
    boot_add(svc, type, name, child, aux0, aux1, flags);
    return child;
}

static CapHandle share_ring_to(ServiceSpec& svc, CapHandle ring, const char* name,
                               uint64_t aux0 = kRingOrder) {
    const CapHandle child = as_cap(
        sys_ring_share(ring, svc.proc, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP));
    if (child == kNullCap) fail_halt("init: ring share failed\n");
    boot_add(svc, MX_BOOT_ENTRY_RING, name, child, aux0, 0);
    return child;
}

static CapHandle share_ring_to_with_rights(ServiceSpec& svc, CapHandle ring,
                                           const char* name, uint32_t rights,
                                           uint64_t aux0 = kRingOrder) {
    const CapHandle child = as_cap(sys_ring_share(ring, svc.proc, rights));
    if (child == kNullCap) fail_halt("init: ring share failed\n");
    boot_add(svc, MX_BOOT_ENTRY_RING, name, child, aux0, 0);
    return child;
}

static CapHandle share_doorbell_to(ServiceSpec& svc, CapHandle db, const char* name,
                                   uint32_t rights = RIGHT_WAIT | RIGHT_NOTIFY | RIGHT_DUP) {
    const CapHandle child = as_cap(sys_doorbell_share(db, svc.proc, rights));
    if (child == kNullCap) fail_halt("init: doorbell share failed\n");
    boot_add(svc, MX_BOOT_ENTRY_DOORBELL, name, child, 0, 0);
    return child;
}

static void share_service_log(ServiceSpec& svc, CapHandle console_db) {
    share_ring_to_with_rights(svc, svc.log_ring, "log.ring",
                              RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    share_doorbell_to(svc, console_db, "log.doorbell", RIGHT_NOTIFY);
}


static void svc_registry_copy(char* dst, uint32_t cap, const char* src) {
    if (!dst || cap == 0) return;
    uint32_t i = 0;
    if (src) {
        while (i + 1 < cap && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static CapHandle create_service_registry(ServiceSpec* services, uint32_t service_count) {
    const CapHandle arena = as_cap(sys_arena_create(4096, 0));
    if (arena == kNullCap) fail_halt("init: svc registry create failed\n");
    if (sys_map_page(arena, kVaSvcRegistry, RIGHT_READ | RIGHT_WRITE) != 0) {
        fail_halt("init: svc registry map failed\n");
    }
    auto* reg = reinterpret_cast<mochi::svc::RegistryPage*>(kVaSvcRegistry);
    for (uint32_t i = 0; i < sizeof(*reg); ++i) {
        reinterpret_cast<uint8_t*>(reg)[i] = 0;
    }
    reg->version = mochi::svc::kRegistryVersion;
    reg->generation = 1;
    const uint32_t count = service_count < mochi::svc::kMaxRegistryEntries
        ? service_count
        : mochi::svc::kMaxRegistryEntries;
    reg->entry_count = static_cast<uint16_t>(count);
    for (uint32_t i = 0; i < count; ++i) {
        mochi::svc::RegistryEntry& e = reg->entries[i];
        svc_registry_copy(e.name, sizeof(e.name), services[i].name);
        svc_registry_copy(e.image, sizeof(e.image), services[i].image);
        svc_registry_copy(e.principal, sizeof(e.principal), services[i].principal);
        svc_registry_copy(e.restart, sizeof(e.restart), services[i].restart);
        e.process = services[i].proc;
        e.main_thread = services[i].main_thread;
        e.state = mochi::svc::kStateSpawned;
        if (services[i].boot_info && services[i].boot_info->magic == MX_BOOT_MAGIC) {
            e.state |= mochi::svc::kStateBootPublished;
        }
        if (services[i].main_thread != kNullCap) {
            e.state |= mochi::svc::kStateRunning;
        }
    }
    mochi::svc::write_end(reg);
    __atomic_store_n(&reg->magic, mochi::svc::kRegistryMagic, __ATOMIC_RELEASE);
    return arena;
}

static void publish_resume_service(ServiceSpec& svc) {
    boot_publish(svc);
    require_ok(sys_thread_resume(svc.main_thread), "init: driver resume failed\n");
}

// Block- and FS-layer smoke tests used to run here. They blocked the boot path
// for ~dozens of round trips per test against services that are already
// covered by the FS image load below (which is the only real consumer of fs).
// Drop them outright.

static uint32_t service_ordinal(const ServiceSpec* svc) {
    if (!svc || !g_rt.services) return 0;
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        if (&g_rt.services[i] == svc) return i;
    }
    return 0;
}

static mochi::svc::RegistryEntry* registry_entry_for(const ServiceSpec* svc) {
    if (!svc || !g_rt.services || !g_rt.svc_registry) return nullptr;
    for (uint32_t i = 0; i < g_rt.service_count && i < g_rt.svc_registry->entry_count; ++i) {
        if (&g_rt.services[i] == svc) return &g_rt.svc_registry->entries[i];
    }
    return nullptr;
}

static uint32_t registry_state_for(const ServiceSpec& svc) {
    uint32_t state = 0;
    if (svc.proc != kNullCap) state |= mochi::svc::kStateSpawned;
    if (svc.proc != kNullCap && svc.boot_info && svc.boot_info->magic == MX_BOOT_MAGIC) state |= mochi::svc::kStateBootPublished;
    if (svc.ready) state |= mochi::svc::kStateRunning;
    return state;
}

static void registry_sync(ServiceSpec& svc) {
    mochi::svc::RegistryEntry* e = registry_entry_for(&svc);
    if (!e) return;
    mochi::svc::write_begin(g_rt.svc_registry);
    e->process = svc.proc;
    e->main_thread = svc.main_thread;
    e->state = registry_state_for(svc);
    mochi::svc::write_end(g_rt.svc_registry);
}

static void console_config_reset() {
    auto* cfg = reinterpret_cast<ConsoleBootConfig*>(kVaConsoleConfig);
    for (uint32_t i = 0; i < sizeof(ConsoleBootConfig); ++i) {
        reinterpret_cast<uint8_t*>(cfg)[i] = 0;
    }
}

struct ClientSession {
    ServiceSpec* client;
    CapHandle request, reply, request_db, reply_db;
    uint64_t identity;
};
struct SessionServer {
    const char* name;
    uint32_t use;
    CapHandle arena;
    MxSessionDirectory* directory;
    ClientSession clients[MX_MAX_SESSIONS];
    uint64_t exports[MX_MAX_EXPORTS];
};
static SessionServer g_servers[2]{{"io.fs", 1, 0, nullptr, {}, {}}, {"nameservice", 2, 0, nullptr, {}, {}}};

static void create_sessions() {
    for (uint32_t index = 0; index < 2; ++index) {
        auto& server = g_servers[index];
        if (!find_service(g_rt.services, g_rt.service_count, server.name)) continue;
        server.arena = as_cap(sys_arena_create(4096, 0));
        const uint64_t va = kVaSessions + index * 0x10000;
        if (!server.arena) {
            fail_halt("init: sessions arena create failed\n");
        }
        if (sys_map_page(server.arena, va, RIGHT_READ | RIGHT_WRITE)) {
            // Distinguish "the arena could not be mapped here" from a capacity
            // failure: a layout that moves the windows can collide with it.
            fail_halt("init: sessions map failed (va out of window?)\n");
        }
        server.directory = reinterpret_cast<MxSessionDirectory*>(va);
        *server.directory = {};
        server.directory->magic = MX_SESSION_MAGIC;
        for (uint32_t i = 0; i < g_rt.service_count; ++i) {
            auto& svc = g_rt.services[i];
            if (!(svc.uses & server.use)) continue;
            if (server.directory->count == MX_MAX_SESSIONS) fail_halt("init: too many sessions\n");
            auto& client = server.clients[server.directory->count++];
            client.client = &svc;
            client.request = as_cap(sys_ring_create(kRingOrder, 0));
            client.reply = as_cap(sys_ring_create(kRingOrder, 0));
            client.request_db = as_cap(sys_doorbell_create());
            client.reply_db = as_cap(sys_doorbell_create());
            if (!client.request || !client.reply || !client.request_db || !client.reply_db) fail_halt("init: session create failed\n");
        }
    }
}

static void refresh_exports() {
    auto& server = g_servers[1];
    auto* nameservice = find_service(g_rt.services, g_rt.service_count, server.name);
    if (!server.directory || !nameservice || !nameservice->proc) return;
    // Never grant into a child before its BootInfo arena has claimed slot 0.
    if (!nameservice->boot_info) return;
    for (uint32_t i = 0; i < g_rt.service_count && i < MX_MAX_EXPORTS; ++i) {
        auto& svc = g_rt.services[i];
        auto& entry = server.directory->exports[i];
        if (server.exports[i] == svc.identity) continue;
        boot_copy_name(entry.name, svc.name);
        entry.rights = RIGHT_READ;
        entry.protocol = streq(svc.name, "io.fs") ? MX_PROTOCOL_FS : 0;
        // Phase J: publish the Mochi-facing GPU service name. The V0 export
        // catalog holds exactly one entry per spawned service
        // (MX_MAX_EXPORTS == service count), and `drv.gpu.linux` is a role hosted
        // by dango.manager rather than a process of its own, so the name is
        // published from the hosting process's slot. Resolving it yields the
        // manager with the GPU service protocol, which is what a client of the
        // service needs to identify it; dango.manager itself stays enumerable
        // through the service registry. Without the appliance the catalog is
        // exactly what it was before.
        if (g_rt.gpu_cmd_ring != kNullCap && streq(svc.name, "dango.manager")) {
            boot_copy_name(entry.name, "drv.gpu.linux");
            entry.protocol = MX_PROTOCOL_GPU_SERVICE;
        }
        const CapHandle cap = svc.proc ? as_cap(sys_cap_grant(nameservice->proc, svc.proc, RIGHT_READ | RIGHT_DUP)) : kNullCap;
        __atomic_store_n(&entry.process, cap, __ATOMIC_RELEASE);
        server.exports[i] = svc.identity;
    }
}

static void wire_session_server(ServiceSpec& svc, SessionServer& server) {
    grant_to(svc, server.arena, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, MX_BOOT_ENTRY_ARENA, "sessions");
    for (uint32_t i = 0; i < server.directory->count; ++i) {
        auto& client = server.clients[i];
        auto& slot = server.directory->slots[i];
        slot.client = client.client->proc ? as_cap(sys_cap_grant(svc.proc, client.client->proc, RIGHT_READ | RIGHT_SPAWN)) : kNullCap;
        client.identity = client.client->identity;
        slot.request_ring = as_cap(sys_ring_share(client.request, svc.proc, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
        slot.reply_ring = as_cap(sys_ring_share(client.reply, svc.proc, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
        slot.request_doorbell = as_cap(sys_doorbell_share(client.request_db, svc.proc, RIGHT_WAIT));
        slot.reply_doorbell = as_cap(sys_doorbell_share(client.reply_db, svc.proc, RIGHT_NOTIFY));
        slot.order = kRingOrder;
        slot.acknowledged = 0;
        __atomic_add_fetch(&slot.generation, 1ull, __ATOMIC_RELEASE);
    }
    if (server.use == 2) {
        for (auto& identity : server.exports) identity = ~0ull;
        refresh_exports();
    }
}

static bool wire_client_sessions(ServiceSpec& svc) {
    for (auto& server : g_servers) {
        if (!(svc.uses & server.use)) continue;
        auto* service = find_service(g_rt.services, g_rt.service_count, server.name);
        if (!service || !service->ready) return false;
        for (uint32_t i = 0; i < server.directory->count; ++i) {
            auto& client = server.clients[i];
            if (client.client != &svc) continue;
            auto& slot = server.directory->slots[i];
            if (client.identity != svc.identity) {
                slot.client = as_cap(sys_cap_grant(service->proc, svc.proc, RIGHT_READ | RIGHT_SPAWN));
                client.identity = svc.identity;
                __atomic_add_fetch(&slot.generation, 1ull, __ATOMIC_RELEASE);
            }
            const auto generation = __atomic_load_n(&slot.generation, __ATOMIC_ACQUIRE);
            const auto deadline = mx_clock_ns() + 5000000000ull;
            while (__atomic_load_n(&slot.acknowledged, __ATOMIC_ACQUIRE) != generation) {
                if (mx_clock_ns() >= deadline || sys_cap_identify(service->proc) < 0) return false;
                sys_yield();
            }
            const bool fs = server.use == 1;
            share_ring_to_with_rights(svc, client.request, fs ? "fs.cmd.ring" : "ns.tx", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_ring_to_with_rights(svc, client.reply, fs ? "fs.reply.ring" : "ns.rx", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(svc, client.request_db, fs ? "fs.cmd.doorbell" : "ns.server.db", RIGHT_NOTIFY);
            share_doorbell_to(svc, client.reply_db, fs ? "fs.reply.doorbell" : "ns.reply.db", RIGHT_WAIT);
            grant_to(svc, service->proc, RIGHT_READ, MX_BOOT_ENTRY_HANDLE, fs ? "fs.peer" : "ns.peer");
        }
    }
    return true;
}

static bool wire_and_resume_service(ServiceSpec& svc) {
    if (!wire_client_sessions(svc)) return false;
    if (streq(svc.name, "nameservice")) {
        wire_session_server(svc, g_servers[1]);
        share_service_log(svc, g_rt.console_db);
        publish_resume_service(svc);
        return true;
    }
    if (streq(svc.name, "io.console")) {
        console_config_reset();
        auto* cfg = reinterpret_cast<ConsoleBootConfig*>(kVaConsoleConfig);
        (void)grant_to(svc, g_rt.config_arena, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                       MX_BOOT_ENTRY_ARENA, "console.config");
        const CapHandle cons_uart = grant_to(svc, kCapUart,
                                             RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                                             MX_BOOT_ENTRY_MMIO, "uart.mmio");
        const CapHandle cons_doorbell =
            share_doorbell_to(svc, g_rt.console_db, "log.doorbell", RIGHT_WAIT);
        share_ring_to_with_rights(svc, g_rt.console_mirror_ring, "tty.mirror.ring",
                                  RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                                  kConsoleMirrorRingOrder);
        share_doorbell_to(svc, g_rt.console_mirror_db, "tty.mirror.doorbell", RIGHT_NOTIFY);
        uint32_t ring_idx = 0;
        for (uint32_t cpu = 0; cpu < kKernelLogRingCount; ++cpu) {
            const CapHandle h = as_cap(sys_ring_share(kKernelLogRings[cpu], svc.proc,
                                                     RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
            if (h == kNullCap || ring_idx >= ConsoleBootConfig::kMaxRings) fail_halt("init: console restart failed\n");
            cfg->log_ring_caps[ring_idx++] = h;
        }
        {
            const CapHandle h = as_cap(sys_ring_share(g_rt.init_log_ring, svc.proc,
                                                     RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
            if (h == kNullCap || ring_idx >= ConsoleBootConfig::kMaxRings) fail_halt("init: console restart failed\n");
            cfg->log_ring_caps[ring_idx++] = h;
        }
        for (uint32_t i = 0; i < g_rt.service_count; ++i) {
            if (g_rt.services[i].log_ring == kNullCap) continue;
            const CapHandle h = as_cap(sys_ring_share(g_rt.services[i].log_ring, svc.proc,
                                                     RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
            if (h == kNullCap || ring_idx >= ConsoleBootConfig::kMaxRings) fail_halt("init: console restart failed\n");
            cfg->log_ring_caps[ring_idx++] = h;
        }
        cfg->uart_cap = cons_uart;
        cfg->log_doorbell_cap = cons_doorbell;
        cfg->term_input_ring_cap = 0;
        cfg->term_output_ring_cap = 0;
        cfg->term_input_doorbell_cap = 0;
        cfg->term_output_doorbell_cap = 0;
        cfg->total_ring_count = ring_idx;
        cfg->version = ConsoleBootConfig::kVersion;
        cfg->flags = 0;
        __atomic_store_n(&cfg->magic, ConsoleBootConfig::kMagic, __ATOMIC_RELEASE);
        boot_publish(svc);
        require_ok(set_service_affinity(svc.main_thread, 1ull << 1), "init: service start failed\n");
        require_ok(sys_thread_resume(svc.main_thread), "init: service start failed\n");
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "demo.pong")) {
        share_ring_to(svc, g_rt.ring_ping_to_pong, "ipc.rx");
        share_ring_to(svc, g_rt.ring_pong_to_ping, "ipc.tx");
        share_doorbell_to(svc, g_rt.db_pong, "ipc.self.db");
        share_doorbell_to(svc, g_rt.db_ping, "ipc.peer.db");
        share_ring_to(svc, svc.log_ring, "log.ring");
        share_doorbell_to(svc, g_rt.console_db, "log.doorbell");
        boot_publish(svc);
        require_ok(set_service_affinity(svc.main_thread, 1ull << 1), "init: service start failed\n");
        require_ok(sys_thread_resume(svc.main_thread), "init: service start failed\n");
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "demo.ping")) {
        share_ring_to(svc, g_rt.ring_ping_to_pong, "ipc.tx");
        share_ring_to(svc, g_rt.ring_pong_to_ping, "ipc.rx");
        share_doorbell_to(svc, g_rt.db_pong, "ipc.peer.db");
        share_doorbell_to(svc, g_rt.db_ping, "ipc.self.db");
        share_ring_to(svc, svc.log_ring, "log.ring");
        share_doorbell_to(svc, g_rt.console_db, "log.doorbell");
        share_doorbell_to(svc, g_rt.db_ipc_done, "ipc.done", RIGHT_NOTIFY | RIGHT_DUP);
        boot_publish(svc);
        require_ok(set_service_affinity(svc.main_thread, 1ull << 0), "init: service start failed\n");
        require_ok(sys_thread_resume(svc.main_thread), "init: service start failed\n");
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "dango.manager")) {
        share_ring_to(svc, svc.log_ring, "log.ring");
        share_doorbell_to(svc, g_rt.console_db, "log.doorbell");
        grant_to(svc, kCapVmRoot, RIGHT_VM_CREATE | RIGHT_VM_INSPECT, MX_BOOT_ENTRY_HANDLE, "vm.root");
        share_doorbell_to(svc, g_rt.db_dango_done, "dango.done.db", RIGHT_NOTIFY);
        share_doorbell_to(svc, g_rt.db_dango_auth_gate, "dango.auth_gate.db", RIGHT_WAIT);
        share_doorbell_to(svc, g_rt.db_dango_layout_done, "dango.layout_done.db", RIGHT_NOTIFY);
        grant_to(svc, kCapInitrd, RIGHT_READ | RIGHT_MAP, MX_BOOT_ENTRY_HANDLE, "boot.initrd");
        // Phase E: pass the assignable GPU function (and its INTx) to Dango so it
        // can build the appliance assignment (spec §6.1, §20 Phase E).
        if (kCapGpuFunction != kNullCap) {
            grant_to(svc, kCapGpuFunction, RIGHT_READ | RIGHT_WRITE, MX_BOOT_ENTRY_HANDLE,
                     "gpu.function");
        }
        if (kCapGpuIrq != kNullCap) {
            grant_to(svc, kCapGpuIrq, RIGHT_READ | RIGHT_WRITE, MX_BOOT_ENTRY_HANDLE, "gpu.irq");
        }
        // Phase J: the server half of the native GPU service runs here, because
        // this process owns the Dango channel. It gets the other side of the
        // endpoint and read-only access to the framebuffer window it mirrors
        // into the appliance staging window.
        if (g_rt.gpu_cmd_ring != kNullCap) {
            share_ring_to_with_rights(svc, g_rt.gpu_cmd_ring, "gpu.cmd.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_ring_to_with_rights(svc, g_rt.gpu_reply_ring, "gpu.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(svc, g_rt.gpu_cmd_db, "gpu.cmd.doorbell", RIGHT_WAIT);
            share_doorbell_to(svc, g_rt.gpu_reply_db, "gpu.reply.doorbell", RIGHT_NOTIFY);
            if (sys_cap_identify(kCapFbMemory) == kCapTypeMmio) {
                grant_to(svc, kCapFbMemory, RIGHT_READ | RIGHT_MAP, MX_BOOT_ENTRY_MMIO, "fb.memory");
            }
        }
        boot_publish(svc);
        require_ok(set_service_affinity(svc.main_thread, 1ull << 1), "init: service start failed\n");
        require_ok(sys_thread_resume(svc.main_thread), "init: service start failed\n");
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "dango.auth_probe")) {
        share_ring_to(svc, svc.log_ring, "log.ring");
        share_doorbell_to(svc, g_rt.console_db, "log.doorbell");
        grant_to(svc, kCapVmRoot, RIGHT_VM_INSPECT, MX_BOOT_ENTRY_HANDLE, "vm.root.restricted");
        share_doorbell_to(svc, g_rt.db_dango_auth_gate, "dango.auth_gate.db", RIGHT_NOTIFY);
        boot_publish(svc);
        require_ok(set_service_affinity(svc.main_thread, 1ull << 0), "init: service start failed\n");
        require_ok(sys_thread_resume(svc.main_thread), "init: service start failed\n");
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "io.framebuffer")) {
        share_service_log(svc, g_rt.console_db);
        if (sys_cap_identify(kCapFbMemory) == kCapTypeMmio)
            grant_to(svc, kCapFbMemory, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, MX_BOOT_ENTRY_MMIO, "fb.memory");
        // Phase J: the client half of the appliance endpoint. Absent without the
        // GPU function, so the framebuffer stays ramfb-only on every other boot.
        if (g_rt.gpu_cmd_ring != kNullCap) {
            share_ring_to_with_rights(svc, g_rt.gpu_cmd_ring, "gpu.cmd.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_ring_to_with_rights(svc, g_rt.gpu_reply_ring, "gpu.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(svc, g_rt.gpu_cmd_db, "gpu.cmd.doorbell", RIGHT_NOTIFY);
            share_doorbell_to(svc, g_rt.gpu_reply_db, "gpu.reply.doorbell", RIGHT_WAIT);
        }
        share_ring_to_with_rights(svc, g_rt.fb_cmd_ring, "fb.cmd.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, kFbCmdRingOrder);
        share_ring_to_with_rights(svc, g_rt.fb_reply_ring, "fb.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.fb_cmd_db, "fb.cmd.doorbell", RIGHT_WAIT);
        share_doorbell_to(svc, g_rt.fb_reply_db, "fb.reply.doorbell", RIGHT_NOTIFY);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "tty.fb0")) {
        share_service_log(svc, g_rt.console_db);
        share_ring_to_with_rights(svc, g_rt.fb_cmd_ring, "fb.cmd.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, kFbCmdRingOrder);
        share_ring_to_with_rights(svc, g_rt.fb_reply_ring, "fb.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.fb_cmd_db, "fb.cmd.doorbell", RIGHT_NOTIFY);
        share_doorbell_to(svc, g_rt.fb_reply_db, "fb.reply.doorbell", RIGHT_WAIT);
        share_ring_to_with_rights(svc, g_rt.console_mirror_ring, "console.mirror.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, kConsoleMirrorRingOrder);
        share_doorbell_to(svc, g_rt.console_mirror_db, "console.mirror.doorbell", RIGHT_WAIT);
        share_ring_to_with_rights(svc, g_rt.input_tty_ring, "input.tty.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.input_tty_db, "input.tty.doorbell", RIGHT_WAIT);
        share_ring_to_with_rights(svc, g_rt.term_input_ring, "term.input.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.term_input_db, "term.input.doorbell", RIGHT_NOTIFY);
        share_ring_to_with_rights(svc, g_rt.term_output_ring, "term.output.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.term_output_db, "term.output.doorbell", RIGHT_WAIT);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "ui.input")) {
        share_service_log(svc, g_rt.console_db);
        share_ring_to_with_rights(svc, g_rt.hid_input_ring, "hid.input.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.hid_input_db, "hid.input.doorbell", RIGHT_WAIT);
        share_ring_to_with_rights(svc, g_rt.input_tty_ring, "input.tty.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.input_tty_db, "input.tty.doorbell", RIGHT_NOTIFY);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "drv.usb.hid")) {
        share_service_log(svc, g_rt.console_db);
        share_ring_to_with_rights(svc, g_rt.xhci_hid_ring, "xhci.hid.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.xhci_hid_db, "xhci.hid.doorbell", RIGHT_WAIT);
        share_ring_to_with_rights(svc, g_rt.hid_input_ring, "hid.input.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.hid_input_db, "hid.input.doorbell", RIGHT_NOTIFY);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "drv.usb.xhci")) {
        share_service_log(svc, g_rt.console_db);
        if (sys_cap_identify(kCapXhciMmio) == kCapTypeMmio) {
            grant_to(svc, kCapXhciMmio, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, MX_BOOT_ENTRY_MMIO, "xhci.mmio");
        }
        if (sys_cap_identify(kCapXhciIrq) == kCapTypeIrq) {
            grant_to(svc, kCapXhciIrq, RIGHT_READ | RIGHT_WRITE, MX_BOOT_ENTRY_HANDLE, "xhci.irq");
        }
        if (sys_cap_identify(kCapXhciDma) == kCapTypeArena) {
            grant_to(svc, kCapXhciDma, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, MX_BOOT_ENTRY_ARENA, "xhci.dma");
        }
        share_ring_to_with_rights(svc, g_rt.xhci_hid_ring, "xhci.hid.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.xhci_hid_db, "xhci.hid.doorbell", RIGHT_NOTIFY);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "drv.pci")) {
        share_service_log(svc, g_rt.console_db);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "drv.blk.virtio")) {
        share_service_log(svc, g_rt.console_db);
        if (sys_cap_identify(kCapVirtioBlkMmio) == kCapTypeMmio) {
            grant_to(svc, kCapVirtioBlkMmio, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, MX_BOOT_ENTRY_MMIO, "virtio.blk.mmio");
        }
        if (sys_cap_identify(kCapVirtioBlkDma) == kCapTypeArena) {
            grant_to(svc, kCapVirtioBlkDma, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, MX_BOOT_ENTRY_ARENA, "virtio.blk.dma");
        }
        share_ring_to_with_rights(svc, g_rt.blk_req_ring, "blk.req.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_ring_to_with_rights(svc, g_rt.blk_reply_ring, "blk.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.blk_req_db, "blk.req.doorbell", RIGHT_WAIT);
        share_doorbell_to(svc, g_rt.blk_reply_db, "blk.reply.doorbell", RIGHT_NOTIFY);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "io.fs")) {
        share_service_log(svc, g_rt.console_db);
        if (sys_cap_identify(kCapVirtioBlkDma) == kCapTypeArena) {
            grant_to(svc, kCapVirtioBlkDma,
                     RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                     MX_BOOT_ENTRY_ARENA, "virtio.blk.dma");
        }
        wire_session_server(svc, g_servers[0]);
        share_ring_to_with_rights(svc, g_rt.blk_req_ring, "blk.req.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_ring_to_with_rights(svc, g_rt.blk_reply_ring, "blk.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.blk_req_db, "blk.req.doorbell", RIGHT_NOTIFY);
        share_doorbell_to(svc, g_rt.blk_reply_db, "blk.reply.doorbell", RIGHT_WAIT);
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    if (streq(svc.name, "msh")) {
        share_service_log(svc, g_rt.console_db);
        share_ring_to_with_rights(svc, g_rt.term_input_ring, "term.input.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.term_input_db, "term.input.doorbell", RIGHT_WAIT);
        share_ring_to_with_rights(svc, g_rt.term_output_ring, "term.output.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.term_output_db, "term.output.doorbell", RIGHT_NOTIFY);
        grant_to(svc, g_rt.svc_registry_cap, RIGHT_READ | RIGHT_MAP, MX_BOOT_ENTRY_ARENA, "svc.registry");
        if (g_rt.svc_ctl_req_ring != kNullCap) {
            share_ring_to_with_rights(svc, g_rt.svc_ctl_req_ring, "svc.cmd.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(svc, g_rt.svc_ctl_req_db, "svc.cmd.doorbell", RIGHT_NOTIFY);
            share_ring_to_with_rights(svc, g_rt.svc_ctl_reply_ring, "svc.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(svc, g_rt.svc_ctl_reply_db, "svc.reply.doorbell", RIGHT_WAIT);
        }
        publish_resume_service(svc);
        registry_sync(svc);
        return true;
    }
    // Applications use the same declared sessions and log endpoint as services.
    share_service_log(svc, g_rt.console_db);
    publish_resume_service(svc);
    return true;
}

static void create_channels() {
    struct Resource { const char* name; CapHandle* handle; uint32_t order; };
    Resource resources[] = {
        {"init_log_ring", &g_rt.init_log_ring, 6},
        {"console_mirror_ring", &g_rt.console_mirror_ring, 10},
        {"console_mirror_db", &g_rt.console_mirror_db, 0},
        {"ring_ping_to_pong", &g_rt.ring_ping_to_pong, 6},
        {"ring_pong_to_ping", &g_rt.ring_pong_to_ping, 6},
        {"db_pong", &g_rt.db_pong, 0},
        {"db_ping", &g_rt.db_ping, 0},
        {"db_ipc_done", &g_rt.db_ipc_done, 0},
        {"ns_request_ring", &g_rt.ns_request_ring, 6},
        {"ns_reply_ring", &g_rt.ns_reply_ring, 6},
        {"db_nameservice", &g_rt.db_nameservice, 0},
        {"db_ns_reply", &g_rt.db_ns_reply, 0},
        {"db_dango_done", &g_rt.db_dango_done, 0},
        {"db_dango_auth_gate", &g_rt.db_dango_auth_gate, 0},
        {"db_dango_layout_done", &g_rt.db_dango_layout_done, 0},
        {"fb_cmd_ring", &g_rt.fb_cmd_ring, 10},
        {"fb_reply_ring", &g_rt.fb_reply_ring, 6},
        {"fb_cmd_db", &g_rt.fb_cmd_db, 0},
        {"fb_reply_db", &g_rt.fb_reply_db, 0},
        {"xhci_hid_ring", &g_rt.xhci_hid_ring, 6},
        {"xhci_hid_db", &g_rt.xhci_hid_db, 0},
        {"hid_input_ring", &g_rt.hid_input_ring, 6},
        {"hid_input_db", &g_rt.hid_input_db, 0},
        {"input_tty_ring", &g_rt.input_tty_ring, 6},
        {"input_tty_db", &g_rt.input_tty_db, 0},
        {"blk_req_ring", &g_rt.blk_req_ring, 6},
        {"blk_reply_ring", &g_rt.blk_reply_ring, 6},
        {"blk_req_db", &g_rt.blk_req_db, 0},
        {"blk_reply_db", &g_rt.blk_reply_db, 0},
        {"fs_cmd_ring", &g_rt.fs_cmd_ring, 6},
        {"fs_reply_ring", &g_rt.fs_reply_ring, 6},
        {"fs_cmd_db", &g_rt.fs_cmd_db, 0},
        {"fs_reply_db", &g_rt.fs_reply_db, 0},
        {"term_input_ring", &g_rt.term_input_ring, 6},
        {"term_output_ring", &g_rt.term_output_ring, 6},
        {"term_input_db", &g_rt.term_input_db, 0},
        {"term_output_db", &g_rt.term_output_db, 0},
        {"svc_ctl_req_ring", &g_rt.svc_ctl_req_ring, 6},
        {"svc_ctl_reply_ring", &g_rt.svc_ctl_reply_ring, 6},
        {"svc_ctl_req_db", &g_rt.svc_ctl_req_db, 0},
        {"svc_ctl_reply_db", &g_rt.svc_ctl_reply_db, 0},
    };
    auto doc = midix::json::parse(reinterpret_cast<const char*>(kVaManifest), g_manifest_size - 1);
    auto channels = midix::json::member(doc, "channels");
    for (auto& resource : resources) {
        bool found = false;
        for (uint32_t i = 0; ; ++i) {
            auto entry = midix::json::element(channels, i);
            if (!entry) break;
            char name[48], kind[16]; uint64_t order = 0;
            if (!midix::json::string(midix::json::member(entry, "name"), name, sizeof(name)) ||
                !midix::json::string(midix::json::member(entry, "kind"), kind, sizeof(kind)))
                fail_halt("init: invalid channel\n");
            if (!streq(name, resource.name)) continue;
            if (found) fail_halt("init: duplicate channel\n");
            found = true;
            if (resource.order) {
                if (!streq(kind, "ring") || !midix::json::number(midix::json::member(entry, "order"), order) || order != resource.order)
                    fail_halt("init: incompatible channel\n");
                *resource.handle = as_cap(sys_ring_create(order, 0));
            } else {
                if (!streq(kind, "doorbell")) fail_halt("init: invalid channel kind\n");
                *resource.handle = as_cap(sys_doorbell_create());
            }
        }
        if (!found || !*resource.handle) fail_halt("init: channel allocation failed\n");
    }
}

// Phase J: the native GPU service endpoint. A GPU service is a role hosted by
// dango.manager (the process that owns the Dango channel), so it is not a
// spawned service and the boot manifest's fixed channel table has no slot for
// it. The endpoint is therefore allocated directly here and handed to both
// halves, exactly as the log and session rings are. Without the appliance the
// endpoint does not exist at all, and every GPU path stays off.
static void create_gpu_service_channels() {
    if (kCapGpuFunction == kNullCap) return;
    g_rt.gpu_cmd_ring = as_cap(sys_ring_create(kRingOrder, 0));
    g_rt.gpu_reply_ring = as_cap(sys_ring_create(kRingOrder, 0));
    g_rt.gpu_cmd_db = as_cap(sys_doorbell_create());
    g_rt.gpu_reply_db = as_cap(sys_doorbell_create());
    if (!g_rt.gpu_cmd_ring || !g_rt.gpu_reply_ring ||
        !g_rt.gpu_cmd_db || !g_rt.gpu_reply_db) {
        fail_halt("init: gpu channel failed\n");
    }
}

static void parse_dependencies() {
    using namespace midix;
    auto doc = json::parse(reinterpret_cast<const char*>(kVaManifest), g_manifest_size - 1);
    auto entries = json::member(doc, "services");
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        auto deps = json::member(json::element(entries, i), "start_after");
        if (!deps || *deps.begin != '[') fail_halt("init: missing dependencies\n");
        for (uint32_t j = 0; ; ++j) {
            auto dep = json::element(deps, j); if (!dep) break;
            char name[32];
            if (!json::string(dep, name, sizeof(name))) fail_halt("init: invalid dependency\n");
            auto* other = find_service(g_rt.services, g_rt.service_count, name);
            if (!other || other == &g_rt.services[i]) fail_halt("init: unknown dependency\n");
            g_rt.services[i].dependencies |= 1u << service_ordinal(other);
        }
    }
    uint32_t done = 0;
    for (uint32_t pass = 0; pass < g_rt.service_count; ++pass)
        for (uint32_t i = 0; i < g_rt.service_count; ++i)
            if (!(g_rt.services[i].dependencies & ~done)) done |= 1u << i;
    if (done != (1u << g_rt.service_count) - 1) fail_halt("init: dependency cycle\n");
}

static bool dependencies_ready(const ServiceSpec& svc) {
    for (uint32_t i = 0; i < g_rt.service_count; ++i)
        if ((svc.dependencies & (1u << i)) && !g_rt.services[i].ready) return false;
    return true;
}

static bool drain_exits() {
    bool progress = false;
    for (;;) {
        const auto event = sys_supervisor_poll(kCapSupervisor);
        if (event.value <= 0) break;
        progress = true;
        // Exit records are generation-tagged: match the record to the exact
        // incarnation that died, never to a same-named later one.
        uint64_t identities[kMaxManifestServices];
        for (uint32_t i = 0; i < g_rt.service_count; ++i) identities[i] = g_rt.services[i].identity;
        const uint32_t index = supervision::match_identity(
            identities, g_rt.service_count, static_cast<uint64_t>(event.value));
        if (index == g_rt.service_count) continue;  // stale/foreign record
        auto& svc = g_rt.services[index];
        svc.proc = svc.main_thread = kNullCap;
        svc.identity = 0;
        svc.ready = false;
        const auto decision = supervision::restart_decision(
            svc.stopping, svc.desired, streq(svc.restart, "always"),
            svc.restart_pending, svc.failures);
        svc.stopping = false;
        if (decision.failed && svc.failures < supervision::kFailureCap) ++svc.failures;
        svc.restart_pending = decision.restart;
        if (decision.delay_ns) svc.retry_at = mx_clock_ns() + decision.delay_ns;
        registry_sync(svc);
    }
    return progress;
}

static bool await_stopped(uint32_t mask) {
    const uint64_t deadline = mx_clock_ns() + 5000000000ull;
    while (mx_clock_ns() < deadline) {
        drain_exits();
        bool stopped = true;
        for (uint32_t i = 0; i < g_rt.service_count; ++i)
            if ((mask & (1u << i)) && g_rt.services[i].proc) stopped = false;
        if (stopped) return true;
        sys_yield();
    }
    return false;
}

static uint32_t dependent_mask(uint32_t mask) {
    uint32_t dependencies[kMaxManifestServices];
    for (uint32_t i = 0; i < g_rt.service_count; ++i) dependencies[i] = g_rt.services[i].dependencies;
    return supervision::dependent_closure(dependencies, g_rt.service_count, mask);
}

static bool stop_mask(uint32_t mask) {
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        auto& svc = g_rt.services[i];
        if (!(mask & (1u << i))) continue;
        svc.ready = false;
        svc.restart_pending = svc.desired;
        if (!svc.proc) continue;
        // Mark the kill as deliberate so drain_exits() does not charge it to the
        // crash-loop backoff: an intended stop must not look like a failure.
        svc.stopping = true;
        if (sys_kill(svc.proc) < 0 && sys_cap_identify(svc.proc) >= 0) {
            svc.stopping = false;
            return false;
        }
    }
    return await_stopped(mask);
}

static bool await_ready(ServiceSpec& svc) {
    const uint64_t deadline = mx_clock_ns() + 30000000000ull;
    while (mx_clock_ns() < deadline) {
        if (sys_doorbell_read(svc.ready_db) != svc.ready_counter) {
            svc.ready = true;
            svc.restart_pending = false;
            svc.failures = 0;  // healthy again: the next crash backoff starts over
            registry_sync(svc);
            return true;
        }
        drain_exits();
        if (!svc.proc) return false;
        sys_yield();
    }
    console_log("service readiness timeout\n");
    // Guard: name the service that missed its deadline.  An unnamed timeout is
    // very expensive to debug (which service starved?), and this path is cold.
    sys_debug_puts("init: not ready: ", 16);
    sys_debug_puts(svc.name, slen(svc.name));
    sys_debug_puts("\n", 1);
    return false;
}

static bool start_service_now(ServiceSpec& svc) {
    if (svc.ready || !dependencies_ready(svc)) return false;
    svc.desired = true;
    // A control request can race a death: the incarnation is gone but its exit
    // record has not been drained yet. Drop the stale generation before granting
    // any capability into it, otherwise the next grant targets a dead process.
    if (svc.proc && sys_cap_identify(svc.proc) < 0) {
        svc.proc = svc.main_thread = kNullCap;
        svc.identity = 0;
        svc.ready = false;
    }
    if (!svc.proc && !spawn_service(svc, MX_SPAWN_SUSPENDED)) return false;
    // Order is load-bearing: BootInfo must claim the child's cap slot 0 before any
    // other capability is granted into it (mochi::boot::kBootInfoCap == slot 0).
    // refresh_exports() grants peer process caps into the nameservice child, so it
    // must run after boot_init_for_service() has reserved slot 0.
    boot_init_for_service(svc, service_ordinal(&svc));
    refresh_exports();
    if (wire_and_resume_service(svc) && await_ready(svc)) return true;
    // The service came up but never reported ready (or wiring failed). Charge it
    // like a crash so a persistent start failure backs off instead of spinning;
    // the cleanup kill below is marked planned and leaves retry_at intact.
    if (svc.failures < supervision::kFailureCap) ++svc.failures;
    svc.retry_at = mx_clock_ns() + supervision::backoff_ns(svc.failures);
    svc.restart_pending = true;
    (void)stop_mask(1u << service_ordinal(&svc));
    return false;
}

static bool stop_service_now(ServiceSpec& svc) {
    if (!svc.proc) return false;
    svc.desired = false;
    svc.restart_pending = false;
    return stop_mask(dependent_mask(1u << service_ordinal(&svc)));
}

static bool restart_service_now(ServiceSpec& svc) {
    svc.desired = true;
    return stop_mask(dependent_mask(1u << service_ordinal(&svc)));
}

static void launch_prepared_services() {
    uint32_t remaining = g_rt.service_count;
    for (uint32_t pass = 0; pass < g_rt.service_count; ++pass) {
        for (uint32_t i = 0; i < g_rt.service_count; ++i) {
            auto& svc = g_rt.services[i];
            if (svc.ready || !dependencies_ready(svc)) continue;
            // The terminal opens its input gate when it receives this marker.
            if (streq(svc.name, "msh")) console_log("system ready\n");
            if (!start_service_now(svc)) fail_halt("init: service launch failed\n");
            --remaining;
        }
    }
    if (remaining) fail_halt("init: services not ready\n");
    console_log("all services ready\n");
}

static bool supervise_services() {
    bool progress = drain_exits();
    uint32_t affected = 0;
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        auto& svc = g_rt.services[i];
        if (!svc.proc && svc.restart_pending) affected |= 1u << i;
    }
    if (!affected) return progress;
    const uint32_t dependents = dependent_mask(affected) & ~affected;
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        auto& svc = g_rt.services[i];
        if ((dependents & (1u << i)) && svc.proc) {
            (void)stop_mask(dependents);
            break;
        }
    }
    // Prepare all replacements first: discovery grants refer to the new generation.
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        auto& svc = g_rt.services[i];
        if (!svc.proc && svc.desired && svc.restart_pending && mx_clock_ns() >= svc.retry_at) {
            if (!spawn_service(svc, MX_SPAWN_SUSPENDED)) {
                svc.retry_at = mx_clock_ns() + 1000000000ull;
                continue;
            }
        }
    }
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        auto& svc = g_rt.services[i];
        if (svc.proc && !svc.ready && svc.desired && svc.restart_pending && dependencies_ready(svc)) {
            if (streq(svc.name, "tty.fb0")) console_log("system ready\n");
            if (start_service_now(svc)) { console_log("service restarted\n"); progress = true; }
        }
    }
    return progress;
}

static void copy_bytes_local(void* dst, const void* src, uint32_t len) {
    (void)libk::copy_n(libk::as_bytes(dst, len), libk::as_bytes(src, len), len);
}

static void shutdown_system_now() {
    console_log("shutdown requested\n");
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        ServiceSpec& svc = g_rt.services[i];
        if (svc.proc != kNullCap) sys_kill(svc.proc);
        svc.proc = kNullCap;
        svc.main_thread = kNullCap;
        registry_sync(svc);
    }
    while (true) sys_yield();
}

static bool reboot_userland_now() {
    console_log("reboot requested\n");
    for (uint32_t i = 0; i < g_rt.service_count; ++i) g_rt.services[i].desired = true;
    return stop_mask((1u << g_rt.service_count) - 1);
}

static bool handle_svc_control_requests() {
    if (!g_rt.svcctl.valid()) return false;
    bool handled = false;
    RingSlot slot{};
    while (g_rt.svcctl.try_recv(&slot)) {
        handled = true;
        mochi::svcctl::Request req{};
        if (slot.length < sizeof(req)) {
            (void)g_rt.svcctl.send_reply(MX_ERR_BAD_PROTOCOL, 0);
            continue;
        }
        copy_bytes_local(&req, slot.payload, sizeof(req));
        // Request::name is a fixed 32-byte field with no NUL guarantee; force
        // termination before any streq() walks off the end of the struct.
        req.name[sizeof(req.name) - 1] = '\0';
        ServiceSpec* svc = nullptr;
        if (slot.type != mochi::svcctl::kRequestShutdown &&
            slot.type != mochi::svcctl::kRequestReboot) {
            svc = find_service(g_rt.services, g_rt.service_count, req.name);
            if (!svc) {
                (void)g_rt.svcctl.send_reply(MX_ERR_NOT_FOUND, 0);
                continue;
            }
        }
        mx_i32_t status = MX_ERR_INVALID;
        mx_u32_t reply_state = 0;
        bool do_shutdown = false;
        bool do_reboot = false;
        if (slot.type == mochi::svcctl::kRequestStart) {
            if (svc->proc != kNullCap || svc->main_thread != kNullCap) {
                status = MX_ERR_BUSY;
            } else {
                status = start_service_now(*svc) ? MX_OK : MX_ERR_INVALID;
            }
        } else if (slot.type == mochi::svcctl::kRequestStop) {
            status = stop_service_now(*svc) ? MX_OK : MX_ERR_DEAD;
        } else if (slot.type == mochi::svcctl::kRequestRestart) {
            status = restart_service_now(*svc) ? MX_OK : MX_ERR_INVALID;
        } else if (slot.type == mochi::svcctl::kRequestShutdown) {
            status = MX_OK;
            do_shutdown = true;
        } else if (slot.type == mochi::svcctl::kRequestReboot) {
            status = MX_OK;
            do_reboot = true;
        } else {
            status = MX_ERR_BAD_PROTOCOL;
        }
        if (svc) reply_state = registry_state_for(*svc);
        (void)g_rt.svcctl.send_reply(status, reply_state);
        if (do_shutdown) shutdown_system_now();
        if (do_reboot) (void)reboot_userland_now();
    }
    return handled;
}

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    if (const auto* entry = mochi::boot::find(boot, "cpu.count")) g_cpu_count = entry->aux0;
    if (!g_cpu_count || g_cpu_count > 4) fail_halt("init: invalid CPU count\n");
    kCapManifest = mochi::boot::require_handle(boot, "manifest");
    kCapInitrd = mochi::boot::require_handle(boot, "boot.initrd");
    kCapSupervisor = mochi::boot::require_handle(boot, "supervisor");
    kCapUart = mochi::boot::require_handle(boot, "uart.mmio");
    kCapVmRoot = mochi::boot::handle(boot, "vm.root");
    kCapLogDoorbell = mochi::boot::require_handle(boot, "log.doorbell");
    kCapFbMemory = mochi::boot::handle(boot, "fb.memory");
    kCapXhciMmio = mochi::boot::handle(boot, "xhci.mmio");
    kCapXhciIrq = mochi::boot::handle(boot, "xhci.irq");
    kCapXhciDma = mochi::boot::handle(boot, "xhci.dma");
    kCapVirtioBlkMmio = mochi::boot::handle(boot, "virtio.blk.mmio");
    kCapGpuFunction = mochi::boot::handle(boot, "gpu.function");
    kCapGpuIrq = mochi::boot::handle(boot, "gpu.irq");
    kCapVirtioBlkDma = mochi::boot::handle(boot, "virtio.blk.dma");
    g_manifest_size = mochi::boot::find(boot, "manifest")->aux0;
    g_bundle_size = mochi::boot::find(boot, "boot.initrd")->aux0;
    for (uint32_t cpu = 0; cpu < kKernelLogRingCount; ++cpu) {
        char name[] = "kernel.log.0"; name[11] = static_cast<char>('0' + cpu);
        kKernelLogRings[cpu] = mochi::boot::require_handle(boot, name);
    }

    const int64_t manifest_map = sys_map_page(kCapManifest, kVaManifest, RIGHT_READ);
    const int64_t initrd_map = sys_map_page(kCapInitrd, kVaInitrd, RIGHT_READ);
    static ServiceSpec manifest_services[kMaxManifestServices]{};
    uint32_t service_count = 0;
    if (manifest_map == 0) {
        const char* manifest_text = reinterpret_cast<const char*>(kVaManifest);
        service_count = parse_manifest_json_services(manifest_text, manifest_services);
        sha256(reinterpret_cast<const uint8_t*>(manifest_text),
                     slen(manifest_text),
                     g_policy_sha256);
        g_policy_sha256_valid = true;
    }
    if (service_count == 0) fail_halt("init: manifest failed\n");
    ServiceSpec* services = manifest_services;
    g_rt.services = services;
    g_rt.service_count = service_count;

    const auto* initrd = reinterpret_cast<const uint8_t*>(initrd_map == 0 ? kVaInitrd : 0);
    for (uint32_t i = 0; i < service_count; ++i) {
        services[i].image_data = archive_find(initrd,
                                              services[i].image,
                                              &services[i].image_size,
                                              services[i].image_sha256);
        services[i].image_sha256_valid = services[i].image_data != nullptr;
    }

    for (uint32_t i = 0; i < service_count; ++i) {
        if (!services[i].image_data || services[i].image_size == 0) {
            fail_halt("init: manifest failed\n");
        }
        if (!spawn_service(services[i], MX_SPAWN_SUSPENDED)) fail_halt("init: spawn failed\n");
    }

    const CapHandle registry = create_service_registry(services, service_count);
    g_rt.svc_registry_cap = registry;
    g_rt.svc_registry = reinterpret_cast<mochi::svc::RegistryPage*>(kVaSvcRegistry);
    g_rt.console_db = kCapLogDoorbell;
    g_rt.config_arena = as_cap(sys_arena_create(4096, 0));
    if (!g_rt.config_arena || sys_map_page(g_rt.config_arena, kVaConsoleConfig, RIGHT_READ | RIGHT_WRITE))
        fail_halt("init: console config failed\n");
    create_channels();
    create_gpu_service_channels();
    create_sessions();
    for (uint32_t i = 0; i < service_count; ++i) {
        services[i].log_ring = as_cap(sys_ring_create(kRingOrder, 0));
        if (!services[i].log_ring) fail_halt("init: log ring failed\n");
    }
    require_ok(sys_map_page(g_rt.init_log_ring, kVaInitLogRing, RIGHT_READ | RIGHT_WRITE), "init: log map failed\n");
    mochi::log_set_sink(reinterpret_cast<RingPage*>(kVaInitLogRing), kRingOrder, g_rt.console_db, "init");
    require_ok(sys_map_page(g_rt.svc_ctl_req_ring, kVaSvcCtlReqRing, RIGHT_READ | RIGHT_WRITE), "init: control map failed\n");
    require_ok(sys_map_page(g_rt.svc_ctl_reply_ring, kVaSvcCtlReplyRing, RIGHT_READ | RIGHT_WRITE), "init: control map failed\n");
    g_rt.svcctl.ep = {reinterpret_cast<RingPage*>(kVaSvcCtlReplyRing), reinterpret_cast<RingPage*>(kVaSvcCtlReqRing),
        kRingOrder, g_rt.svc_ctl_reply_db, g_rt.svc_ctl_req_db};
    parse_dependencies();
    launch_prepared_services();
    while (true) {
        bool progress = handle_svc_control_requests();
        if (supervise_services()) progress = true;
        if (!progress) sys_yield();
    }
}
