// userland/dango/manager/main.cc — Linux appliance construction and channel probe.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "dango/vm.h"
#include "dango/channel.h"
#include "dango/layout.h"
#include "midix/abi/image.h"
#include "midix/abi/dango_channel_guest.h"
#include "midix/abi/dango_channel_test.h"
#include "midix/abi/protocols/gpu.h"

// The channel ring order is a guest-visible wire property; the manager must
// agree with the driver and the DTS generator, so take it from the shared ABI.
static_assert(DANGO_CH_DEFAULT_RING_ORDER == MX_DEFAULT_RING_ORDER,
              "channel ring order must match the Midix ring default");

constexpr uint64_t kLinuxRunBudgetNs = 6ull * 1000ull * 1000ull;
constexpr uint64_t kLinuxRunPeriodNs = 16666667ull;
// Linux early boot does a lot of tiny MMIO exits, especially console output.
// Amortise the user/kernel syscall boundary over more guest exits.
constexpr uint32_t kLinuxRunMaxExits = 4096;
// Keep the bootstrap runner on the manager CPU.  Init starts dango.manager on
// CPU1 so the vCPU can own that core without stalling the CPU0 boot path.
constexpr uint64_t kLinuxRunnerCoreMask = 1ull << 1;
constexpr uint32_t kLinuxRunnerHomeCpu = 1;

// The Linux vCPU runner is a *VM-class* workload: the kernel has dedicated VM
// bands precisely so a guest cannot outrank native services.  Spec §7: sustained
// rendering work stays budgeted, and the latency hint belongs to the completion
// notification path, not the runner.  SCHED_BAND_VM_FAIR is the budgeted VM band.
constexpr uint32_t kLinuxRunnerSchedBand = dango::SCHED_BAND_VM_FAIR;
constexpr uint64_t kVaConsoleRing = 0x2000'0000ull;
constexpr uint64_t kVaBootInitrd = 0x2400'0000ull;
constexpr uint64_t kInitrdScanLimit = 64ull * 1024ull * 1024ull;

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(TarHeader) == 512);

struct LinuxArtifacts {
    const uint8_t* manifest;
    uint64_t manifest_size;
    const uint8_t* image;
    uint64_t image_size;
    const uint8_t* initramfs;
    uint64_t initramfs_size;
    const uint8_t* dtb;
    uint64_t dtb_size;
    dango::VmLayout layout;
    uint64_t entry;
    bool gpu_profile;
};

static uint32_t slen(const char* s) {
    uint32_t n = 0;
    while (s[n]) ++n;
    return n;
}

static CapHandle as_cap(int64_t v) {
    return v > 0 ? static_cast<CapHandle>(v) : kNullCap;
}

template <typename T>
static T* wait_map(CapHandle cap, int64_t expected_type, uint64_t va, uint32_t rights) {
    while (true) {
        if (sys_cap_identify(cap) == expected_type) {
            if (sys_map_page(cap, va, rights) == 0) return reinterpret_cast<T*>(va);
        }
        sys_yield();
    }
}

static void wait_cap(CapHandle cap, int64_t expected_type) {
    while (sys_cap_identify(cap) != expected_type) {
        sys_yield();
    }
}

static void console_log(const char* msg) {
    mochi::log_raw_marker(msg);
}

static void dango_log(const char* msg) {
    mochi::log_write(mochi::LogLevel::Info, msg);
}

static void dango_debug(const char* msg) {
    (void)msg;
}

static void dango_counter(const char* key, uint64_t value) {
    mochi::log_counter(key, value);
}

static uint64_t status_magnitude(int64_t rc) {
    return rc < 0 ? static_cast<uint64_t>(-rc) : static_cast<uint64_t>(rc);
}

[[noreturn]] static void fail(const char* msg) {
    // Keep failures visible even if the console service is blocked.
    sys_debug_puts(msg, slen(msg));
    console_log(msg);
    while (true) sys_yield();
}

static bool aligned(uint64_t v) {
    return (v & (dango::kPageSize - 1)) == 0;
}

static bool checked_add(uint64_t a, uint64_t b, uint64_t* out) {
    const uint64_t r = a + b;
    if (r < a) return false;
    *out = r;
    return true;
}

static uint64_t round_up_page(uint64_t value) {
    return (value + dango::kPageSize - 1) & ~(dango::kPageSize - 1);
}

static bool overlaps(uint64_t a_base, uint64_t a_size, uint64_t b_base, uint64_t b_size) {
    uint64_t a_end = 0;
    uint64_t b_end = 0;
    if (!checked_add(a_base, a_size, &a_end)) return true;
    if (!checked_add(b_base, b_size, &b_end)) return true;
    return a_base < b_end && a_end > b_base;
}

static bool contains(uint64_t outer_base, uint64_t outer_size,
                     uint64_t inner_base, uint64_t inner_size) {
    uint64_t outer_end = 0;
    uint64_t inner_end = 0;
    if (!checked_add(outer_base, outer_size, &outer_end)) return false;
    if (!checked_add(inner_base, inner_size, &inner_end)) return false;
    return inner_base >= outer_base && inner_end <= outer_end && inner_end >= inner_base;
}

static bool validate_layout(const dango::VmLayout& layout) {
    if (!aligned(layout.ram_base) || !aligned(layout.ram_size)) return false;
    if (!aligned(layout.kernel_load) || !aligned(layout.initramfs_load)) return false;
    if (!aligned(layout.dtb_base) || !aligned(layout.dtb_size)) return false;
    if (!aligned(layout.mmio_base) || !aligned(layout.mmio_size)) return false;
    if (!aligned(layout.channel_base) || !aligned(layout.channel_size)) return false;
    if (layout.ram_size == 0 || layout.channel_size == 0) return false;
    if (!contains(layout.ram_base, layout.ram_size, layout.kernel_load, dango::kPageSize)) return false;
    if (!contains(layout.ram_base, layout.ram_size, layout.initramfs_load, dango::kPageSize)) return false;
    if (!contains(layout.ram_base, layout.ram_size, layout.dtb_base, layout.dtb_size)) return false;
    // The hello layout reserves space inside RAM; the GPU transport must use
    // separate, non-DMA pages outside it. Reject a partial overlap in either case.
    if (overlaps(layout.ram_base, layout.ram_size, layout.channel_base, layout.channel_size) &&
        !contains(layout.ram_base, layout.ram_size, layout.channel_base, layout.channel_size)) return false;
    if (overlaps(layout.ram_base, layout.ram_size, layout.mmio_base, layout.mmio_size)) return false;
    if (overlaps(layout.dtb_base, layout.dtb_size, layout.mmio_base, layout.mmio_size)) return false;
    if (overlaps(layout.dtb_base, layout.dtb_size, layout.channel_base, layout.channel_size)) return false;
    if (overlaps(layout.mmio_base, layout.mmio_size, layout.channel_base, layout.channel_size)) return false;
    return true;
}

static bool block_zero(const uint8_t* data) {
    for (uint32_t i = 0; i < 512; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

static uint64_t octal_value(const char* s, uint32_t len) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < len; ++i) {
        const char c = s[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') continue;
        value = (value << 3) + static_cast<uint64_t>(c - '0');
    }
    return value;
}

static bool streq(const char* a, const char* b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
}

static bool tar_name_matches(const TarHeader* h, const char* wanted) {
    char full[160];
    uint32_t n = 0;
    if (h->prefix[0]) {
        for (uint32_t i = 0; i < sizeof(h->prefix) && h->prefix[i] && n + 1 < sizeof(full); ++i) {
            full[n++] = h->prefix[i];
        }
        if (n + 1 < sizeof(full)) full[n++] = '/';
    }
    for (uint32_t i = 0; i < sizeof(h->name) && h->name[i] && n + 1 < sizeof(full); ++i) {
        full[n++] = h->name[i];
    }
    full[n] = '\0';
    return streq(full, wanted);
}

static const uint8_t* tar_find(const uint8_t* base, const char* name, uint64_t* out_size) {
    if (!base || !name || !out_size) return nullptr;

    uint64_t off = 0;
    while (off + sizeof(TarHeader) <= kInitrdScanLimit) {
        const auto* h = reinterpret_cast<const TarHeader*>(base + off);
        if (block_zero(base + off)) return nullptr;
        const uint64_t size = octal_value(h->size, sizeof(h->size));
        const uint64_t data_off = off + sizeof(TarHeader);
        const uint64_t rounded = (size + 511ull) & ~511ull;
        if (data_off + rounded < data_off || data_off + rounded > kInitrdScanLimit) return nullptr;
        if ((h->typeflag == '\0' || h->typeflag == '0') && tar_name_matches(h, name)) {
            *out_size = size;
            return base + data_off;
        }
        off = data_off + rounded;
    }
    return nullptr;
}

static bool range_within_bytes(uint64_t off, uint64_t len, uint64_t total) {
    const uint64_t end = off + len;
    return end >= off && end <= total;
}

static bool bundle_name_eq(const uint8_t* strings, uint64_t strings_size,
                           uint32_t name_offset, const char* wanted) {
    if (!strings || !wanted || name_offset >= strings_size) return false;
    uint64_t pos = name_offset;
    uint32_t i = 0;
    while (pos < strings_size && strings[pos] && wanted[i]) {
        if (static_cast<char>(strings[pos]) != wanted[i]) return false;
        ++pos;
        ++i;
    }
    return pos < strings_size && strings[pos] == '\0' && wanted[i] == '\0';
}

static const uint8_t* bundle_find(const uint8_t* base, const char* name, uint64_t* out_size) {
    if (!base || !name || !out_size) return nullptr;
    const auto* h = reinterpret_cast<const MxBundleHeader*>(base);
    if (h->magic != MX_BUNDLE_MAGIC ||
        h->version != MX_BUNDLE_VERSION ||
        h->object_count > 64 ||
        h->bundle_size == 0 ||
        h->bundle_size > kInitrdScanLimit) {
        return nullptr;
    }
    const uint64_t object_table_size = static_cast<uint64_t>(h->object_count) * sizeof(MxBundleObject);
    if (!range_within_bytes(h->object_table_offset, object_table_size, h->bundle_size) ||
        !range_within_bytes(h->string_table_offset, h->string_table_size, h->bundle_size)) {
        return nullptr;
    }
    const auto* objects = reinterpret_cast<const MxBundleObject*>(base + h->object_table_offset);
    const uint8_t* strings = base + h->string_table_offset;
    for (uint32_t i = 0; i < h->object_count; ++i) {
        const MxBundleObject& obj = objects[i];
        if (!bundle_name_eq(strings, h->string_table_size, obj.name_offset, name)) continue;
        if (!range_within_bytes(obj.offset, obj.size, h->bundle_size)) return nullptr;
        *out_size = obj.size;
        return base + obj.offset;
    }
    return nullptr;
}

static const uint8_t* archive_find(const uint8_t* base, const char* name, uint64_t* out_size) {
    if (!base) return nullptr;
    const auto* h = reinterpret_cast<const MxBundleHeader*>(base);
    if (h->magic == MX_BUNDLE_MAGIC) return bundle_find(base, name, out_size);
    return tar_find(base, name, out_size);
}


static bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool key_at(const uint8_t* data, uint64_t size, uint64_t off, const char* key) {
    if (off != 0) {
        const char prev = static_cast<char>(data[off - 1]);
        if (prev != '\n' && prev != '\r') return false;
    }
    uint64_t i = off;
    while (i < size && (data[i] == ' ' || data[i] == '\t')) ++i;
    for (uint32_t k = 0; key[k]; ++k, ++i) {
        if (i >= size || static_cast<char>(data[i]) != key[k]) return false;
    }
    while (i < size && (data[i] == ' ' || data[i] == '\t')) ++i;
    return i < size && data[i] == '=';
}

static const uint8_t* manifest_value(const uint8_t* data, uint64_t size,
                                     const char* key, uint64_t* out_len) {
    if (!data || !key || !out_len) return nullptr;
    for (uint64_t off = 0; off < size; ++off) {
        if (!key_at(data, size, off, key)) continue;
        uint64_t i = off;
        while (i < size && data[i] != '=') ++i;
        if (i >= size) return nullptr;
        ++i;
        while (i < size && ascii_space(static_cast<char>(data[i]))) ++i;
        if (i >= size) return nullptr;
        if (data[i] == '"') {
            const uint64_t start = ++i;
            while (i < size && data[i] != '"') ++i;
            if (i >= size) return nullptr;
            *out_len = i - start;
            return data + start;
        }
        const uint64_t start = i;
        while (i < size && !ascii_space(static_cast<char>(data[i])) && data[i] != '#') ++i;
        *out_len = i - start;
        return data + start;
    }
    return nullptr;
}

static bool token_equals(const uint8_t* p, uint64_t len, const char* s) {
    uint64_t i = 0;
    while (i < len && s[i]) {
        if (static_cast<char>(p[i]) != s[i]) return false;
        ++i;
    }
    return i == len && s[i] == '\0';
}

static bool parse_u64_token(const uint8_t* p, uint64_t len, uint64_t* out) {
    if (!p || !out || len == 0) return false;
    uint64_t i = 0;
    uint32_t base = 10;
    if (len >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i >= len) return false;
    uint64_t value = 0;
    for (; i < len; ++i) {
        const char c = static_cast<char>(p[i]);
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        if (digit >= base) return false;
        if (value > (~0ull - digit) / base) return false;
        value = value * base + digit;
    }
    *out = value;
    return true;
}

static bool manifest_u64(const LinuxArtifacts& artifacts, const char* key, uint64_t* out) {
    uint64_t len = 0;
    const uint8_t* value = manifest_value(artifacts.manifest, artifacts.manifest_size, key, &len);
    return parse_u64_token(value, len, out);
}

static bool parse_linux_manifest(LinuxArtifacts* artifacts) {
    if (!artifacts || !artifacts->manifest || artifacts->manifest_size == 0) return false;

    uint64_t manifest_kernel_size = 0;
    uint64_t manifest_initramfs_size = 0;
    uint64_t manifest_dtb_size = 0;
    if (!manifest_u64(*artifacts, "kernel_size", &manifest_kernel_size) ||
        !manifest_u64(*artifacts, "initramfs_size", &manifest_initramfs_size) ||
        !manifest_u64(*artifacts, "dtb_size", &manifest_dtb_size) ||
        manifest_kernel_size != artifacts->image_size ||
        manifest_initramfs_size != artifacts->initramfs_size ||
        manifest_dtb_size != artifacts->dtb_size) {
        return false;
    }

    dango::VmLayout layout = dango::v3_layout();
    if (!manifest_u64(*artifacts, "memory_base", &layout.ram_base) ||
        !manifest_u64(*artifacts, "memory_size", &layout.ram_size) ||
        !manifest_u64(*artifacts, "kernel_load", &layout.kernel_load) ||
        !manifest_u64(*artifacts, "initramfs_load", &layout.initramfs_load) ||
        !manifest_u64(*artifacts, "dtb_load", &layout.dtb_base)) {
        return false;
    }

    layout.dtb_size = round_up_page(artifacts->dtb_size);
    if (layout.dtb_size < dango::kDtbSize) layout.dtb_size = dango::kDtbSize;
    layout.mmio_base = dango::kMmioBase;
    layout.mmio_size = dango::kMmioSize;
    layout.channel_base = dango::kChannelBase;
    layout.channel_size = dango::kChannelSize;

    uint64_t entry_len = 0;
    const uint8_t* entry = manifest_value(artifacts->manifest, artifacts->manifest_size,
                                          "entry", &entry_len);
    uint64_t entry_pc = 0;
    if (entry && token_equals(entry, entry_len, "kernel_load")) {
        entry_pc = layout.kernel_load;
    } else if (!entry || !parse_u64_token(entry, entry_len, &entry_pc)) {
        return false;
    }

    uint64_t profile_len = 0;
    const auto* profile = manifest_value(artifacts->manifest, artifacts->manifest_size,
                                         "profile", &profile_len);
    artifacts->gpu_profile = profile && token_equals(profile, profile_len, "gpu");
    if (profile && !artifacts->gpu_profile && !token_equals(profile, profile_len, "hello")) return false;
    if (artifacts->gpu_profile) {
        layout.channel_base = dango::kChannelGuestBase;
        layout.channel_size = dango::kChannelGuestSize;
    }
    artifacts->layout = layout;
    artifacts->entry = entry_pc;
    return true;
}

static bool discover_linux_artifacts(const uint8_t* archive, LinuxArtifacts* out) {
    if (!archive || !out) {
        dango_log("linux artifact discovery failed: boot archive unavailable\n");
        return false;
    }
    LinuxArtifacts artifacts{};
    artifacts.manifest = archive_find(archive, "linux/manifest.toml", &artifacts.manifest_size);
    artifacts.image = archive_find(archive, "linux/Image", &artifacts.image_size);
    artifacts.initramfs = archive_find(archive, "linux/initramfs.cpio", &artifacts.initramfs_size);
    artifacts.dtb = archive_find(archive, "linux/dango-linux.dtb", &artifacts.dtb_size);

    bool missing = false;
    if (!artifacts.manifest) {
        dango_log("linux artifact missing: linux/manifest.toml\n");
        missing = true;
    }
    if (!artifacts.image) {
        dango_log("linux artifact missing: linux/Image\n");
        missing = true;
    }
    if (!artifacts.initramfs) {
        dango_log("linux artifact missing: linux/initramfs.cpio\n");
        missing = true;
    }
    if (!artifacts.dtb) {
        dango_log("linux artifact missing: linux/dango-linux.dtb\n");
        missing = true;
    }
    if (missing) return false;

    if (!parse_linux_manifest(&artifacts)) {
        dango_log("linux artifact discovery failed: manifest invalid or size mismatch\n");
        return false;
    }
    out->manifest = artifacts.manifest;
    out->manifest_size = artifacts.manifest_size;
    out->image = artifacts.image;
    out->image_size = artifacts.image_size;
    out->initramfs = artifacts.initramfs;
    out->initramfs_size = artifacts.initramfs_size;
    out->dtb = artifacts.dtb;
    out->dtb_size = artifacts.dtb_size;
    out->layout.ram_base = artifacts.layout.ram_base;
    out->layout.ram_size = artifacts.layout.ram_size;
    out->layout.kernel_load = artifacts.layout.kernel_load;
    out->layout.initramfs_load = artifacts.layout.initramfs_load;
    out->layout.dtb_base = artifacts.layout.dtb_base;
    out->layout.dtb_size = artifacts.layout.dtb_size;
    out->layout.mmio_base = artifacts.layout.mmio_base;
    out->layout.mmio_size = artifacts.layout.mmio_size;
    out->layout.channel_base = artifacts.layout.channel_base;
    out->layout.channel_size = artifacts.layout.channel_size;
    out->entry = artifacts.entry;
    out->gpu_profile = artifacts.gpu_profile;
    return true;
}

static uint64_t guest_offset(const dango::VmLayout& layout, uint64_t guest_pa) {
    return guest_pa - layout.ram_base;
}

static uint32_t a64_movz64(unsigned rd, uint16_t imm, unsigned shift) {
    return 0xD2800000u | ((shift / 16u) << 21) | (static_cast<uint32_t>(imm) << 5) | rd;
}

static uint32_t a64_movk64(unsigned rd, uint16_t imm, unsigned shift) {
    return 0xF2800000u | ((shift / 16u) << 21) | (static_cast<uint32_t>(imm) << 5) | rd;
}

static uint32_t a64_movz32(unsigned rd, uint16_t imm) {
    return 0x52800000u | (static_cast<uint32_t>(imm) << 5) | rd;
}

static uint32_t a64_mov_xzr(unsigned rd) {
    return 0xAA1F03E0u | rd;
}

static uint32_t a64_str_w_imm0(unsigned rt, unsigned rn) {
    return 0xB9000000u | (rn << 5) | rt;
}

static uint32_t a64_br(unsigned rn) {
    return 0xD61F0000u | (rn << 5);
}

static void emit_load_u64(uint32_t* code, uint32_t* idx, unsigned rd, uint64_t value) {
    code[(*idx)++] = a64_movz64(rd, static_cast<uint16_t>(value & 0xFFFFu), 0);
    code[(*idx)++] = a64_movk64(rd, static_cast<uint16_t>((value >> 16) & 0xFFFFu), 16);
    code[(*idx)++] = a64_movk64(rd, static_cast<uint16_t>((value >> 32) & 0xFFFFu), 32);
    code[(*idx)++] = a64_movk64(rd, static_cast<uint16_t>((value >> 48) & 0xFFFFu), 48);
}

static bool install_linux_trampoline(CapHandle memory,
                                     const dango::VmLayout& layout,
                                     uint64_t trampoline_pa,
                                     uint64_t linux_entry,
                                     uint64_t dtb_pa) {
    uint32_t code[96]{};
    uint32_t idx = 0;

    // Write a deterministic marker to the guest serial before entering Linux.
    // The host VMM traps guest PL011 MMIO at 0x09000000 and forwards it to the
    // second QEMU serial backend, so this bypasses Linux console setup entirely.
    emit_load_u64(code, &idx, 4, 0x09000000ull);
    const char msg[] = "dango: trampoline reached\n";
    for (uint32_t i = 0; i + 1 < sizeof(msg); ++i) {
        code[idx++] = a64_movz32(5, static_cast<uint8_t>(msg[i]));
        code[idx++] = a64_str_w_imm0(5, 4);
    }

    // Linux arm64 boot ABI: x0 = FDT physical address, x1-x3 = 0.
    emit_load_u64(code, &idx, 0, dtb_pa);
    code[idx++] = a64_mov_xzr(1);
    code[idx++] = a64_mov_xzr(2);
    code[idx++] = a64_mov_xzr(3);
    emit_load_u64(code, &idx, 4, linux_entry);
    code[idx++] = a64_br(4);

    if (!contains(layout.ram_base, layout.ram_size, trampoline_pa, idx * sizeof(uint32_t))) {
        return false;
    }
    return sys_vm_memory_write(memory,
                               guest_offset(layout, trampoline_pa),
                               code,
                               idx * sizeof(uint32_t)) == 0;
}

// --- Phase H/I: bootstrap GPU client (`drv.gpu.linux`) ----------------------
//
// The manager is the host half of the Midix GPU control plane: it owns the
// shared staging window, drives the bootstrap opcodes over the Dango channel
// rings, and renders the frame the appliance scans out. Every wire structure
// and opcode comes from midix/abi/protocols/gpu.h -- nothing is restated here.

// Bootstrap scanout geometry: one full XRGB8888 frame in the staging window.
// The mode and the window size are a single fact (the shared Dango binding), so
// this does not restate them.
constexpr uint32_t kGpuScanoutWidth = dango::kGpuModeWidth;
constexpr uint32_t kGpuScanoutHeight = dango::kGpuModeHeight;
constexpr uint32_t kGpuBytesPerPixel = dango::kGpuModeBytesPerPixel;
constexpr uint32_t kGpuScanoutStride = kGpuScanoutWidth * kGpuBytesPerPixel;
static_assert(static_cast<uint64_t>(kGpuScanoutStride) * kGpuScanoutHeight == dango::kStagingSize,
              "the bootstrap frame must fill the staging window exactly");

// XRGB8888 bar colours, left to right: red, green, blue, white. Byte order is
// chosen by the fill below. The Phase F kms_test draws the same bars, so one
// host capture assertion covers both the DRM and the protocol path.
constexpr uint8_t kGpuBarRgb[][3] = {
    {255, 0, 0},
    {0, 255, 0},
    {0, 0, 255},
    {255, 255, 255},
};
constexpr uint32_t kGpuBarCount = sizeof(kGpuBarRgb) / sizeof(kGpuBarRgb[0]);

// The acceptance markers below are literal strings, so pin the values they
// advertise to the same constants the client negotiates and renders with.
static_assert(MX_GPU_VERSION_MAJOR == 1 && MX_GPU_VERSION_MINOR == 0,
              "the 'gpu control: hello major=1 minor=0' marker must match the ABI version");
static_assert(kGpuScanoutWidth == 1024 && kGpuScanoutHeight == 768,
              "the 'gpu control: mode 1024x768' marker must match the bootstrap mode");
// The frozen staging window is exactly one bootstrap frame; dango/layout.h
// keeps it disjoint from BAR4, the MMIO window and guest RAM.
static_assert(static_cast<uint64_t>(kGpuScanoutStride) * kGpuScanoutHeight ==
                  dango::kStagingSize,
              "the shared staging window must hold exactly one bootstrap frame");
static_assert(kGpuScanoutWidth % kGpuBarCount == 0, "the bar pattern must tile the scanout");

// One bootstrap request/reply. `ring_try_push` stamps the slot with the
// producer's ring index, which is the correlation token the appliance echoes
// back (protocols/gpu.h: "`seq` is the correlation field"), so the client reads
// it here and requires the reply to carry it. `request_seq` is written before
// the push because the caller (GPU_PRESENT) also matches the later asynchronous
// completion against the same token.
static bool gpu_request(RingPage* tx, RingPage* rx, CapHandle h2g_db, CapHandle g2h_db,
                        CapHandle channel, uint32_t opcode, const void* request,
                        uint32_t length, uint64_t* request_seq, RingSlot* reply) {
    if (request_seq) *request_seq = __atomic_load_n(&tx->head.v, __ATOMIC_RELAXED);
    if (!ring_try_push(tx, DANGO_CH_DEFAULT_RING_ORDER, opcode, request, length, h2g_db)) {
        return false;
    }
    if (sys_vm_channel_notify(channel) != 0) return false;
    if (!ring_wait_pop(rx, DANGO_CH_DEFAULT_RING_ORDER, reply, g2h_db)) return false;
    return reply->type == opcode && (!request_seq || reply->seq == *request_seq);
}

// Wait for an unsolicited frame of a given opcode (the asynchronous completion).
static bool gpu_wait(RingPage* rx, CapHandle g2h_db, uint32_t opcode, RingSlot* reply) {
    return ring_wait_pop(rx, DANGO_CH_DEFAULT_RING_ORDER, reply, g2h_db) &&
           reply->type == opcode;
}

// Decode one reply payload. The payload is 8-byte aligned inside the slot (it
// starts at offset 16 of a 64-byte slot) and every MxGpu* struct is at most the
// 48-byte inline budget, so the struct can be read in place.
template <typename T>
static bool gpu_reply(const RingSlot& slot, T* out) {
    static_assert(sizeof(T) <= 48, "GPU reply must fit one ring payload");
    if (slot.length < sizeof(T)) return false;
    *out = *reinterpret_cast<const T*>(slot.payload);
    return true;
}

// Fill the staging window with the bootstrap bar pattern, one scanout row per
// syscall. The row lives in .bss: a service thread gets a small stack, and the
// pixel layout is then computed once instead of per row.
static bool gpu_fill_bars(CapHandle staging) {
    static uint8_t row[kGpuScanoutStride];
    const uint32_t bar_pixels = kGpuScanoutWidth / kGpuBarCount;
    for (uint32_t bar = 0; bar < kGpuBarCount; ++bar) {
        for (uint32_t x = 0; x < bar_pixels; ++x) {
            uint8_t* px = row + (bar * bar_pixels + x) * kGpuBytesPerPixel;
            // XRGB8888 is little-endian in memory: B, G, R, then the unused X.
            px[0] = kGpuBarRgb[bar][2];
            px[1] = kGpuBarRgb[bar][1];
            px[2] = kGpuBarRgb[bar][0];
            px[3] = 0;
        }
    }
    for (uint32_t y = 0; y < kGpuScanoutHeight; ++y) {
        if (sys_vm_memory_write(staging, static_cast<uint64_t>(y) * kGpuScanoutStride,
                                row, kGpuScanoutStride) != 0) {
            return false;
        }
    }
    return true;
}

// Create, publish and fill the shared staging window. It runs before the vCPU
// is resumed, so the channel descriptor always describes the window by the time
// the guest can open the channel (the same ordering discipline the GPU
// assignment above follows). The window is declared to the channel *before* the
// stage-2 mapping exists: vm::stage_channel() validates the IPA against free
// stage-2 space, so declaring it after sys_vm_memory_map() would be rejected as
// an overlap with the mapping it just created.
static void gpu_staging_publish(CapHandle vm, CapHandle channel) {
    const CapHandle staging = as_cap(
        sys_vm_memory_create(vm, dango::kStagingSize, MX_VM_MEMORY_FLAG_SHARED_STAGING));
    if (staging == kNullCap) fail("gpu client FAILED: staging create\n");
    if (sys_vm_channel_stage(channel, dango::kStagingGuestIpa, dango::kStagingSize) != 0) {
        fail("gpu client FAILED: staging declare\n");
    }
    if (sys_vm_memory_map(vm, staging, dango::kStagingGuestIpa, dango::kStagingSize,
                          dango::RIGHT_READ | dango::RIGHT_WRITE) != 0) {
        fail("gpu client FAILED: staging map\n");
    }
    if (!gpu_fill_bars(staging)) fail("gpu client FAILED: staging fill\n");
    dango_counter("gpu.staging.ipa", dango::kStagingGuestIpa);
    dango_counter("gpu.staging.size", dango::kStagingSize);
}

// Drive GPU_HELLO -> ... -> GPU_PRESENT_COMPLETE. Each marker is printed only
// after the stage it names succeeded, so a partial exchange never looks green.
static void gpu_bootstrap_client(CapHandle channel, RingPage* tx, RingPage* rx,
                                 CapHandle h2g_db, CapHandle g2h_db) {
    RingSlot slot{};
    // The pushed slot's seq, i.e. the token every reply must echo. GPU_PRESENT
    // leaves it set for the asynchronous completion match below.
    uint64_t seq = 0;

    // 1. GPU_HELLO: negotiate the bootstrap version and require the staging
    //    buffer path this client depends on.
    MxGpuHello hello{};
    hello.major = MX_GPU_VERSION_MAJOR;
    hello.minor = MX_GPU_VERSION_MINOR;
    hello.flags = MX_GPU_HELLO_FLAG_NONE;
    hello.feature_bits = 0;
    hello.boot_generation = 0; // unknown until the appliance answers
    MxGpuHelloReply hello_reply{};
    if (!gpu_request(tx, rx, h2g_db, g2h_db, channel, GPU_HELLO, &hello, sizeof(hello), &seq,
                     &slot) ||
        !gpu_reply(slot, &hello_reply) || hello_reply.status != MX_OK) {
        fail("gpu client FAILED: hello\n");
    }
    if (hello_reply.major != MX_GPU_VERSION_MAJOR ||
        hello_reply.minor != MX_GPU_VERSION_MINOR ||
        (hello_reply.feature_bits & MX_GPU_FEATURE_STAGING_BUFFER) == 0) {
        dango_counter("gpu.hello.major", hello_reply.major);
        dango_counter("gpu.hello.minor", hello_reply.minor);
        fail("gpu client FAILED: hello rejected\n");
    }
    dango_counter("gpu.boot_generation", hello_reply.boot_generation);
    console_log("gpu control: hello major=1 minor=0\n");

    // 2. GPU_ENUM_OUTPUTS: the bootstrap appliance exposes exactly one output.
    //    Device-object ids are minted by the appliance, so the first
    //    device-level enumeration goes out with the null id.
    MxGpuEnumOutputsRequest outputs{};
    outputs.device_id = 0;
    outputs.boot_generation = hello_reply.boot_generation;
    outputs.index = 0;
    outputs.flags = 0;
    MxGpuEnumOutputsReply outputs_reply{};
    if (!gpu_request(tx, rx, h2g_db, g2h_db, channel, GPU_ENUM_OUTPUTS, &outputs,
                     sizeof(outputs), &seq, &slot) ||
        !gpu_reply(slot, &outputs_reply) || outputs_reply.status != MX_OK ||
        outputs_reply.output_count != 1) {
        dango_counter("gpu.outputs.count", outputs_reply.output_count);
        fail("gpu client FAILED: outputs\n");
    }
    console_log("gpu control: outputs=1\n");

    // 3. GPU_GET_MODES: the single bootstrap mode the host row asserts.
    MxGpuGetModesRequest modes{};
    modes.output_id = outputs_reply.output_id;
    modes.boot_generation = hello_reply.boot_generation;
    modes.index = 0;
    modes.flags = 0;
    MxGpuGetModesReply modes_reply{};
    if (!gpu_request(tx, rx, h2g_db, g2h_db, channel, GPU_GET_MODES, &modes,
                     sizeof(modes), &seq, &slot) ||
        !gpu_reply(slot, &modes_reply) || modes_reply.status != MX_OK) {
        fail("gpu client FAILED: modes\n");
    }
    if (modes_reply.width != kGpuScanoutWidth || modes_reply.height != kGpuScanoutHeight ||
        modes_reply.pixel_format != MX_GPU_PIXFMT_XRGB8888) {
        dango_counter("gpu.mode.width", modes_reply.width);
        dango_counter("gpu.mode.height", modes_reply.height);
        fail("gpu client FAILED: modes rejected\n");
    }
    console_log("gpu control: mode 1024x768\n");

    // 4. GPU_BUFFER_REGISTER: hand the appliance the staged scanout buffer.
    //    `shm_id` is the buffer's byte offset within the shared staging window
    //    (0 = the whole frame just rendered at the window start), so the
    //    registered buffer *is* what gpu_fill_bars() wrote.
    MxGpuBufferRegisterRequest buffer{};
    buffer.shm_id = 0;
    buffer.boot_generation = hello_reply.boot_generation;
    buffer.width = kGpuScanoutWidth;
    buffer.height = kGpuScanoutHeight;
    buffer.stride = kGpuScanoutStride;
    buffer.pixel_format = MX_GPU_PIXFMT_XRGB8888;
    buffer.flags = MX_GPU_BUFFER_FLAG_STAGING;
    buffer.reserved0 = 0;
    MxGpuBufferRegisterReply buffer_reply{};
    if (!gpu_request(tx, rx, h2g_db, g2h_db, channel, GPU_BUFFER_REGISTER, &buffer,
                     sizeof(buffer), &seq, &slot) ||
        !gpu_reply(slot, &buffer_reply) || buffer_reply.status != MX_OK ||
        buffer_reply.buffer_id == 0) {
        fail("gpu client FAILED: buffer register\n");
    }
    console_log("gpu staging: registered\n");

    // 5. GPU_SET_MODE: bind output + mode + buffer into one framebuffer.
    MxGpuSetModeRequest set_mode{};
    set_mode.output_id = outputs_reply.output_id;
    set_mode.mode_id = modes_reply.mode_id;
    set_mode.buffer_id = buffer_reply.buffer_id;
    set_mode.boot_generation = buffer_reply.boot_generation;
    set_mode.flags = 0;
    set_mode.reserved0 = 0;
    MxGpuSetModeReply set_mode_reply{};
    if (!gpu_request(tx, rx, h2g_db, g2h_db, channel, GPU_SET_MODE, &set_mode,
                     sizeof(set_mode), &seq, &slot) ||
        !gpu_reply(slot, &set_mode_reply) || set_mode_reply.status != MX_OK) {
        fail("gpu client FAILED: set mode\n");
    }

    // 6. GPU_PRESENT: enqueue the whole frame, then wait for the asynchronous
    //    completion that says it reached the scanout.
    MxGpuPresentRequest present{};
    present.buffer_id = buffer_reply.buffer_id;
    present.framebuffer_id = set_mode_reply.framebuffer_id;
    present.boot_generation = set_mode_reply.boot_generation;
    present.flags = MX_GPU_PRESENT_FLAG_FULL_FRAME;
    present.damage_count = 0; // whole framebuffer; no damage list
    MxGpuPresentReply present_reply{};
    if (!gpu_request(tx, rx, h2g_db, g2h_db, channel, GPU_PRESENT, &present,
                     sizeof(present), &seq, &slot) ||
        !gpu_reply(slot, &present_reply) || present_reply.status != MX_OK) {
        fail("gpu client FAILED: present enqueue\n");
    }
    // The completion notification is a separate, unsolicited frame: it carries
    // no request of its own, so its `seq` is the GPU_PRESENT token it completes.
    MxGpuPresentComplete complete{};
    if (!gpu_wait(rx, g2h_db, GPU_PRESENT_COMPLETE, &slot) || slot.seq != seq ||
        !gpu_reply(slot, &complete) || complete.status != MX_OK ||
        complete.buffer_id != buffer_reply.buffer_id) {
        dango_counter("gpu.present.status", status_magnitude(complete.status));
        fail("gpu client FAILED: present complete\n");
    }
    dango_counter("gpu.buffer.id", buffer_reply.buffer_id);
    dango_counter("gpu.framebuffer.id", set_mode_reply.framebuffer_id);
    console_log("gpu present: complete\n");
}

static void runner_entry(uint64_t vcpu_arg) {
    const CapHandle vcpu = static_cast<CapHandle>(vcpu_arg);
    dango_log("vm runner thread entered\n");
    bool logged_first_return = false;
    bool logged_hello = false;
    while (true) {
        dango::VcpuRunBudget budget{};
        budget.budget_ns = kLinuxRunBudgetNs;
        budget.deadline_ns = 0;
        budget.max_exits = kLinuxRunMaxExits;

        if (!logged_first_return) {
            dango_log("vm runner entering first vcpu_run\n");
        }
        dango::VcpuRunResult result{};
        const int64_t rc = sys_vcpu_run(vcpu, &budget, &result);
        if (!logged_first_return) {
            dango_log("vm runner first vcpu_run returned\n");
            dango_counter("vcpu.first_rc", status_magnitude(rc));
            dango_counter("vcpu.first_exit_reason", result.exit_reason);
            dango_counter("vcpu.first_last_esr", result.last_esr);
            logged_first_return = true;
        }
        if (rc == dango::E_NOTIMPL) {
            dango_log("vm runner stopped: vcpu_run not implemented\n");
            dango_counter("vcpu.exit_reason", result.exit_reason);
            dango_counter("vcpu.last_esr", result.last_esr);
            break;
        }
        if (rc < 0) {
            dango_log("vm runner stopped: vcpu_run syscall error\n");
            dango_counter("vcpu.rc", status_magnitude(rc));
            dango_counter("vcpu.exit_reason", result.exit_reason);
            dango_counter("vcpu.last_esr", result.last_esr);
            dango::VcpuInfo info{};
            if (sys_vcpu_inspect(vcpu, &info, sizeof(info)) == 0) {
                dango_counter("vcpu.state", info.state);
                dango_counter("vcpu.run_count", info.run_count);
                dango_counter("vcpu.last_exit_reason", info.last_exit_reason);
            }
            break;
        }
        if (result.exit_reason == dango::VCPU_EXIT_UNSUPPORTED_TRAP) {
            dango_log("vm runner stopped: unsupported guest trap\n");
            dango_counter("vcpu.last_esr", result.last_esr);
            break;
        }
        if (result.exit_reason == dango::VCPU_EXIT_SHUTDOWN) {
            dango_log("vm runner stopped: guest shutdown\n");
            break;
        }
        if (!logged_hello && sys_debug_counter(dango::VM_DEBUG_GUEST_LINUX_HELLO) > 0) {
            logged_hello = true;
            dango_log("linux guest responded\n");
        }
        // Spec §7/§8: sustained guest execution stays budgeted.  The runner
        // holds the vCPU on a budget/period sched context, so it must return to
        // the scheduler between runs; otherwise it monopolises a CPU and starves
        // the native services.  Yield once per budget window.
        sys_yield();
    }
    sys_thread_exit(0);
}

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    const CapHandle kConsoleRing = mochi::boot::require_handle(boot, "log.ring");
    const CapHandle kConsoleDoorbell = mochi::boot::require_handle(boot, "log.doorbell");
    const CapHandle kVmRoot = mochi::boot::require_handle(boot, "vm.root");
    const CapHandle kDoneDoorbell = mochi::boot::require_handle(boot, "dango.done.db");
    const CapHandle kAuthGateDoorbell = mochi::boot::require_handle(boot, "dango.auth_gate.db");
    const CapHandle kLayoutDoneDoorbell = mochi::boot::require_handle(boot, "dango.layout_done.db");
    const CapHandle kBootInitrd = mochi::boot::require_handle(boot, "boot.initrd");

    auto* log_ring = wait_map<RingPage>(kConsoleRing, dango::CAP_SHARED_RING,
                                        kVaConsoleRing,
                                        dango::RIGHT_READ | dango::RIGHT_WRITE);
    wait_cap(kConsoleDoorbell, dango::CAP_DOORBELL);
    mochi::log_set_sink(log_ring, DANGO_CH_DEFAULT_RING_ORDER, kConsoleDoorbell, "dango.manager");
    wait_cap(kVmRoot, dango::CAP_VM_ROOT);
    wait_cap(kDoneDoorbell, dango::CAP_DOORBELL);
    wait_cap(kAuthGateDoorbell, dango::CAP_DOORBELL);
    wait_cap(kLayoutDoneDoorbell, dango::CAP_DOORBELL);
    const uint8_t* boot_archive = wait_map<uint8_t>(kBootInitrd, dango::CAP_PAGE,
                                                    kVaBootInitrd,
                                                    dango::RIGHT_READ);

    const uint64_t auth_gate_base = sys_doorbell_read(kAuthGateDoorbell);

    dango_log("manager starting\n");
    LinuxArtifacts linux{};
    const bool have_linux = discover_linux_artifacts(boot_archive, &linux);
    if (have_linux) {
        dango_log("linux artifacts ready\n");
        dango_counter("linux.entry", linux.entry);
        dango_counter("linux.kernel_load", linux.layout.kernel_load);
    }

    const int64_t el = sys_debug_counter(dango::VM_DEBUG_EL_CURRENT);
    const int64_t el2 = sys_debug_counter(dango::VM_DEBUG_EL2_AVAILABLE);
    const int64_t vhe = sys_debug_counter(dango::VM_DEBUG_VHE_AVAILABLE);
    if (el < 0 || el > 3 || el2 < 0 || vhe < 0) fail("el2 detection failed\n");
    dango_counter("vm.current_el", static_cast<uint64_t>(el));
    dango_counter("vm.el2_available", static_cast<uint64_t>(el2));
    if (el2 == 0) fail("el2 unavailable: dango VM cannot run guest\n");
    dango_debug("el2 available\n");

    const dango::VmLayout layout = have_linux ? linux.layout : dango::v3_layout();
    if (!validate_layout(layout)) fail("layout validation failed\n");
    const uint64_t active_ram_size = have_linux ? layout.ram_size : (16ull * 1024ull * 1024ull);

    dango_debug("creating vm\n");
    const int64_t vm_rc = sys_vm_create(kVmRoot, 0);
    if (vm_rc < 0) fail("vm create syscall failed\n");
    const CapHandle vm = as_cap(vm_rc);
    if (vm == kNullCap) fail("vm object invalid\n");
    dango_log("vm created\n");

    dango_debug("inspecting vm\n");
    dango::VmInfo vm_info{};
    if (sys_vm_inspect(vm, &vm_info, sizeof(vm_info)) != 0 ||
        vm_info.state != dango::VM_CREATED) {
        fail("vm object invalid\n");
    }
    dango_counter("vm.id", vm_info.vm_id);
    dango_debug("vm object ready\n");

    dango_debug("creating guest memory\n");
    const CapHandle memory = as_cap(sys_vm_memory_create(vm, active_ram_size, 0));
    if (memory == kNullCap) fail("guest memory failed\n");
    dango_debug("guest memory created\n");

    const uint32_t guest_rights = dango::RIGHT_READ | dango::RIGHT_WRITE | dango::RIGHT_EXEC;
    dango_debug("mapping guest memory\n");
    if (sys_vm_memory_map(vm, memory, layout.ram_base, active_ram_size, guest_rights) != 0) {
        fail("guest memory failed\n");
    }
    dango_debug("guest memory mapped\n");

    dango_debug("inspecting guest memory\n");
    dango::VmMemoryInfo mem_info{};
    if (sys_vm_memory_inspect(memory, &mem_info, sizeof(mem_info)) != 0 ||
        mem_info.guest_pa_base != layout.ram_base ||
        mem_info.mapped_size != active_ram_size ||
        mem_info.mapped_stage2 == 0) {
        fail("guest memory failed\n");
    }
    dango_debug("guest memory ready\n");

    dango_debug("inspecting stage2\n");
    if (sys_vm_inspect(vm, &vm_info, sizeof(vm_info)) != 0 ||
        vm_info.stage2_active != 0 ||
        vm_info.stage2_built == 0 ||
        vm_info.stage2_mapped_pages != (active_ram_size / dango::kPageSize)) {
        fail("stage2 invalid\n");
    }
    dango_debug("stage2 ready\n");

    if (have_linux) {
        if (!contains(layout.ram_base, layout.ram_size, layout.kernel_load, linux.image_size) ||
            !contains(layout.ram_base, layout.ram_size, layout.initramfs_load, linux.initramfs_size) ||
            !contains(layout.ram_base, layout.ram_size, layout.dtb_base, linux.dtb_size) ||
            linux.dtb_size > layout.dtb_size ||
            !contains(layout.ram_base, layout.ram_size, linux.entry, 4) ||
            overlaps(layout.kernel_load, linux.image_size,
                     layout.initramfs_load, linux.initramfs_size) ||
            overlaps(layout.kernel_load, linux.image_size,
                     layout.dtb_base, linux.dtb_size) ||
            overlaps(layout.initramfs_load, linux.initramfs_size,
                     layout.dtb_base, linux.dtb_size)) {
            fail("linux load failed\n");
        }
        dango_debug("copying linux image\n");
        if (sys_vm_memory_write(memory,
                                guest_offset(layout, layout.kernel_load),
                                linux.image,
                                linux.image_size) != 0) {
            fail("linux load failed\n");
        }
        dango_debug("linux image copied\n");

        dango_debug("copying linux initramfs\n");
        if (sys_vm_memory_write(memory,
                                guest_offset(layout, layout.initramfs_load),
                                linux.initramfs,
                                linux.initramfs_size) != 0) {
            fail("linux load failed\n");
        }
        dango_debug("linux initramfs copied\n");

        dango_debug("copying linux dtb\n");
        if (sys_vm_memory_write(memory,
                                guest_offset(layout, layout.dtb_base),
                                linux.dtb,
                                linux.dtb_size) != 0) {
            fail("linux load failed\n");
        }
        dango_debug("linux dtb copied\n");

        dango_log("linux image loaded\n");
    }

    dango_debug("creating vcpu\n");
    const CapHandle vcpu = as_cap(sys_vcpu_create(vm, 0));
    if (vcpu == kNullCap) fail("vcpu object invalid\n");
    dango_debug("vcpu created\n");

    uint64_t entry_pc = have_linux ? linux.entry : 0x4008'0000ull;
    if (have_linux) {
        const uint64_t trampoline_pa = layout.ram_base + 0x1000ull;
        if (!install_linux_trampoline(memory, layout, trampoline_pa, linux.entry, layout.dtb_base)) {
            fail("linux trampoline install failed\n");
        }
        entry_pc = trampoline_pa;
        dango_counter("linux.trampoline", trampoline_pa);
    }
    dango_debug("setting vcpu entry\n");
    if (sys_vcpu_set_entry(vcpu, entry_pc, layout.dtb_base, 0) != 0) {
        fail("vcpu object invalid\n");
    }
    dango_debug("vcpu entry set\n");

    dango_debug("inspecting vcpu\n");
    dango::VcpuInfo vcpu_info{};
    if (sys_vcpu_inspect(vcpu, &vcpu_info, sizeof(vcpu_info)) != 0 ||
        (vcpu_info.state != dango::VCPU_CONFIGURED &&
         vcpu_info.state != dango::VCPU_READY) ||
        vcpu_info.entry_pc != entry_pc) {
        fail("vcpu object invalid\n");
    }
    dango_counter("vcpu.id", vcpu_info.vcpu_id);
    dango_counter("vcpu.owner_vm_id", vcpu_info.owner_vm_id);
    dango_debug("vcpu ready\n");

    // Phase E: build the virtio-gpu appliance assignment when Dango was granted
    // the GPU function authority (spec §6.1/§8.11) and the guest carries the GPU
    // PCI description. The order is the secure-attach contract: assign -> map
    // BARs -> route INTx -> expose ECAM, all before the vCPU is resumed.
    const CapHandle gpu_function = mochi::boot::handle(boot, "gpu.function");
    const CapHandle gpu_irq = mochi::boot::handle(boot, "gpu.irq");
    if (have_linux && linux.gpu_profile) {
        if (!gpu_function || !gpu_irq) fail("gpu authority missing\n");
        // Opt in to host BAR assignment: direct -kernel boot has no firmware, so
        // the function's BARs read as 0 until we program them from the PCIe MMIO
        // window (spec §6.2.5.1).
        const CapHandle assign =
            as_cap(sys_vm_assign_device(vm, gpu_function, dango::ASSIGN_FLAG_ASSIGN_BARS));
        if (assign == kNullCap) fail("gpu assign failed\n");

        // Map BAR4 (the virtio modern common/notify/ISR window) into the DTB MMIO
        // window at an IPA aligned to the BAR size. BAR1 is deliberately NOT
        // mapped: it carries the MSI-X table/PBA, which the assignment layer
        // refuses to expose (the appliance uses INTx, not MSI-X).
        const uint32_t bar_rights = dango::RIGHT_READ | dango::RIGHT_WRITE;
        // A BAR's guest IPA must be aligned to its aperture size (the kernel
        // rejects otherwise); BAR4 is 16 KiB.  Assert it at compile time.
        static_assert((dango::kGpuBar4Ipa % dango::kGpuBar4Size) == 0,
                      "BAR4 guest IPA must be aligned to its aperture size");
        const int64_t bar4_rc = sys_vm_assign_map_mmio(assign, 4, dango::kGpuBar4Ipa,
                                                       dango::kGpuBar4Size, bar_rights, 0);
        if (bar4_rc != 0) {
            dango_counter("gpu.bar4_rc", static_cast<uint64_t>(bar4_rc));
            fail("gpu bar map failed\n");
        }
        // PCI INTx -> guest SPI (dango-linux-gpu.dts.in interrupt-map).
        if (sys_vm_assign_route_irq(assign, gpu_irq, vcpu, dango::kGpuIntxIrq, 0) != 0) {
            fail("gpu irq route failed\n");
        }
        // Synthetic ECAM aperture (1 MiB); reads/writes trap through the
        // stage-2 handler in kernel/pci/ecam.cc.
        if (sys_vm_assign_expose_ecam(assign, dango::kGpuEcamBase, dango::kGpuEcamSize) != 0) {
            fail("gpu ecam expose failed\n");
        }
        dango_log("gpu appliance ready\n");
    }

    dango::SchedContext vcpu_sched{};
    vcpu_sched.band = kLinuxRunnerSchedBand;
    vcpu_sched.weight = 1024;
    vcpu_sched.budget_ns = kLinuxRunBudgetNs;
    vcpu_sched.period_ns = kLinuxRunPeriodNs;
    vcpu_sched.deadline_ns = 0;
    const auto* cpu_count = mochi::boot::find(boot, "cpu.count");
    const bool single_cpu = cpu_count && cpu_count->aux0 == 1;
    vcpu_sched.core_mask = single_cpu ? 1 : kLinuxRunnerCoreMask;
    vcpu_sched.home_cpu = single_cpu ? 0 : kLinuxRunnerHomeCpu;
    vcpu_sched.flags = dango::SCHED_CTX_VM_RUNNER | dango::SCHED_CTX_NO_STEAL;
    const CapHandle vcpu_sched_ctx = as_cap(sys_sched_context_create(&vcpu_sched, sizeof(vcpu_sched)));
    if (vcpu_sched_ctx == kNullCap) {
        dango_counter("vcpu.sched_create_failed", 1);
        fail("vcpu scheduler context create failed\n");
    }
    const int64_t vcpu_sched_bind_rc = sys_vcpu_bind_sched_context(vcpu, vcpu_sched_ctx);
    if (vcpu_sched_bind_rc != 0) {
        dango_counter("vcpu.sched_bind_rc",
                      static_cast<uint64_t>(vcpu_sched_bind_rc < 0 ? -vcpu_sched_bind_rc : vcpu_sched_bind_rc));
        fail("vcpu scheduler bind failed\n");
    }
    const int64_t vcpu_core_mask_rc =
        sys_vcpu_set_core_mask(vcpu, vcpu_sched.core_mask, vcpu_sched.home_cpu);
    if (vcpu_core_mask_rc != 0) {
        dango_counter("vcpu.core_mask_rc",
                      static_cast<uint64_t>(vcpu_core_mask_rc < 0 ? -vcpu_core_mask_rc : vcpu_core_mask_rc));
        fail("vcpu scheduler core mask failed\n");
    }

    if (!have_linux) {
        dango_log("vcpu stub run skipped: no linux guest payload\n");
    }

    const CapHandle channel = as_cap(sys_vm_channel_create(vm, DANGO_CH_DEFAULT_RING_ORDER, 0));
    if (channel == kNullCap) fail("channel setup failed\n");
    const CapHandle h2g_ring = as_cap(sys_vm_channel_endpoint(
        channel, dango::VM_CHANNEL_HOST_TO_GUEST_RING,
        dango::RIGHT_READ | dango::RIGHT_WRITE | dango::RIGHT_MAP | dango::RIGHT_DUP));
    const CapHandle g2h_ring = as_cap(sys_vm_channel_endpoint(
        channel, dango::VM_CHANNEL_GUEST_TO_HOST_RING,
        dango::RIGHT_READ | dango::RIGHT_WRITE | dango::RIGHT_MAP | dango::RIGHT_DUP));
    const CapHandle h2g_db = as_cap(sys_vm_channel_endpoint(
        channel, dango::VM_CHANNEL_HOST_TO_GUEST_DOORBELL,
        dango::RIGHT_NOTIFY | dango::RIGHT_WAIT | dango::RIGHT_DUP));
    const CapHandle g2h_db = as_cap(sys_vm_channel_endpoint(
        channel, dango::VM_CHANNEL_GUEST_TO_HOST_DOORBELL,
        dango::RIGHT_NOTIFY | dango::RIGHT_WAIT | dango::RIGHT_DUP));
    if (h2g_ring == kNullCap || g2h_ring == kNullCap ||
        h2g_db == kNullCap || g2h_db == kNullCap ||
        sys_cap_identify(h2g_ring) != dango::CAP_SHARED_RING ||
        sys_cap_identify(g2h_ring) != dango::CAP_SHARED_RING ||
        sys_cap_identify(h2g_db) != dango::CAP_DOORBELL ||
        sys_cap_identify(g2h_db) != dango::CAP_DOORBELL) {
        fail("channel setup failed\n");
    }
    dango::VmChannelInfo channel_info{};
    if (sys_vm_channel_inspect(channel, &channel_info, sizeof(channel_info)) != 0 ||
        channel_info.endpoint_present_mask != 0xFull) {
        fail("channel setup failed\n");
    }
    RingPage* channel_tx = nullptr;
    RingPage* channel_rx = nullptr;
    if (have_linux && linux.gpu_profile) {
        if (sys_vm_channel_map(channel, vcpu, layout.channel_base, layout.channel_size,
                               dango::kChannelGuestIrq) != 0)
            fail("channel guest mapping failed\n");
        if (sys_map_page(h2g_ring, dango::kChannelHostTxVa,
                         dango::RIGHT_READ | dango::RIGHT_WRITE) != 0 ||
            sys_map_page(g2h_ring, dango::kChannelHostRxVa,
                         dango::RIGHT_READ | dango::RIGHT_WRITE) != 0)
            fail("channel host mapping failed\n");
        channel_tx = reinterpret_cast<RingPage*>(dango::kChannelHostTxVa);
        channel_rx = reinterpret_cast<RingPage*>(dango::kChannelHostRxVa);
        // Exercise both host endpoints, including wraparound, before Linux can
        // access them. Leave equal, nonzero counters for the guest to discover.
        for (uint64_t n = 0; n < 256; ++n) {
            RingSlot msg{};
            if (!ring_try_push(channel_tx, DANGO_CH_DEFAULT_RING_ORDER, DANGO_CH_TEST_PING, &n,
                               sizeof(n), h2g_db) ||
                !ring_pop(channel_tx, DANGO_CH_DEFAULT_RING_ORDER, &msg) || msg.seq != n ||
                msg.length != sizeof(n) ||
                *reinterpret_cast<const uint64_t*>(msg.payload) != n ||
                !ring_try_push(channel_rx, DANGO_CH_DEFAULT_RING_ORDER, DANGO_CH_TEST_PONG, &n,
                               sizeof(n), g2h_db) ||
                !ring_pop(channel_rx, DANGO_CH_DEFAULT_RING_ORDER, &msg) || msg.seq != n ||
                msg.length != sizeof(n) ||
                *reinterpret_cast<const uint64_t*>(msg.payload) != n)
                fail("channel native probe FAILED\n");
        }
        console_log("channel native probe passed\n");
        // Phase H/I: publish and fill the shared staging window while the guest
        // is still stopped, so the channel descriptor describes it the moment
        // the guest can open the channel.
        gpu_staging_publish(vm, channel);
    }
    console_log("channel ready\n");
    console_log("layout ready\n");
    mochi::boot::ready(boot);

    if (sys_notify(kLayoutDoneDoorbell) != 0) fail("service failed\n");
    if (sys_wait_doorbell(kAuthGateDoorbell, auth_gate_base) != 0) {
        fail("service failed\n");
    }

    if (have_linux) {
        dango_log("starting linux guest\n");
        const CapHandle runner_th = as_cap(
            sys_thread_create(kNullCap,
                              reinterpret_cast<uint64_t>(runner_entry),
                              static_cast<uint64_t>(vcpu),
                              MX_THREAD_SUSPENDED));
        if (runner_th != kNullCap) {
            dango_counter("vm.runner.band", static_cast<uint64_t>(vcpu_sched.band));
            dango_counter("vm.runner.core_mask", vcpu_sched.core_mask);
            dango_counter("vm.runner.home_cpu", vcpu_sched.home_cpu);
            const int64_t runner_affinity_rc = sys_thread_set_affinity(runner_th, vcpu_sched.core_mask);
            if (runner_affinity_rc != 0) {
                dango_counter("vm.runner.affinity_rc",
                              static_cast<uint64_t>(runner_affinity_rc < 0 ? -runner_affinity_rc : runner_affinity_rc));
                fail("vcpu runner affinity failed\n");
            }
            const int64_t runner_bind_rc = sys_sched_context_bind_thread(runner_th, vcpu_sched_ctx);
            if (runner_bind_rc != 0) {
                dango_counter("vm.runner.sched_bind_rc",
                              static_cast<uint64_t>(runner_bind_rc < 0 ? -runner_bind_rc : runner_bind_rc));
                fail("vcpu runner scheduler bind failed\n");
            }
            const int64_t runner_resume_rc = sys_thread_resume(runner_th);
            if (runner_resume_rc != 0) {
                dango_counter("vm.runner.resume_rc",
                              static_cast<uint64_t>(runner_resume_rc < 0 ? -runner_resume_rc : runner_resume_rc));
                fail("vcpu runner resume failed\n");
            }
            dango_log("vm runner started async\n");
        } else {
            dango_debug("vm runner create failed\n");
        }
    }

    console_log("service ready\n");
    (void)sys_notify(kDoneDoorbell);

    // Phase H/I: drive the bootstrap GPU control plane and present path first.
    // The guest owns exactly one channel endpoint (the driver's open() is
    // exclusive) and its agent runs dango-gpud before the transport self-test,
    // so this exchange must complete before the guest ping/pong probe below,
    // which then reuses the same channel.
    if (channel_tx && channel_rx) {
        gpu_bootstrap_client(channel, channel_tx, channel_rx, h2g_db, g2h_db);
    }

    if (channel_tx && channel_rx) {
        // The GPU client above already used this channel, so the transport
        // self-test's ring sequence and counters are relative to its start.
        dango::VmChannelInfo probe_before{};
        if (sys_vm_channel_inspect(channel, &probe_before, sizeof(probe_before)) != 0)
            fail("channel guest probe FAILED: counters\n");
        RingSlot message{};
        if (!ring_wait_pop(channel_rx, DANGO_CH_DEFAULT_RING_ORDER, &message, g2h_db) ||
            message.type != DANGO_CH_TEST_READY || message.length != 0)
            fail("channel guest probe FAILED: ready\n");
        const uint64_t ready_seq = message.seq;
        for (uint64_t n = 0; n < DANGO_CH_TEST_ROUNDS; ++n) {
            if (!ring_try_push(channel_tx, DANGO_CH_DEFAULT_RING_ORDER, DANGO_CH_TEST_PING, &n,
                               sizeof(n), h2g_db) ||
                sys_vm_channel_notify(channel) != 0 ||
                !ring_wait_pop(channel_rx, DANGO_CH_DEFAULT_RING_ORDER, &message, g2h_db) ||
                message.type != DANGO_CH_TEST_PONG || message.length != sizeof(n) ||
                message.seq != ready_seq + 1 + n ||
                *reinterpret_cast<const uint64_t*>(message.payload) != n)
                fail("channel guest probe FAILED: order\n");
        }
        if (!ring_wait_pop(channel_rx, DANGO_CH_DEFAULT_RING_ORDER, &message, g2h_db) ||
            message.type != DANGO_CH_TEST_DONE || message.length != sizeof(uint64_t) ||
            *reinterpret_cast<const uint64_t*>(message.payload) == 0)
            fail("channel guest probe FAILED: IRQ\n");
        if (sys_vm_channel_inspect(channel, &channel_info, sizeof(channel_info)) != 0 ||
            channel_info.messages_host_to_guest - probe_before.messages_host_to_guest !=
                DANGO_CH_TEST_ROUNDS ||
            channel_info.messages_guest_to_host - probe_before.messages_guest_to_host <
                DANGO_CH_TEST_ROUNDS + 2)
            fail("channel guest probe FAILED: counters\n");
        dango_counter("channel.h2g", channel_info.messages_host_to_guest);
        dango_counter("channel.g2h", channel_info.messages_guest_to_host);
        console_log("channel guest probe passed: " DANGO_STRINGIFY(DANGO_CH_TEST_ROUNDS)
                    " ordered round trips\n");
    }

    // A latency-band monitor that repeatedly yields remains runnable and can
    // starve the budgeted VM runner. The runner reports its own transitions;
    // the manager sleeps until a future control-plane request is implemented.
    const CapHandle idle = as_cap(sys_doorbell_create());
    if (!idle) fail("manager idle doorbell failed\n");
    for (;;) (void)sys_wait_doorbell(idle, sys_doorbell_read(idle));
}
