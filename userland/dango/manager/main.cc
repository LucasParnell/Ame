// userland/dango/manager/main.cc — Dango VM manager scaffold test.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "dango/vm.h"
#include "dango/channel.h"
#include "dango/layout.h"
#include "midix/abi/image.h"

constexpr uint32_t kRingOrder = 6;
constexpr uint64_t kLinuxRunBudgetNs = 6ull * 1000ull * 1000ull;
constexpr uint64_t kLinuxRunPeriodNs = 16666667ull;
// Linux early boot does a lot of tiny MMIO exits, especially console output.
// Amortise the user/kernel syscall boundary over more guest exits.
constexpr uint32_t kLinuxRunMaxExits = 4096;
constexpr uint32_t kMonitorPollYieldQuantum = 64;
// Keep the bootstrap runner on the manager CPU.  Init starts dango.manager on
// CPU1 so the vCPU can own that core without stalling the CPU0 boot path.
constexpr uint64_t kLinuxRunnerCoreMask = 1ull << 1;
constexpr uint32_t kLinuxRunnerHomeCpu = 1;

// The manager itself is a native service and therefore runs in the latency
// band. If the VM runner is left in VM_LATENCY during bootstrap, the manager's
// own stats/yield loop remains the only latency runnable and strict band order
// keeps picking it ahead of the lower VM_LATENCY queue forever. Put the bootstrap
// runner in the latency band; SCHED_CTX_VM_RUNNER still marks it as VM time.
constexpr uint32_t kLinuxRunnerSchedBand = dango::SCHED_BAND_LATENCY;
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

static bool should_log_counter_delta(uint64_t value, uint64_t last) {
    if (last == ~0ull) return true;
    if (value == last) return false;
    if (value <= 4) return true;
    if ((value & (value - 1)) == 0) return true;
    return value >= last + 1024ull;
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
    if (!contains(layout.ram_base, layout.ram_size, layout.channel_base, layout.channel_size)) return false;
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

static void runner_entry(uint64_t vcpu_arg) {
    const CapHandle vcpu = static_cast<CapHandle>(vcpu_arg);
    dango_log("vm runner thread entered\n");
    bool logged_first_return = false;
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
    mochi::log_set_sink(log_ring, kRingOrder, kConsoleDoorbell, "dango.manager");
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
    if (have_linux && gpu_function != kNullCap) {
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
        static_assert((0x5000'0000ull % 0x4000ull) == 0, "BAR4 guest IPA must be 16 KiB aligned");
        const int64_t bar4_rc = sys_vm_assign_map_mmio(assign, 4, 0x5000'0000ull, 0x4000, bar_rights, 0);
        if (bar4_rc != 0) {
            dango_counter("gpu.bar4_rc", static_cast<uint64_t>(bar4_rc));
            fail("gpu bar map failed\n");
        }
        // PCI INTx -> guest SPI 12 (dango-linux-gpu.dts.in interrupt-map).
        if (gpu_irq != kNullCap && sys_vm_assign_route_irq(assign, gpu_irq, vcpu, 12, 0) != 0) {
            fail("gpu irq route failed\n");
        }
        // Synthetic ECAM aperture (1 MiB); reads/writes trap through the
        // stage-2 handler in kernel/pci/ecam.cc.
        if (sys_vm_assign_expose_ecam(assign, 0x3f00'0000ull, 0x10'0000ull) != 0) {
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

    const CapHandle channel = as_cap(sys_vm_channel_create(vm, kRingOrder, 0));
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

    bool logged_agent = false;
    bool logged_hello = false;
    uint32_t monitor_poll_divider = 0;
    while (true) {
        const bool poll_now = ((monitor_poll_divider++ & (kMonitorPollYieldQuantum - 1u)) == 0);
        if (have_linux && !logged_hello && poll_now) {
            const int64_t saw_hello = sys_debug_counter(dango::VM_DEBUG_GUEST_LINUX_HELLO);
            const int64_t saw_agent = sys_debug_counter(dango::VM_DEBUG_GUEST_AGENT_START);
            if (saw_hello > 0) {
                dango_log("linux guest responded\n");
                logged_hello = true;
            } else if (!logged_agent && saw_agent > 0) {
                dango_log("linux guest agent started\n");
                logged_agent = true;
            }
        }
        static uint64_t last_attempts = ~0ull;
        static uint64_t last_notimpl = ~0ull;
        static uint64_t last_irq = ~0ull;
        static uint64_t last_wfx = ~0ull;
        static uint64_t last_unsup = ~0ull;
        if (have_linux && poll_now) {
            const int64_t attempts = sys_debug_counter(dango::VM_DEBUG_VCPU_RUN_ATTEMPTS);
            const int64_t notimpl = sys_debug_counter(dango::VM_DEBUG_VCPU_RUN_NOTIMPL);
            const int64_t irq = sys_debug_counter(dango::VM_DEBUG_VCPU_IRQ_EXITS);
            const int64_t wfx = sys_debug_counter(dango::VM_DEBUG_VCPU_WFX_EXITS);
            const int64_t unsup = sys_debug_counter(dango::VM_DEBUG_VCPU_UNSUPPORTED_EXITS);
            if (attempts >= 0) {
                const uint64_t value = static_cast<uint64_t>(attempts);
                if (should_log_counter_delta(value, last_attempts)) {
                    last_attempts = value;
                    dango_counter("vcpu.run_attempts", value);
                }
            }
            if (notimpl >= 0) {
                const uint64_t value = static_cast<uint64_t>(notimpl);
                if (should_log_counter_delta(value, last_notimpl)) {
                    last_notimpl = value;
                    dango_counter("vcpu.run_notimpl", value);
                }
            }
            if (irq >= 0) {
                const uint64_t value = static_cast<uint64_t>(irq);
                if (should_log_counter_delta(value, last_irq)) {
                    last_irq = value;
                    dango_counter("vcpu.irq_exits", value);
                }
            }
            if (wfx >= 0) {
                const uint64_t value = static_cast<uint64_t>(wfx);
                if (should_log_counter_delta(value, last_wfx)) {
                    last_wfx = value;
                    dango_counter("vcpu.wfx_exits", value);
                }
            }
            if (unsup >= 0) {
                const uint64_t value = static_cast<uint64_t>(unsup);
                if (should_log_counter_delta(value, last_unsup)) {
                    last_unsup = value;
                    dango_counter("vcpu.unsupported_exits", value);
                }
            }
        }
        if (logged_hello) {
            // Diagnostics are useful while bringing the guest up, but after the
            // agent responds this service thread should stop burning latency-band
            // quanta.  The runner thread keeps the VM alive.
            const CapHandle idle_db = as_cap(sys_doorbell_create());
            if (idle_db != kNullCap) {
                const uint64_t seen = sys_doorbell_read(idle_db);
                (void)sys_wait_doorbell(idle_db, seen);
            }
        }
        sys_yield();
    }
}
