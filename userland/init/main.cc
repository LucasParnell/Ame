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
#include "mochi/svc_registry.h"
#include "mochi/svc_control.h"
#include "midix/abi/boot.h"
#include "midix/abi/service.h"
#include "midix/abi/image.h"
#include "midix/abi/protocols/nameservice.h"
#include "midix/abi/protocols/fs.h"

constexpr uint32_t PRINCIPAL_NATIVE_TRUSTED = 1;
constexpr uint32_t PRINCIPAL_NATIVE_SANDBOXED = 2;

constexpr uint32_t kRingOrder = 6;
constexpr uint32_t kFbCmdRingOrder = 10;       // 1024 slots for chatty per-cell tty rendering.
constexpr uint32_t kConsoleMirrorRingOrder = 10; // 48 KiB of UART->TTY mirror backlog.
constexpr uint64_t kVaManifest = 0x2000'0000ull;
constexpr uint64_t kVaInitrd = 0x2400'0000ull;
constexpr uint64_t kVaInitLogRing = 0x2800'0000ull;
constexpr uint64_t kVaPingToPongRing = 0x2801'0000ull;
constexpr uint64_t kVaConsoleConfig  = 0x2802'0000ull;
// kVaFs{Cmd,Reply}Ring were originally for an FS smoke test that has been
// removed; load_service_images_from_fs still needs them to talk to io.fs.
constexpr uint64_t kVaFsCmdRing = 0x2803'0000ull;
constexpr uint64_t kVaFsReplyRing = 0x2804'0000ull;
constexpr uint64_t kVaSvcCtlReqRing = 0x2807'0000ull;
constexpr uint64_t kVaSvcCtlReplyRing = 0x2808'0000ull;
constexpr uint64_t kVaBootInfoBase   = 0x2900'0000ull;
constexpr uint64_t kVaSvcRegistry    = 0x2A00'0000ull;
constexpr uint64_t kVaFsImageBase    = 0x3000'0000ull;
constexpr uint64_t kFsImageStride    = 0x0004'0000ull; // 256 KiB, kernel arena max.

static CapHandle kCapManifest, kCapInitrd, kCapSupervisor, kCapNameservice, kCapUart;
static CapHandle kCapVmRoot, kCapLogDoorbell, kCapFbMemory, kCapXhciMmio, kCapXhciIrq, kCapXhciDma;
static CapHandle kCapVirtioBlkMmio, kCapVirtioBlkDma;
constexpr uint32_t kKernelLogRingCount = 4;
static CapHandle kKernelLogRings[kKernelLogRingCount];
static uint64_t g_manifest_size = 0;
static uint64_t g_bundle_size = 0;
static uint32_t g_cpu_count = 1;
static int64_t set_service_affinity(CapHandle thread, uint64_t preferred) {
    const uint64_t online = (1ull << g_cpu_count) - 1;
    return sys_thread_set_affinity(thread, (preferred & online) ? (preferred & online) : 1);
}

constexpr uint32_t kMaxManifestServices = 16;
constexpr uint32_t kManifestScanLimit = 64 * 1024;
constexpr bool kEmitBootMetrics = false;

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
    uint32_t n = 0;
    while (s[n]) ++n;
    return n;
}

static bool streq(const char* a, const char* b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
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
        if (!json::string(json::member(obj, "name"), svc.name, sizeof(svc.name)) ||
            !json::string(json::member(obj, "object"), svc.image, sizeof(svc.image)) ||
            !json::string(json::member(obj, "principal"), svc.principal, sizeof(svc.principal)) ||
            !json::string(json::member(obj, "restart"), svc.restart, sizeof(svc.restart))) return 0;
        if ((!streq(svc.principal, "native_trusted") && !streq(svc.principal, "native_sandboxed")) ||
            (!streq(svc.restart, "always") && !streq(svc.restart, "never"))) return 0;
        for (uint32_t i = 0; i < count; ++i) if (streq(out[i].name, svc.name)) return 0;
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

static void console_log_counter(const char* key, uint64_t value) {
    if constexpr (!kEmitBootMetrics) {
        (void)key;
        (void)value;
        return;
    }
    mochi::log_counter(key, value);
}

static const uint8_t* archive_find(const uint8_t* base, const char* wanted,
                                    uint64_t* size, uint8_t hash[MX_SHA256_SIZE]) {
    if (!base || g_bundle_size < sizeof(MxBundleHeader)) return nullptr;
    midix::format::Bundle bundle{};
    bundle.data = base;
    bundle.size = g_bundle_size;
    const auto* src = reinterpret_cast<const uint8_t*>(base);
    for (uint32_t i = 0; i < sizeof(bundle.header); ++i) reinterpret_cast<uint8_t*>(&bundle.header)[i] = src[i];
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

static bool service_started(const ServiceSpec* svc) {
    return svc && svc->main_thread != kNullCap;
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

static bool image_filename(const ServiceSpec& svc, char out[36]) {
    uint32_t n = 0;
    while (n < sizeof(svc.image) && svc.image[n]) {
        if (n + 5 >= 36) return false;
        out[n] = svc.image[n];
        ++n;
    }
    const char suffix[] = ".mxi";
    for (uint32_t i = 0; i < 4; ++i) out[n + i] = suffix[i];
    out[n + 4] = '\0';
    return true;
}

static bool open_child(const mochi::fs::Client& fs, fs_handle_t parent,
                       const char* name, FsOpenReply* out) {
    return fs.wait_open(parent, name, FS_OPEN_READ, out) &&
           out->status == MX_OK &&
           out->handle != 0;
}

// Walks /ame/userland/bin once. The result is reused for every image lookup so
// we don't repeat the 3 directory opens per service.
static bool open_userland_bin(const mochi::fs::Client& fs, FsOpenReply* bin) {
    FsOpenReply ame{};
    FsOpenReply userland{};
    if (!open_child(fs, 0, "ame", &ame)) return false;
    if (!open_child(fs, ame.handle, "userland", &userland)) return false;
    return open_child(fs, userland.handle, "bin", bin);
}

static bool open_userland_image(const mochi::fs::Client& fs,
                                fs_handle_t bin_handle,
                                const ServiceSpec& svc,
                                FsOpenReply* out) {
    char image_name[36]{};
    if (!image_filename(svc, image_name)) return false;
    return open_child(fs, bin_handle, image_name, out);
}

static bool load_service_image_from_fs(const mochi::fs::Client& fs,
                                       fs_handle_t bin_handle,
                                       ServiceSpec& svc,
                                       uint32_t ordinal) {
    // The trusted SHA from the initrd bundle must already be in place. Without
    // it we have nothing to compare against, so the FS image cannot be
    // promoted to trusted status.
    if (!svc.image_sha256_valid) return false;

    FsOpenReply open{};
    if (!open_userland_image(fs, bin_handle, svc, &open)) return false;
    if (open.type != FS_INODE_FILE ||
        open.size == 0 ||
        open.size > kFsImageStride) {
        return false;
    }
    uint64_t arena_size = (open.size + 4095ull) & ~4095ull;
    if (arena_size == 0 || arena_size > kFsImageStride) return false;
    const CapHandle arena = as_cap(sys_arena_create(arena_size, 0));
    if (arena == kNullCap) return false;
    const uint64_t va = kVaFsImageBase + (static_cast<uint64_t>(ordinal) * kFsImageStride);
    if (sys_map_page(arena, va, RIGHT_READ | RIGHT_WRITE) != 0) {
        sys_cap_revoke(arena);
        return false;
    }
    auto* dst = reinterpret_cast<uint8_t*>(va);

    uint64_t off = 0;
    while (off < open.size) {
        const uint32_t want =
            static_cast<uint32_t>((open.size - off) > FS_INLINE_READ_MAX
                                      ? FS_INLINE_READ_MAX
                                      : (open.size - off));
        FsReadReply read{};
        if (!fs.try_read(open.handle, off, want, &read) ||
            read.status != MX_OK ||
            read.length_read != want) {
            sys_cap_revoke(arena);
            return false;
        }
        // Word-copy the inline payload; FS_INLINE_READ_MAX is small but this
        // still runs once per chunk for every image we pull.
        const uint8_t* src = read.inline_data;
        uint8_t* d = dst + off;
        uint32_t i = 0;
        while (i + 8 <= want) {
            uint64_t w;
            __builtin_memcpy(&w, src + i, 8);
            __builtin_memcpy(d + i, &w, 8);
            i += 8;
        }
        while (i < want) {
            d[i] = src[i];
            ++i;
        }
        off += want;
    }

    // Verify the FS bytes match the trusted SHA from the boot bundle before we
    // expose this image as a respawn source. The disk is untrusted at rest;
    // promoting an unverified image would let a tampered AmeFS replace msh
    // (or any service we extend this path to).
    uint8_t digest[MX_SHA256_SIZE]{};
    sha256(dst, open.size, digest);
    if (!bytes_equal(digest, svc.image_sha256, MX_SHA256_SIZE)) {
        console_log("init: AmeFS image sha mismatch; keeping initrd image\n");
        sys_cap_revoke(arena);
        return false;
    }

    svc.image_data = reinterpret_cast<const uint8_t*>(va);
    svc.image_size = open.size;
    svc.image_arena = arena;
    // svc.image_sha256 already holds the trusted SHA and matches `digest`;
    // leave it (and image_sha256_valid) untouched.
    return true;
}

// Only msh is consumed after this point — respawn_suspended_from_loaded_image
// is called on the shell and nothing else. Loading the other 14 service images
// from AmeFS was pure round-trip overhead (~3.8s at boot) because each
// FS_READ_REPLY ships at most ~40 bytes of inline payload.
//
// Maps the FS cmd/reply rings on first call; the smoke tests used to own this
// mapping but they have been removed from the boot path.
static uint32_t load_service_images_from_fs(CapHandle fs_cmd_ring,
                                            CapHandle fs_reply_ring,
                                            CapHandle fs_cmd_db,
                                            CapHandle fs_reply_db) {
    if (sys_map_page(fs_cmd_ring, kVaFsCmdRing, RIGHT_READ | RIGHT_WRITE) != 0) {
        return 0;
    }
    if (sys_map_page(fs_reply_ring, kVaFsReplyRing, RIGHT_READ | RIGHT_WRITE) != 0) {
        return 0;
    }
    const mochi::fs::Client fs{
        {reinterpret_cast<RingPage*>(kVaFsCmdRing),
         reinterpret_cast<RingPage*>(kVaFsReplyRing),
         kRingOrder,
         fs_cmd_db,
         fs_reply_db},
    };
    if (!fs.valid()) return 0;
    ServiceSpec* shell = find_service(g_rt.services, g_rt.service_count, "msh");
    if (!shell) return 0;
    FsOpenReply bin{};
    if (!open_userland_bin(fs, &bin)) return 0;
    if (!load_service_image_from_fs(fs, bin.handle, *shell, service_ordinal(shell))) {
        return 0;
    }
    return 1;
}


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
    if (svc.proc != kNullCap && svc.main_thread != kNullCap) state |= mochi::svc::kStateRunning;
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

static bool respawn_suspended_from_loaded_image(ServiceSpec& svc) {
    if (svc.image_arena == kNullCap ||
        svc.proc == kNullCap ||
        svc.main_thread == kNullCap) {
        return false;
    }
    // Spawn the replacement first; only kill the live process once we know
    // the new one is up. The previous order (kill → spawn → return false on
    // failure) left the system with a dead msh and no fallback if spawn ever
    // failed, even though the FS image has already been verified by then.
    const CapHandle old_proc = svc.proc;
    const CapHandle old_thread = svc.main_thread;
    svc.proc = kNullCap;
    svc.main_thread = kNullCap;
    if (!spawn_service(svc, MX_SPAWN_SUSPENDED)) {
        svc.proc = old_proc;
        svc.main_thread = old_thread;
        return false;
    }
    sys_kill(old_proc);
    boot_init_for_service(svc, service_ordinal(&svc));
    registry_sync(svc);
    return true;
}

static void console_config_reset() {
    auto* cfg = reinterpret_cast<ConsoleBootConfig*>(kVaConsoleConfig);
    for (uint32_t i = 0; i < sizeof(ConsoleBootConfig); ++i) {
        reinterpret_cast<uint8_t*>(cfg)[i] = 0;
    }
}

static bool wire_and_resume_service(ServiceSpec& svc) {
    if (streq(svc.name, "nameservice")) {
        ServiceSpec* ping = find_service(g_rt.services, g_rt.service_count, "demo.ping");
        ServiceSpec* pong = find_service(g_rt.services, g_rt.service_count, "demo.pong");
        ServiceSpec* dango = find_service(g_rt.services, g_rt.service_count, "dango.manager");
        ServiceSpec* fs = find_service(g_rt.services, g_rt.service_count, "io.fs");
        if (!ping || !pong || ping->proc == kNullCap || pong->proc == kNullCap) return false;
        share_ring_to(svc, g_rt.ns_request_ring, "ns.requests");
        share_ring_to(svc, g_rt.ns_reply_ring, "ns.replies");
        share_doorbell_to(svc, g_rt.db_nameservice, "ns.server.db");
        share_doorbell_to(svc, g_rt.db_ns_reply, "ns.client.db");
        grant_to(svc, ping->proc, RIGHT_SPAWN, MX_BOOT_ENTRY_HANDLE, "client.process");
        grant_to(svc, pong->proc, RIGHT_READ | RIGHT_DUP, MX_BOOT_ENTRY_ENDPOINT,
                 "service.demo.pong", 0, RIGHT_READ, MX_ENDPOINT_FLAG_CLIENT_ALLOCATED);
        if (dango && dango->proc != kNullCap) {
            grant_to(svc, dango->proc, RIGHT_READ | RIGHT_DUP, MX_BOOT_ENTRY_ENDPOINT,
                     "service.dango.manager", 0, RIGHT_READ, MX_ENDPOINT_FLAG_CLIENT_ALLOCATED);
        }
        if (fs && fs->proc != kNullCap) {
            grant_to(svc, fs->proc, RIGHT_READ | RIGHT_DUP, MX_BOOT_ENTRY_ENDPOINT,
                     "service.io.fs", MX_PROTOCOL_FS, RIGHT_READ, MX_ENDPOINT_FLAG_CLIENT_ALLOCATED);
        }
        share_ring_to(svc, svc.log_ring, "log.ring");
        share_doorbell_to(svc, g_rt.console_db, "log.doorbell");
        boot_publish(svc);
        require_ok(set_service_affinity(svc.main_thread, 1ull << 0), "init: service start failed\n");
        require_ok(sys_thread_resume(svc.main_thread), "init: service start failed\n");
        registry_sync(svc);
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
        share_ring_to(svc, g_rt.ns_request_ring, "ns.tx");
        share_ring_to(svc, g_rt.ns_reply_ring, "ns.rx");
        share_doorbell_to(svc, g_rt.db_nameservice, "ns.server.db", RIGHT_NOTIFY | RIGHT_DUP);
        share_doorbell_to(svc, g_rt.db_ns_reply, "ns.reply.db", RIGHT_WAIT | RIGHT_DUP);
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
        const uint64_t layout_base = sys_doorbell_read(g_rt.db_dango_layout_done);
        boot_publish(svc);
        require_ok(set_service_affinity(svc.main_thread, 1ull << 1), "init: service start failed\n");
        require_ok(sys_thread_resume(svc.main_thread), "init: service start failed\n");
        if (sys_wait_doorbell(g_rt.db_dango_layout_done, layout_base) != 0) return false;
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
        share_ring_to_with_rights(svc, g_rt.fs_cmd_ring, "fs.cmd.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_ring_to_with_rights(svc, g_rt.fs_reply_ring, "fs.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
        share_doorbell_to(svc, g_rt.fs_cmd_db, "fs.cmd.doorbell", RIGHT_WAIT);
        share_doorbell_to(svc, g_rt.fs_reply_db, "fs.reply.doorbell", RIGHT_NOTIFY);
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
        ServiceSpec* fs = find_service(g_rt.services, g_rt.service_count, "io.fs");
        if (fs && fs->proc != kNullCap) {
            share_ring_to_with_rights(svc, g_rt.fs_cmd_ring, "fs.cmd.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_ring_to_with_rights(svc, g_rt.fs_reply_ring, "fs.reply.ring", RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(svc, g_rt.fs_cmd_db, "fs.cmd.doorbell", RIGHT_NOTIFY);
            share_doorbell_to(svc, g_rt.fs_reply_db, "fs.reply.doorbell", RIGHT_WAIT);
        }
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
    return false;
}

static bool start_service_now(ServiceSpec& svc) {
    if (svc.proc != kNullCap || svc.main_thread != kNullCap) return false;
    if (!spawn_service(svc, MX_SPAWN_SUSPENDED)) return false;
    boot_init_for_service(svc, service_ordinal(&svc));
    if (!wire_and_resume_service(svc)) {
        if (svc.proc != kNullCap) sys_kill(svc.proc);
        svc.proc = kNullCap;
        svc.main_thread = kNullCap;
        registry_sync(svc);
        return false;
    }
    return true;
}

static bool stop_service_now(ServiceSpec& svc) {
    if (svc.proc == kNullCap || svc.main_thread == kNullCap) return false;
    sys_kill(svc.proc);
    svc.proc = kNullCap;
    svc.main_thread = kNullCap;
    registry_sync(svc);
    return true;
}

static bool restart_service_now(ServiceSpec& svc) {
    const bool was_running = (svc.proc != kNullCap && svc.main_thread != kNullCap);
    if (was_running) {
        (void)stop_service_now(svc);
        for (uint32_t i = 0; i < 1024; ++i) sys_yield();
    }
    return start_service_now(svc);
}

static void copy_bytes_local(void* dst, const void* src, uint32_t len) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* p = static_cast<const uint8_t*>(src);
    for (uint32_t i = 0; i < len; ++i) d[i] = p[i];
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
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        ServiceSpec& svc = g_rt.services[i];
        if (svc.proc != kNullCap) sys_kill(svc.proc);
        svc.proc = kNullCap;
        svc.main_thread = kNullCap;
        registry_sync(svc);
    }
    for (uint32_t i = 0; i < 4096; ++i) sys_yield();
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        ServiceSpec& svc = g_rt.services[i];
        if (!spawn_service(svc, MX_SPAWN_SUSPENDED)) return false;
    }
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        boot_init_for_service(g_rt.services[i], i);
        registry_sync(g_rt.services[i]);
    }
    for (uint32_t i = 0; i < g_rt.service_count; ++i) {
        if (!wire_and_resume_service(g_rt.services[i])) return false;
    }
    console_log("userland reboot complete\n");
    return true;
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
    kCapNameservice = mochi::boot::handle(boot, "nameservice");
    kCapUart = mochi::boot::require_handle(boot, "uart.mmio");
    kCapVmRoot = mochi::boot::handle(boot, "vm.root");
    kCapLogDoorbell = mochi::boot::require_handle(boot, "log.doorbell");
    kCapFbMemory = mochi::boot::handle(boot, "fb.memory");
    kCapXhciMmio = mochi::boot::handle(boot, "xhci.mmio");
    kCapXhciIrq = mochi::boot::handle(boot, "xhci.irq");
    kCapXhciDma = mochi::boot::handle(boot, "xhci.dma");
    kCapVirtioBlkMmio = mochi::boot::handle(boot, "virtio.blk.mmio");
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

    ServiceSpec* nameservice = find_service(services, service_count, "nameservice");
    ServiceSpec* console = find_service(services, service_count, "io.console");
    ServiceSpec* pong = find_service(services, service_count, "demo.pong");
    ServiceSpec* ping = find_service(services, service_count, "demo.ping");
    ServiceSpec* dango = find_service(services, service_count, "dango.manager");
    ServiceSpec* auth_probe = find_service(services, service_count, "dango.auth_probe");
    ServiceSpec* fs_service = find_service(services, service_count, "io.fs");
    ServiceSpec* shell_service = find_service(services, service_count, "msh");
    const CapHandle nameservice_proc = nameservice ? nameservice->proc : kNullCap;
    const CapHandle console_proc = console ? console->proc : kNullCap;
    CapHandle pong_proc = pong ? pong->proc : kNullCap;
    const CapHandle ping_proc = ping ? ping->proc : kNullCap;
    const CapHandle dango_proc = dango ? dango->proc : kNullCap;
    const CapHandle auth_probe_proc = auth_probe ? auth_probe->proc : kNullCap;
    const CapHandle fs_proc = fs_service ? fs_service->proc : kNullCap;
    const CapHandle shell_proc = shell_service ? shell_service->proc : kNullCap;

    // Every service receives a generic BootInfo page as cap slot 0 before any
    // other capability. This removes fixed cap layouts from service code while
    // preserving the simple userland-only startup policy.
    for (uint32_t i = 0; i < service_count; ++i) {
        if (services[i].proc == kNullCap) fail_halt("init: bootinfo failed\n");
        boot_init_for_service(services[i], i);
    }

    const CapHandle svc_registry = create_service_registry(services, service_count);
    g_rt.svc_registry_cap = svc_registry;
    g_rt.svc_registry = reinterpret_cast<mochi::svc::RegistryPage*>(kVaSvcRegistry);

    // Console startup caps: BootInfo slot 0 + config page + UART + shared log doorbell + one ring per producer.
    const CapHandle console_db =
        (sys_cap_identify(kCapLogDoorbell) == kCapTypeDoorbell)
            ? kCapLogDoorbell
            : as_cap(sys_doorbell_create());
    const CapHandle init_log_ring = as_cap(sys_ring_create(kRingOrder, 0));
    g_rt.console_db = console_db;
    g_rt.init_log_ring = init_log_ring;
    if (console_db == kNullCap || init_log_ring == kNullCap || console_proc == kNullCap) {
        fail_halt("init: wiring preconsole caps failed\n");
    }
    for (uint32_t i = 0; i < service_count; ++i) {
        services[i].log_ring = as_cap(sys_ring_create(kRingOrder, 0));
        if (services[i].log_ring == kNullCap) fail_halt("init: wiring log ring create failed\n");
    }
    if (sys_map_page(init_log_ring, kVaInitLogRing, RIGHT_READ | RIGHT_WRITE) != 0) {
        fail_halt("init: wiring init log map failed\n");
    }
    mochi::log_set_sink(reinterpret_cast<RingPage*>(kVaInitLogRing),
                        kRingOrder,
                        console_db,
                        "init");

    // Best-effort UART log mirror into tty.fb0. io.console is the sole
    // producer; tty.fb0 is the sole consumer. UART remains authoritative.
    const CapHandle console_mirror_ring = as_cap(sys_ring_create(kConsoleMirrorRingOrder, 0));
    const CapHandle console_mirror_db = as_cap(sys_doorbell_create());
    g_rt.console_mirror_ring = console_mirror_ring;
    g_rt.console_mirror_db = console_mirror_db;
    if (console_mirror_ring == kNullCap || console_mirror_db == kNullCap) {
        fail_halt("init: wiring console mirror failed\n");
    }

    // Allocate config page shared with io.console.
    const CapHandle config_arena = as_cap(sys_arena_create(4096, 0));
    g_rt.config_arena = config_arena;
    if (config_arena == kNullCap) fail_halt("init: wiring console config arena create failed\n");
    if (sys_map_page(config_arena, kVaConsoleConfig, RIGHT_READ | RIGHT_WRITE) != 0) {
        fail_halt("init: wiring console config map failed\n");
    }
    auto* cfg = reinterpret_cast<ConsoleBootConfig*>(kVaConsoleConfig);
    cfg->magic = 0;  // written last as ready flag

    // Grant caps to io.console and publish their child-side handles in BootInfo.
    (void)grant_to(*console, config_arena,
                   RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                   MX_BOOT_ENTRY_ARENA, "console.config");
    const CapHandle cons_uart = grant_to(*console, kCapUart,
                                         RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                                         MX_BOOT_ENTRY_MMIO, "uart.mmio");
    const CapHandle cons_doorbell =
        share_doorbell_to(*console, console_db, "log.doorbell", RIGHT_WAIT);
    share_ring_to_with_rights(*console, console_mirror_ring, "tty.mirror.ring",
                              RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                              kConsoleMirrorRingOrder);
    share_doorbell_to(*console, console_mirror_db, "tty.mirror.doorbell",
                      RIGHT_NOTIFY);

    uint32_t ring_idx = 0;
    for (uint32_t cpu = 0; cpu < kKernelLogRingCount; ++cpu) {
        const CapHandle h = as_cap(
            sys_ring_share(kKernelLogRings[cpu], console_proc,
                           RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
        if (h == kNullCap || ring_idx >= ConsoleBootConfig::kMaxRings)
            fail_halt("init: wiring kernel log ring share failed\n");
        cfg->log_ring_caps[ring_idx++] = h;
    }
    {
        const CapHandle h = as_cap(
            sys_ring_share(init_log_ring, console_proc,
                           RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
        if (h == kNullCap || ring_idx >= ConsoleBootConfig::kMaxRings)
            fail_halt("init: wiring init log ring share failed\n");
        cfg->log_ring_caps[ring_idx++] = h;
    }
    for (uint32_t i = 0; i < service_count; ++i) {
        const CapHandle h = as_cap(
            sys_ring_share(services[i].log_ring, console_proc,
                           RIGHT_READ | RIGHT_WRITE | RIGHT_MAP));
        if (h == kNullCap || ring_idx >= ConsoleBootConfig::kMaxRings)
            fail_halt("init: wiring service log ring share failed\n");
        cfg->log_ring_caps[ring_idx++] = h;
    }

    cfg->uart_cap         = cons_uart;
    cfg->log_doorbell_cap = cons_doorbell;
    cfg->term_input_ring_cap = 0;
    cfg->term_output_ring_cap = 0;
    cfg->term_input_doorbell_cap = 0;
    cfg->term_output_doorbell_cap = 0;
    cfg->total_ring_count = ring_idx;
    cfg->version          = ConsoleBootConfig::kVersion;
    cfg->flags            = 0;
    cfg->reserved0        = 0;
    __atomic_store_n(&cfg->magic, ConsoleBootConfig::kMagic, __ATOMIC_RELEASE);
    boot_publish(*console);

    if (!console || console->main_thread == kNullCap) fail_halt("init: wiring console missing failed\n");
    require_ok(set_service_affinity(console->main_thread, 1ull << 0), "init: wiring console affinity failed\n");
    require_ok(sys_thread_resume(console->main_thread), "init: wiring console resume failed\n");
    console_log("console ready\n");

    // IPC demo wiring.  Switch first-boot manifests intentionally omit the demo
    // pair, so nameservice must be able to start without these test endpoints.
    const bool have_ipc_demo =
        nameservice && pong && ping &&
        nameservice_proc != kNullCap &&
        pong_proc != kNullCap &&
        ping_proc != kNullCap;
    const CapHandle ring_ping_to_pong =
        have_ipc_demo ? as_cap(sys_ring_create(kRingOrder, 0)) : kNullCap;
    const CapHandle ring_pong_to_ping =
        have_ipc_demo ? as_cap(sys_ring_create(kRingOrder, 0)) : kNullCap;
    const CapHandle db_pong = have_ipc_demo ? as_cap(sys_doorbell_create()) : kNullCap;
    const CapHandle db_ping = have_ipc_demo ? as_cap(sys_doorbell_create()) : kNullCap;
    const CapHandle db_ipc_done = have_ipc_demo ? as_cap(sys_doorbell_create()) : kNullCap;
    const CapHandle ns_request_ring = as_cap(sys_ring_create(kRingOrder, 0));
    const CapHandle ns_reply_ring = as_cap(sys_ring_create(kRingOrder, 0));
    const CapHandle db_nameservice = as_cap(sys_doorbell_create());
    const CapHandle db_ns_reply = as_cap(sys_doorbell_create());
    g_rt.ring_ping_to_pong = ring_ping_to_pong;
    g_rt.ring_pong_to_ping = ring_pong_to_ping;
    g_rt.db_pong = db_pong;
    g_rt.db_ping = db_ping;
    g_rt.db_ipc_done = db_ipc_done;
    g_rt.ns_request_ring = ns_request_ring;
    g_rt.ns_reply_ring = ns_reply_ring;
    g_rt.db_nameservice = db_nameservice;
    g_rt.db_ns_reply = db_ns_reply;
    if (have_ipc_demo &&
        sys_map_page(ring_ping_to_pong, kVaPingToPongRing, RIGHT_READ | RIGHT_WRITE) != 0) {
        fail_halt("init: wiring ping-pong map failed\n");
    }
    RingPage* ping_to_pong_ring =
        have_ipc_demo ? reinterpret_cast<RingPage*>(kVaPingToPongRing) : nullptr;
    if (ns_request_ring == kNullCap || ns_reply_ring == kNullCap ||
        db_nameservice == kNullCap || db_ns_reply == kNullCap ||
        nameservice_proc == kNullCap) {
        fail_halt("init: wiring nameservice primitive create failed\n");
    }
    if (have_ipc_demo &&
        (ring_ping_to_pong == kNullCap || ring_pong_to_ping == kNullCap ||
         db_pong == kNullCap || db_ping == kNullCap || db_ipc_done == kNullCap ||
         !ping_to_pong_ring)) {
        fail_halt("init: wiring ipc primitive create failed\n");
    }

    if (!nameservice || nameservice->main_thread == kNullCap) {
        fail_halt("init: wiring required service missing failed\n");
    }
    if (have_ipc_demo &&
        (pong->main_thread == kNullCap || ping->main_thread == kNullCap)) {
        fail_halt("init: wiring required service missing failed\n");
    }

    // Nameservice endpoint and service registry. Nameservice learns about its
    // request/reply rings, the current v0 client process, and boot-time services
    // through BootInfo instead of fixed cap slots.
    share_ring_to(*nameservice, ns_request_ring, "ns.requests");
    share_ring_to(*nameservice, ns_reply_ring, "ns.replies");
    share_doorbell_to(*nameservice, db_nameservice, "ns.server.db");
    share_doorbell_to(*nameservice, db_ns_reply, "ns.client.db");
    if (have_ipc_demo) {
        grant_to(*nameservice, ping_proc, RIGHT_SPAWN,
                 MX_BOOT_ENTRY_HANDLE, "client.process");
        grant_to(*nameservice, pong_proc, RIGHT_READ | RIGHT_DUP,
                 MX_BOOT_ENTRY_ENDPOINT, "service.demo.pong",
                 0, RIGHT_READ, MX_ENDPOINT_FLAG_CLIENT_ALLOCATED);
    }
    if (dango_proc != kNullCap) {
        grant_to(*nameservice, dango_proc, RIGHT_READ | RIGHT_DUP,
                 MX_BOOT_ENTRY_ENDPOINT, "service.dango.manager",
                 0, RIGHT_READ, MX_ENDPOINT_FLAG_CLIENT_ALLOCATED);
    }
    if (fs_proc != kNullCap) {
        grant_to(*nameservice, fs_proc, RIGHT_READ | RIGHT_DUP,
                 MX_BOOT_ENTRY_ENDPOINT, "service.io.fs",
                 MX_PROTOCOL_FS, RIGHT_READ, MX_ENDPOINT_FLAG_CLIENT_ALLOCATED);
    }
    share_ring_to(*nameservice, nameservice->log_ring, "log.ring");
    share_doorbell_to(*nameservice, console_db, "log.doorbell");
    boot_publish(*nameservice);
    require_ok(set_service_affinity(nameservice->main_thread, 1ull << 0),
               "init: wiring nameservice affinity failed\n");
    require_ok(sys_thread_resume(nameservice->main_thread), "init: wiring nameservice resume failed\n");

    if (have_ipc_demo) {
        // Pong endpoint from Pong's perspective.
        share_ring_to(*pong, ring_ping_to_pong, "ipc.rx");
        share_ring_to(*pong, ring_pong_to_ping, "ipc.tx");
        share_doorbell_to(*pong, db_pong, "ipc.self.db");
        share_doorbell_to(*pong, db_ping, "ipc.peer.db");
        share_ring_to(*pong, pong->log_ring, "log.ring");
        share_doorbell_to(*pong, console_db, "log.doorbell");
        boot_publish(*pong);

        // Ping endpoint from Ping's perspective, plus a nameservice client endpoint.
        share_ring_to(*ping, ring_ping_to_pong, "ipc.tx");
        share_ring_to(*ping, ring_pong_to_ping, "ipc.rx");
        share_doorbell_to(*ping, db_pong, "ipc.peer.db");
        share_doorbell_to(*ping, db_ping, "ipc.self.db");
        share_ring_to(*ping, ping->log_ring, "log.ring");
        share_doorbell_to(*ping, console_db, "log.doorbell");
        share_doorbell_to(*ping, db_ipc_done, "ipc.done", RIGHT_NOTIFY | RIGHT_DUP);
        share_ring_to(*ping, ns_request_ring, "ns.tx");
        share_ring_to(*ping, ns_reply_ring, "ns.rx");
        share_doorbell_to(*ping, db_nameservice, "ns.server.db", RIGHT_NOTIFY | RIGHT_DUP);
        share_doorbell_to(*ping, db_ns_reply, "ns.reply.db", RIGHT_WAIT | RIGHT_DUP);
        boot_publish(*ping);

        require_ok(set_service_affinity(pong->main_thread, 1ull << 1),
                   "init: wiring pong affinity failed\n");
        require_ok(sys_thread_resume(pong->main_thread), "init: wiring pong resume failed\n");
        require_ok(set_service_affinity(ping->main_thread, 1ull << 0),
                   "init: wiring ping affinity failed\n");
        require_ok(sys_thread_resume(ping->main_thread), "init: wiring ping resume failed\n");

        const uint64_t ipc_done_base = sys_doorbell_read(db_ipc_done);
        const int64_t ipc_wait_rc = sys_wait_doorbell(db_ipc_done, ipc_done_base);
        if (ipc_wait_rc < 0) fail_halt("ipc: hot ring failed\n");

        if (!dango) {
            // Supervision demo: kill pong through the process/supervisor path,
            // then restart it according to the manifest policy.
            if (pong_proc == kNullCap) {
                fail_halt("init: supervision failed\n");
            }
            sys_kill(pong_proc);

            bool saw_death = false;
            for (int i = 0; i < 4096; ++i) {
                SysPair event = sys_supervisor_poll(kCapSupervisor);
                if (event.value > 0) {
                    saw_death = true;
                    break;
                }
                sys_yield();
            }
            if (!saw_death || !pong || !token_eq(pong->restart, "always")) {
                fail_halt("init: supervision failed\n");
            }
            if (!spawn_service(*pong, MX_SPAWN_SUSPENDED)) fail_halt("init: supervision failed\n");
            pong_proc = pong->proc;
            if (pong_proc != kNullCap) {
                boot_init_for_service(*pong, service_count + 16);
                share_ring_to(*pong, ring_ping_to_pong, "ipc.rx");
                share_ring_to(*pong, ring_pong_to_ping, "ipc.tx");
                share_doorbell_to(*pong, db_pong, "ipc.self.db");
                share_doorbell_to(*pong, db_ping, "ipc.peer.db");
                share_ring_to(*pong, pong->log_ring, "log.ring");
                share_doorbell_to(*pong, console_db, "log.doorbell");
                boot_publish(*pong);
                require_ok(set_service_affinity(pong->main_thread, 1ull << 1),
                           "init: supervision failed\n");
                require_ok(sys_thread_resume(pong->main_thread), "init: supervision failed\n");
            } else {
                fail_halt("init: supervision failed\n");
            }
        }
        console_log("supervision ready\n");

        const uint64_t doorbell_sleeps = static_cast<uint64_t>(sys_debug_counter(5));
        const uint64_t doorbell_wakes = static_cast<uint64_t>(sys_debug_counter(6));
        if (doorbell_sleeps == 0 || doorbell_wakes == 0) {
            fail_halt("ipc: doorbell sleep failed\n");
        }

        if (static_cast<uint64_t>(sys_debug_counter(17)) >= 2) console_log("local scheduling ready\n");
        if (static_cast<uint64_t>(sys_debug_counter(23)) > 0 &&
            static_cast<uint64_t>(sys_debug_counter(24)) > 0 &&
            static_cast<uint64_t>(sys_debug_counter(25)) > 0) {
            console_log("remote wake ready\n");
        }
        if (static_cast<uint64_t>(sys_debug_counter(28)) > 0) {
            console_log("cross-core doorbell ready\n");
        }
        console_log("fairness check complete\n");
        console_log("base services ready\n");
        console_log("scheduler tests ready\n");
    } else {
        console_log("ipc demo skipped\n");
        console_log("base services ready\n");
    }
    // sys_debug_counter is a real syscall (ring-0 round trip) per slot. When
    // kEmitBootMetrics is off, console_log_counter is a no-op, but the argument
    // expressions still run. Gate the whole block so we don't pay for ~32
    // syscalls' worth of latency on every boot just to throw the numbers away.
    if constexpr (kEmitBootMetrics) {
        console_log_counter("metric.syscalls", static_cast<uint64_t>(sys_debug_counter(0)));
        console_log_counter("metric.ipc.syscalls", static_cast<uint64_t>(sys_debug_counter(1)));
        console_log_counter("metric.context.switches", static_cast<uint64_t>(sys_debug_counter(2)));
        console_log_counter("metric.debug.puts", static_cast<uint64_t>(sys_debug_counter(3)));
        console_log_counter("metric.el0.faults", static_cast<uint64_t>(sys_debug_counter(4)));
        console_log_counter("metric.doorbell.sleeps", static_cast<uint64_t>(sys_debug_counter(5)));
        console_log_counter("metric.doorbell.wakes", static_cast<uint64_t>(sys_debug_counter(6)));
        console_log_counter("metric.timer.ticks", static_cast<uint64_t>(sys_debug_counter(7)));
        console_log_counter("metric.doorbell.wait.calls", static_cast<uint64_t>(sys_debug_counter(8)));
        console_log_counter("metric.doorbell.wait.immediate", static_cast<uint64_t>(sys_debug_counter(9)));
        console_log_counter("metric.notify.calls", static_cast<uint64_t>(sys_debug_counter(10)));
        console_log_counter("metric.notify.no.waiter", static_cast<uint64_t>(sys_debug_counter(11)));
        console_log_counter("metric.notify.woke.thread", static_cast<uint64_t>(sys_debug_counter(12)));
        console_log_counter("metric.yields", static_cast<uint64_t>(sys_debug_counter(13)));
        console_log_counter("metric.block.switches", static_cast<uint64_t>(sys_debug_counter(14)));
        console_log_counter("metric.timer.preempts", static_cast<uint64_t>(sys_debug_counter(15)));
        console_log_counter("metric.exit.switches", static_cast<uint64_t>(sys_debug_counter(16)));
        console_log_counter("metric.cpus.online", static_cast<uint64_t>(sys_debug_counter(17)));
        console_log_counter("metric.cpu0.context.switches", static_cast<uint64_t>(sys_debug_counter(18)));
        console_log_counter("metric.cpu1.context.switches", static_cast<uint64_t>(sys_debug_counter(19)));
        console_log_counter("metric.cpu0.timer.preempts", static_cast<uint64_t>(sys_debug_counter(20)));
        console_log_counter("metric.cpu1.timer.preempts", static_cast<uint64_t>(sys_debug_counter(21)));
        console_log_counter("metric.local.wakes", static_cast<uint64_t>(sys_debug_counter(22)));
        console_log_counter("metric.remote.wakes", static_cast<uint64_t>(sys_debug_counter(23)));
        console_log_counter("metric.ipi.sent", static_cast<uint64_t>(sys_debug_counter(24)));
        console_log_counter("metric.ipi.handled", static_cast<uint64_t>(sys_debug_counter(25)));
        console_log_counter("metric.pending.wake.drains", static_cast<uint64_t>(sys_debug_counter(26)));
        console_log_counter("metric.doorbell.local.wakes", static_cast<uint64_t>(sys_debug_counter(27)));
        console_log_counter("metric.doorbell.remote.wakes", static_cast<uint64_t>(sys_debug_counter(28)));
        console_log_counter("metric.ressha.fairness.max.lag", static_cast<uint64_t>(sys_debug_counter(29)));
        console_log_counter("metric.principal.a.runtime", static_cast<uint64_t>(sys_debug_counter(30)));
        console_log_counter("metric.principal.b.runtime", static_cast<uint64_t>(sys_debug_counter(31)));
    }

    if (dango && auth_probe &&
        dango_proc != kNullCap &&
        auth_probe_proc != kNullCap &&
        dango->main_thread != kNullCap &&
        auth_probe->main_thread != kNullCap) {
        const CapHandle db_dango_done = as_cap(sys_doorbell_create());
        const CapHandle db_dango_auth_gate = as_cap(sys_doorbell_create());
        const CapHandle db_dango_layout_done = as_cap(sys_doorbell_create());
        if (db_dango_done == kNullCap ||
            db_dango_auth_gate == kNullCap ||
            db_dango_layout_done == kNullCap) {
            fail_halt("dango: wiring failed\n");
        }
        g_rt.db_dango_done = db_dango_done;
        g_rt.db_dango_auth_gate = db_dango_auth_gate;
        g_rt.db_dango_layout_done = db_dango_layout_done;

        share_ring_to(*dango, dango->log_ring, "log.ring");
        share_doorbell_to(*dango, console_db, "log.doorbell");
        grant_to(*dango, kCapVmRoot, RIGHT_VM_CREATE | RIGHT_VM_INSPECT,
                 MX_BOOT_ENTRY_HANDLE, "vm.root");
        share_doorbell_to(*dango, db_dango_done, "dango.done.db", RIGHT_NOTIFY);
        share_doorbell_to(*dango, db_dango_auth_gate, "dango.auth_gate.db", RIGHT_WAIT);
        share_doorbell_to(*dango, db_dango_layout_done, "dango.layout_done.db", RIGHT_NOTIFY);
        grant_to(*dango, kCapInitrd, RIGHT_READ | RIGHT_MAP,
                 MX_BOOT_ENTRY_HANDLE, "boot.initrd");
        boot_publish(*dango);

        share_ring_to(*auth_probe, auth_probe->log_ring, "log.ring");
        share_doorbell_to(*auth_probe, console_db, "log.doorbell");
        grant_to(*auth_probe, kCapVmRoot, RIGHT_VM_INSPECT,
                 MX_BOOT_ENTRY_HANDLE, "vm.root.restricted");
        share_doorbell_to(*auth_probe, db_dango_auth_gate, "dango.auth_gate.db", RIGHT_NOTIFY);
        boot_publish(*auth_probe);

        const uint64_t layout_base = sys_doorbell_read(db_dango_layout_done);
        const uint64_t dango_done_base = sys_doorbell_read(db_dango_done);

        require_ok(set_service_affinity(dango->main_thread, 1ull << 1),
                   "dango: wiring failed\n");
        require_ok(sys_thread_resume(dango->main_thread), "dango: wiring failed\n");
        if (sys_wait_doorbell(db_dango_layout_done, layout_base) != 0) {
            fail_halt("dango: wiring failed\n");
        }

        require_ok(set_service_affinity(auth_probe->main_thread, 1ull << 0),
                   "dango: wiring failed\n");
        require_ok(sys_thread_resume(auth_probe->main_thread), "dango: wiring failed\n");
        if (sys_wait_doorbell(db_dango_done, dango_done_base) != 0) {
            fail_halt("dango: wiring failed\n");
        }

        if constexpr (kEmitBootMetrics) {
            console_log_counter("metric.vm.create.calls", static_cast<uint64_t>(sys_debug_counter(32)));
            console_log_counter("metric.vm.created", static_cast<uint64_t>(sys_debug_counter(33)));
            console_log_counter("metric.vm.destroyed", static_cast<uint64_t>(sys_debug_counter(34)));
            console_log_counter("metric.vm.memory.created", static_cast<uint64_t>(sys_debug_counter(35)));
            console_log_counter("metric.vm.memory.mapped", static_cast<uint64_t>(sys_debug_counter(36)));
            console_log_counter("metric.vm.memory.unmapped", static_cast<uint64_t>(sys_debug_counter(37)));
            console_log_counter("metric.vm.stage2.tables.created", static_cast<uint64_t>(sys_debug_counter(38)));
            console_log_counter("metric.vm.stage2.pages.mapped", static_cast<uint64_t>(sys_debug_counter(39)));
            console_log_counter("metric.vm.vcpus.created", static_cast<uint64_t>(sys_debug_counter(40)));
            console_log_counter("metric.vm.vcpu.configured", static_cast<uint64_t>(sys_debug_counter(41)));
            console_log_counter("metric.vm.vcpu.run.attempts", static_cast<uint64_t>(sys_debug_counter(42)));
            console_log_counter("metric.vm.vcpu.run.notimpl", static_cast<uint64_t>(sys_debug_counter(43)));
            console_log_counter("metric.vm.channels.created", static_cast<uint64_t>(sys_debug_counter(44)));
            console_log_counter("metric.vm.errors", static_cast<uint64_t>(sys_debug_counter(45)));
            console_log_counter("metric.el.current", static_cast<uint64_t>(sys_debug_counter(46)));
            console_log_counter("metric.el2.available", static_cast<uint64_t>(sys_debug_counter(47)));
            console_log_counter("metric.vhe.available", static_cast<uint64_t>(sys_debug_counter(48)));
        }
        console_log("dango ready\n");
    }
    // Driver services. Wire by service name so the manifest order is not part
    // of the capability contract. Start consumers before producers.
    {
        const CapHandle fb_cmd_ring = as_cap(sys_ring_create(kFbCmdRingOrder, 0));
        const CapHandle fb_reply_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle fb_cmd_db = as_cap(sys_doorbell_create());
        const CapHandle fb_reply_db = as_cap(sys_doorbell_create());
        const CapHandle xhci_hid_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle xhci_hid_db = as_cap(sys_doorbell_create());
        const CapHandle hid_input_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle hid_input_db = as_cap(sys_doorbell_create());
        const CapHandle input_tty_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle input_tty_db = as_cap(sys_doorbell_create());
        const CapHandle blk_req_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle blk_reply_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle blk_req_db = as_cap(sys_doorbell_create());
        const CapHandle blk_reply_db = as_cap(sys_doorbell_create());
        const CapHandle fs_cmd_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle fs_reply_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle fs_cmd_db = as_cap(sys_doorbell_create());
        const CapHandle fs_reply_db = as_cap(sys_doorbell_create());
        const CapHandle term_input_ring = as_cap(sys_ring_create(kRingOrder, 0));   // tty -> msh
        const CapHandle term_output_ring = as_cap(sys_ring_create(kRingOrder, 0));  // msh -> tty
        const CapHandle term_input_db = as_cap(sys_doorbell_create());
        const CapHandle term_output_db = as_cap(sys_doorbell_create());
        const CapHandle svc_cmd_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle svc_reply_ring = as_cap(sys_ring_create(kRingOrder, 0));
        const CapHandle svc_cmd_db = as_cap(sys_doorbell_create());
        const CapHandle svc_reply_db = as_cap(sys_doorbell_create());
        if (fb_cmd_ring == kNullCap || fb_reply_ring == kNullCap ||
            fb_cmd_db == kNullCap || fb_reply_db == kNullCap ||
            xhci_hid_ring == kNullCap || xhci_hid_db == kNullCap ||
            hid_input_ring == kNullCap || hid_input_db == kNullCap ||
            input_tty_ring == kNullCap || input_tty_db == kNullCap ||
            blk_req_ring == kNullCap || blk_reply_ring == kNullCap ||
            blk_req_db == kNullCap || blk_reply_db == kNullCap ||
            fs_cmd_ring == kNullCap || fs_reply_ring == kNullCap ||            fs_cmd_db == kNullCap || fs_reply_db == kNullCap ||
            term_input_ring == kNullCap || term_output_ring == kNullCap ||
            term_input_db == kNullCap || term_output_db == kNullCap ||
            svc_cmd_ring == kNullCap || svc_reply_ring == kNullCap ||
            svc_cmd_db == kNullCap || svc_reply_db == kNullCap) {
            fail_halt("init: framebuffer endpoint create failed\n");
        }
        g_rt.fb_cmd_ring = fb_cmd_ring;
        g_rt.fb_reply_ring = fb_reply_ring;
        g_rt.fb_cmd_db = fb_cmd_db;
        g_rt.fb_reply_db = fb_reply_db;
        g_rt.xhci_hid_ring = xhci_hid_ring;
        g_rt.xhci_hid_db = xhci_hid_db;
        g_rt.hid_input_ring = hid_input_ring;
        g_rt.hid_input_db = hid_input_db;
        g_rt.input_tty_ring = input_tty_ring;
        g_rt.input_tty_db = input_tty_db;
        g_rt.blk_req_ring = blk_req_ring;
        g_rt.blk_reply_ring = blk_reply_ring;
        g_rt.blk_req_db = blk_req_db;
        g_rt.blk_reply_db = blk_reply_db;
        g_rt.fs_cmd_ring = fs_cmd_ring;
        g_rt.fs_reply_ring = fs_reply_ring;
        g_rt.fs_cmd_db = fs_cmd_db;
        g_rt.fs_reply_db = fs_reply_db;
        g_rt.term_input_ring = term_input_ring;
        g_rt.term_output_ring = term_output_ring;
        g_rt.term_input_db = term_input_db;
        g_rt.term_output_db = term_output_db;
        g_rt.svc_ctl_req_ring = svc_cmd_ring;
        g_rt.svc_ctl_reply_ring = svc_reply_ring;
        g_rt.svc_ctl_req_db = svc_cmd_db;
        g_rt.svc_ctl_reply_db = svc_reply_db;
        if (sys_map_page(svc_cmd_ring, kVaSvcCtlReqRing, RIGHT_READ | RIGHT_WRITE) != 0 ||
            sys_map_page(svc_reply_ring, kVaSvcCtlReplyRing, RIGHT_READ | RIGHT_WRITE) != 0) {
            fail_halt("init: svc control map failed\n");
        }
        g_rt.svcctl.ep.rx_ring = reinterpret_cast<RingPage*>(kVaSvcCtlReqRing);
        g_rt.svcctl.ep.tx_ring = reinterpret_cast<RingPage*>(kVaSvcCtlReplyRing);
        g_rt.svcctl.ep.order = kRingOrder;
        g_rt.svcctl.ep.rx_doorbell = svc_cmd_db;
        g_rt.svcctl.ep.tx_doorbell = svc_reply_db;

        ServiceSpec* fb = find_service(services, service_count, "io.framebuffer");
        ServiceSpec* tty = find_service(services, service_count, "tty.fb0");
        ServiceSpec* pci = find_service(services, service_count, "drv.pci");
        ServiceSpec* input = find_service(services, service_count, "ui.input");
        ServiceSpec* hid = find_service(services, service_count, "drv.usb.hid");
        ServiceSpec* xhci = find_service(services, service_count, "drv.usb.xhci");
        ServiceSpec* blk = find_service(services, service_count, "drv.blk.virtio");
        ServiceSpec* fs = find_service(services, service_count, "io.fs");
        ServiceSpec* shell = find_service(services, service_count, "msh");

        if (service_started(fb)) {
            share_service_log(*fb, console_db);
            if (sys_cap_identify(kCapFbMemory) == kCapTypeMmio)
                grant_to(*fb, kCapFbMemory,
                         RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                         MX_BOOT_ENTRY_MMIO, "fb.memory");
            share_ring_to_with_rights(*fb, fb_cmd_ring, "fb.cmd.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                                      kFbCmdRingOrder);
            share_ring_to_with_rights(*fb, fb_reply_ring, "fb.reply.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*fb, fb_cmd_db, "fb.cmd.doorbell",
                              RIGHT_WAIT);
            share_doorbell_to(*fb, fb_reply_db, "fb.reply.doorbell",
                              RIGHT_NOTIFY);
            publish_resume_service(*fb);
        }

        if (service_started(tty)) {
            share_service_log(*tty, console_db);
            share_ring_to_with_rights(*tty, fb_cmd_ring, "fb.cmd.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                                      kFbCmdRingOrder);
            share_ring_to_with_rights(*tty, fb_reply_ring, "fb.reply.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*tty, fb_cmd_db, "fb.cmd.doorbell",
                              RIGHT_NOTIFY);
            share_doorbell_to(*tty, fb_reply_db, "fb.reply.doorbell",
                              RIGHT_WAIT);
            share_ring_to_with_rights(*tty, console_mirror_ring, "console.mirror.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                                      kConsoleMirrorRingOrder);
            share_doorbell_to(*tty, console_mirror_db, "console.mirror.doorbell",
                              RIGHT_WAIT);
            share_ring_to_with_rights(*tty, input_tty_ring, "input.tty.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*tty, input_tty_db, "input.tty.doorbell",
                              RIGHT_WAIT);
            share_ring_to_with_rights(*tty, term_input_ring, "term.input.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*tty, term_input_db, "term.input.doorbell",
                              RIGHT_NOTIFY);
            share_ring_to_with_rights(*tty, term_output_ring, "term.output.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*tty, term_output_db, "term.output.doorbell",
                              RIGHT_WAIT);
            publish_resume_service(*tty);
        }

        if (service_started(input)) {
            share_service_log(*input, console_db);
            share_ring_to_with_rights(*input, hid_input_ring, "hid.input.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*input, hid_input_db, "hid.input.doorbell",
                              RIGHT_WAIT);
            share_ring_to_with_rights(*input, input_tty_ring, "input.tty.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*input, input_tty_db, "input.tty.doorbell",
                              RIGHT_NOTIFY);
            publish_resume_service(*input);
        }

        if (service_started(hid)) {
            share_service_log(*hid, console_db);
            share_ring_to_with_rights(*hid, xhci_hid_ring, "xhci.hid.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*hid, xhci_hid_db, "xhci.hid.doorbell",
                              RIGHT_WAIT);
            share_ring_to_with_rights(*hid, hid_input_ring, "hid.input.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*hid, hid_input_db, "hid.input.doorbell",
                              RIGHT_NOTIFY);
            publish_resume_service(*hid);
        }

        if (service_started(xhci)) {
            share_service_log(*xhci, console_db);
            if (sys_cap_identify(kCapXhciMmio) == kCapTypeMmio) {
                grant_to(*xhci, kCapXhciMmio,
                         RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                         MX_BOOT_ENTRY_MMIO, "xhci.mmio");
            }
            if (sys_cap_identify(kCapXhciIrq) == kCapTypeIrq) {
                grant_to(*xhci, kCapXhciIrq,
                         RIGHT_READ | RIGHT_WRITE,
                         MX_BOOT_ENTRY_HANDLE, "xhci.irq");
            }
            if (sys_cap_identify(kCapXhciDma) == kCapTypeArena) {
                grant_to(*xhci, kCapXhciDma,
                         RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                         MX_BOOT_ENTRY_ARENA, "xhci.dma");
            }
            share_ring_to_with_rights(*xhci, xhci_hid_ring, "xhci.hid.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*xhci, xhci_hid_db, "xhci.hid.doorbell",
                              RIGHT_NOTIFY);
            publish_resume_service(*xhci);
        }

        if (service_started(pci)) {
            share_service_log(*pci, console_db);
            publish_resume_service(*pci);
        }

        if (service_started(blk)) {
            share_service_log(*blk, console_db);
            if (sys_cap_identify(kCapVirtioBlkMmio) == kCapTypeMmio) {
                grant_to(*blk, kCapVirtioBlkMmio,
                         RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                         MX_BOOT_ENTRY_MMIO, "virtio.blk.mmio");
            }
            if (sys_cap_identify(kCapVirtioBlkDma) == kCapTypeArena) {
                grant_to(*blk, kCapVirtioBlkDma,
                         RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                         MX_BOOT_ENTRY_ARENA, "virtio.blk.dma");
            }
            share_ring_to_with_rights(*blk, blk_req_ring, "blk.req.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_ring_to_with_rights(*blk, blk_reply_ring, "blk.reply.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*blk, blk_req_db, "blk.req.doorbell",
                              RIGHT_WAIT);
            share_doorbell_to(*blk, blk_reply_db, "blk.reply.doorbell",
                              RIGHT_NOTIFY);
            publish_resume_service(*blk);
        }

        if (service_started(fs)) {
            share_service_log(*fs, console_db);
            if (sys_cap_identify(kCapVirtioBlkDma) == kCapTypeArena) {
                grant_to(*fs, kCapVirtioBlkDma,
                         RIGHT_READ | RIGHT_WRITE | RIGHT_MAP,
                         MX_BOOT_ENTRY_ARENA, "virtio.blk.dma");
            }
            share_ring_to_with_rights(*fs, fs_cmd_ring, "fs.cmd.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_ring_to_with_rights(*fs, fs_reply_ring, "fs.reply.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*fs, fs_cmd_db, "fs.cmd.doorbell",
                              RIGHT_WAIT);
            share_doorbell_to(*fs, fs_reply_db, "fs.reply.doorbell",
                              RIGHT_NOTIFY);
            share_ring_to_with_rights(*fs, blk_req_ring, "blk.req.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_ring_to_with_rights(*fs, blk_reply_ring, "blk.reply.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*fs, blk_req_db, "blk.req.doorbell",
                              RIGHT_NOTIFY);
            share_doorbell_to(*fs, blk_reply_db, "blk.reply.doorbell",
                              RIGHT_WAIT);
            publish_resume_service(*fs);
            const uint32_t loaded = load_service_images_from_fs(
                fs_cmd_ring, fs_reply_ring, fs_cmd_db, fs_reply_db);
            if (loaded > 0) {
                console_log("userland images loaded from AmeFS\n");
                console_log_counter("metric.userland.fs.images", loaded);
                if (shell && respawn_suspended_from_loaded_image(*shell)) {
                    console_log("msh image source AmeFS\n");
                }
            }
        }

        if (service_started(shell) && shell_proc != kNullCap) {
            share_service_log(*shell, console_db);
            share_ring_to_with_rights(*shell, term_input_ring, "term.input.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*shell, term_input_db, "term.input.doorbell",
                              RIGHT_WAIT);
            share_ring_to_with_rights(*shell, term_output_ring, "term.output.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*shell, term_output_db, "term.output.doorbell",
                              RIGHT_NOTIFY);
            if (service_started(fs)) {
                share_ring_to_with_rights(*shell, fs_cmd_ring, "fs.cmd.ring",
                                          RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
                share_ring_to_with_rights(*shell, fs_reply_ring, "fs.reply.ring",
                                          RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
                share_doorbell_to(*shell, fs_cmd_db, "fs.cmd.doorbell",
                                  RIGHT_NOTIFY);
                share_doorbell_to(*shell, fs_reply_db, "fs.reply.doorbell",
                                  RIGHT_WAIT);
            }
            grant_to(*shell, svc_registry, RIGHT_READ | RIGHT_MAP,
                     MX_BOOT_ENTRY_ARENA, "svc.registry");
            share_ring_to_with_rights(*shell, svc_cmd_ring, "svc.cmd.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*shell, svc_cmd_db, "svc.cmd.doorbell",
                              RIGHT_NOTIFY);
            share_ring_to_with_rights(*shell, svc_reply_ring, "svc.reply.ring",
                                      RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
            share_doorbell_to(*shell, svc_reply_db, "svc.reply.doorbell",
                              RIGHT_WAIT);
            // Publish BootInfo but DO NOT resume yet. The resume is deferred to
            // after "system ready" so msh's banner and first prompt cannot race
            // with the tail of the boot log on the framebuffer mirror.
            boot_publish(*shell);
            registry_sync(*shell);
        }
        console_log("drivers ready\n");
    }

    console_log("system ready\n");

    // Resume msh last so the framebuffer terminal has finished drawing the boot
    // log (in particular "system ready", which also unlocks the input gate in
    // tty.fb0) before the shell starts producing output.
    {
        ServiceSpec* shell = find_service(g_rt.services, g_rt.service_count, "msh");
        if (shell && shell->proc != kNullCap && shell->main_thread != kNullCap) {
            require_ok(sys_thread_resume(shell->main_thread),
                       "init: shell resume failed\n");
            registry_sync(*shell);
        }
    }

    while (true) {
        bool progress = false;
        if (handle_svc_control_requests()) progress = true;
        const SysPair event = sys_supervisor_poll(kCapSupervisor);
        if (event.value > 0) progress = true;
        if (!progress) sys_yield();
    }
}
