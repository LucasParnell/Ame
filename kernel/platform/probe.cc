// kernel/platform/probe.cc — DTB hardware discovery.
//
// platform::probe() walks the DTB with fdt::walk() and fills g_hw with
// discovered hardware addresses and device presence flags.
// Falls back to board_fallback.cc constants when DTB is absent or corrupt.

#include "anko/hw_info.h"
#include "anko/boot_reservations.h"
#include "anko/fdt.h"
#include "libk/string.h"
#include "midix/hw/tegra_dc.h"
#ifdef ANKO_BOARD_HUAWEI_ANNE
#include "anko/boot_cache.h"
#endif

extern "C" u64 g_boot_el;

namespace platform {

namespace {

BootReservations g_boot_reservations{};

static bool starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

// Match each compatible string once. Table order preserves device precedence
// even if firmware lists several supported compatible strings in another order.
struct Compatible {
    const char* name;
    u32 length;
    u8 kind;
};
template <usize N> constexpr Compatible compatible(const char (&name)[N], u8 kind) {
    return {name, N - 1, kind};
}

// Read a big-endian physical address from the first 4 or 8 bytes of data.
// #address-cells=2 (standard for AArch64 boards): two BE32 words → 64-bit addr.
static PhysAddr read_phys_be(const u8* p, u32 avail) {
    if (avail >= 8) {
        return (PhysAddr(libk::be32(p)) << 32) | PhysAddr(libk::be32(p + 4));
    }
    if (avail >= 4) {
        return PhysAddr(libk::be32(p));
    }
    return 0;
}

constexpr u32 kMaxDepth = 64; // same bound as the structural walker
constexpr u32 kRegMax = 64;   // enough for two address/size pairs (GIC)

// Node kind tags.
constexpr u8 kNone = 0;
constexpr u8 kMemory = 1;
constexpr u8 kPL011 = 2;
constexpr u8 kTegra = 3;
constexpr u8 kGIC = 4;
constexpr u8 kFwCfg = 5;
constexpr u8 kVirtio = 6;
constexpr u8 kPCIe = 7;
constexpr u8 kSimpleFb = 8;
constexpr u8 kSMMU = 9;
constexpr u8 kAnneUsb = 10;

constexpr u32 kRangesMax = 128; // PCI ranges: 3 windows * 7 cells * 4 B

// One cache line of hot metadata per active depth. Property bytes stay in the
// immutable DTB: no copied register/range arrays, pointers into scratch, or
// heap-owned node graph. A single path buffer is reused by the streaming walk.
struct alignas(64) NodeSlot {
    const u8* reg;
    const u8* ranges;
    u32 reg_len, ranges_len;
    u32 fb_width, fb_height, fb_stride;
    u32 path_length;
    u8 kind;
    bool status_ok, parent_enabled, fb_format32, path_valid;
    u8 selection; // direct children: 1 = /chosen, 2 = /aliases
    u8 reservation; // 1 = /reserved-memory, 2 = its descendants
};
static_assert(sizeof(NodeSlot) == 64);
struct ConsoleCandidate {
    PhysAddr base;
    u16 path_offset, path_length;
    ConsoleKind kind;
};
struct Alias {
    const char* name;
    const u8* data;
    u32 length;
};
struct ProbeCtx {
    HwInfo* hw;
    BootReservations* reservations;
    NodeSlot slots[kMaxDepth];
    char path[256];
    char console_path[256];
    bool console_found;
    // Cold, bounded selection data handles /chosen and /aliases in any order.
    // Copy paths only for usable UARTs, never for every node. Overflow invokes
    // an exact streaming replay; limits must not change which UART is selected.
    ConsoleCandidate consoles[16];
    Alias aliases[32];
    char console_paths[1536];
    u32 console_count, alias_count, path_used;
    bool console_overflow, alias_overflow, collect_selection = true;
};

static void copy_console_path(char (&out)[256], const u8* data, u32 len) {
    if (!len || len > sizeof(out) || data[len - 1]) return;
    u32 i = 0;
    for (; i + 1 < len && data[i] && data[i] != ':'; ++i) out[i] = data[i];
    out[i] = 0;
}

static u8 compatible_kind(const u8* data, u32 len) {
    static constexpr Compatible kinds[] = {
        compatible("arm,pl011", kPL011),
        compatible("nvidia,tegra210-uart", kTegra),
        compatible("nvidia,tegra20-uart", kTegra),
        compatible("arm,gic-400", kGIC),
        compatible("arm,cortex-a9-gic", kGIC),
        compatible("arm,cortex-a15-gic", kGIC),
        compatible("arm,gic-v2", kGIC),
        compatible("qemu,fw-cfg-mmio", kFwCfg),
        compatible("virtio,mmio", kVirtio),
        compatible("pci-host-ecam-generic", kPCIe),
        compatible("hisilicon,hi6250-usb-otg", kAnneUsb),
        compatible("arm,smmu-v3", kSMMU),
        compatible("simple-framebuffer", kSimpleFb),
    };
    u32 best = sizeof(kinds) / sizeof(kinds[0]);
    for (u32 off = 0; off < len;) {
        u32 end = off;
        while (end < len && data[end]) {
            ++end;
        }
        if (end == len) {
            return kNone;
        }
        for (u32 i = 0; i < best; ++i) {
            if (end - off == kinds[i].length &&
                libk::memcmp(data + off, kinds[i].name, kinds[i].length) == 0) {
                best = i;
                break;
            }
        }
        off = end + 1;
    }
    return best < sizeof(kinds) / sizeof(kinds[0]) ? kinds[best].kind : kNone;
}

static bool probe_begin(void* v, const char* name, u32 depth) {
    auto& ctx = *static_cast<ProbeCtx*>(v);
    if (depth >= kMaxDepth) {
        return true;
    }
    auto& s = ctx.slots[depth];
    s = {};
    s.kind = depth == 1 && (libk::strcmp(name, "memory") == 0 || starts_with(name, "memory@"))
                 ? kMemory
                 : kNone;
    s.parent_enabled = !depth || ctx.slots[depth - 1].status_ok;
    s.status_ok = s.parent_enabled;
    // Preserve the reservation consumer's existing 16-level bound and reserve
    // disabled nodes too: firmware-owned memory is independent of device use.
    if (ctx.reservations && depth < 16) {
        if (depth == 1 && libk::strcmp(name, "reserved-memory") == 0) {
            s.reservation = 1;
        } else if (depth && ctx.slots[depth - 1].reservation) {
            s.reservation = 2;
        }
    }
    if (depth == 1 && ctx.collect_selection) {
        if (libk::strcmp(name, "chosen") == 0) s.selection = 1;
        else if (libk::strcmp(name, "aliases") == 0) s.selection = 2;
    }
    s.path_valid = !depth || ctx.slots[depth - 1].path_valid;
    u32 pos = depth ? ctx.slots[depth - 1].path_length : 0;
    const u32 length = libk::strlen(name);
    if (!s.path_valid || pos + (depth ? 1u : 0u) + length >= sizeof(ctx.path)) {
        s.path_valid = false;
    } else {
        if (depth) {
            ctx.path[pos++] = '/';
        }
        libk::memcpy(ctx.path + pos, name, length);
        s.path_length = pos + length;
        ctx.path[s.path_length] = 0;
    }
    return true;
}

static void probe_end(void* v, u32 depth) {
    auto& ctx = *static_cast<ProbeCtx*>(v);
    if (depth >= kMaxDepth) {
        return;
    }
    const auto& s = ctx.slots[depth];
    if (s.path_valid) {
        ctx.path[s.path_length] = 0;
    }
    if (!s.status_ok) {
        return;
    }
    HwInfo& hw = *ctx.hw;

    if ((s.kind == kPL011 || s.kind == kTegra) && s.reg_len >= 8 && ctx.collect_selection) {
        if (ctx.console_count == sizeof(ctx.consoles) / sizeof(ctx.consoles[0]) ||
            (s.path_valid && s.path_length + 1 > sizeof(ctx.console_paths) - ctx.path_used)) {
            ctx.console_overflow = true;
        } else {
            auto& console = ctx.consoles[ctx.console_count++];
            console.base = read_phys_be(s.reg, s.reg_len);
            console.kind = s.kind == kPL011 ? ConsoleKind::PL011 : ConsoleKind::Tegra16550;
            console.path_offset = ctx.path_used;
            console.path_length = s.path_valid ? s.path_length : 0;
            if (s.path_valid) {
                libk::memcpy(ctx.console_paths + ctx.path_used, ctx.path, s.path_length + 1);
                ctx.path_used += s.path_length + 1;
            }
        }
        return;
    }

    switch (s.kind) {
    case kMemory:
        // reg: [addr_hi addr_lo size_hi size_lo] (2×2 BE32 words)
        if (s.reg_len >= 16) {
            hw.ram_base = read_phys_be(s.reg, s.reg_len);
            hw.ram_size = read_phys_be(s.reg + 8, s.reg_len >= 16 ? 8 : 0);
        }
        break;
    case kPL011:
        if (s.reg_len >= 8 && (!ctx.console_found) &&
            (!ctx.console_path[0] ||
             (s.path_valid && libk::strcmp(ctx.path, ctx.console_path) == 0))) {
            ctx.console_found = true;
            hw.console_kind = ConsoleKind::PL011;
            hw.console_base = read_phys_be(s.reg, s.reg_len);
        }
        break;
    case kTegra:
        if (s.reg_len >= 8 && (!ctx.console_found) &&
            (!ctx.console_path[0] ||
             (s.path_valid && libk::strcmp(ctx.path, ctx.console_path) == 0))) {
            ctx.console_found = true;
            hw.console_kind = ConsoleKind::Tegra16550;
            hw.console_base = read_phys_be(s.reg, s.reg_len);
        }
        break;
    case kGIC:
        // reg: [dist_addr_hi dist_addr_lo dist_size_hi dist_size_lo
        //       cpu_addr_hi  cpu_addr_lo  cpu_size_hi  cpu_size_lo]
        hw.intc_kind = IntcKind::GICv2;
        if (s.reg_len >= 8) {
            hw.gic_dist_base = read_phys_be(s.reg, s.reg_len);
        }
        if (s.reg_len >= 32) {
            hw.gic_cpu_base = read_phys_be(s.reg + 16, 8);
        }
        break;
    case kFwCfg:
        if (s.reg_len >= 8) {
            hw.has_fwcfg = true;
            hw.fwcfg_base = read_phys_be(s.reg, s.reg_len);
        }
        break;
    case kVirtio:
        // Record every virtio-mmio transport; the legacy has_virtio_blk /
        // virtio_base fields still point at the first instance.
        if (s.reg_len >= 8) {
            const PhysAddr base = read_phys_be(s.reg, s.reg_len);
            if (!hw.has_virtio_blk) {
                hw.has_virtio_blk = true;
                hw.virtio_base = base;
            }
            if (hw.virtio_count < HwInfo::kMaxVirtioMmio) {
                hw.virtio_bases[hw.virtio_count++] = base;
            }
        }
        break;
    case kAnneUsb:
        // The fixed Anne resource table is granted only for this DT core.
        hw.has_anne_usb = s.reg_len >= 16 && read_phys_be(s.reg, s.reg_len) == 0xff100000 &&
                          read_phys_be(s.reg + 8, s.reg_len - 8) == 0x40000;
        break;
    case kSMMU:
        // SMMUv3 register base (arm,smmu-v3 reg).  Keep the first instance.
        if (s.reg_len >= 8 && !hw.has_smmu) {
            hw.has_smmu = true;
            hw.smmu_base = read_phys_be(s.reg, s.reg_len);
        }
        break;
    case kPCIe:
        if (s.reg_len >= 8) {
            hw.has_pcie = true;
            hw.ecam_base = read_phys_be(s.reg, s.reg_len);
        }
        // The "ranges" property gives the PCIe MMIO window (child_addr[3],
        // parent_addr[2], size[2] per entry, big-endian).  Take the first
        // non-config window: it is where the attach path allocates BARs.
        for (u32 i = 0; i + 7 <= s.ranges_len / 4; i += 7) {
            const u8* e = s.ranges + 4 * i;
            const u32 space = libk::be32(e);
            const u32 kind = (space >> 24) & 0x3u; // 1 = IO, 2 = MMIO32, 3 = MMIO64
            if (kind != 2 && kind != 3) {
                continue;
            }
            const u64 base = (static_cast<u64>(libk::be32(e + 12)) << 32) | libk::be32(e + 16);
            const u64 size = (static_cast<u64>(libk::be32(e + 20)) << 32) | libk::be32(e + 24);
            if (size == 0) {
                continue;
            }
            hw.has_pci_mmio = true;
            hw.pci_mmio_base = base;
            hw.pci_mmio_size = size;
            break;
        }
        break;
    case kSimpleFb:
        // This renderer only supports tightly packed 32-bit scanout. Validate
        // both the advertised allocation and DRAM before exposing its address.
        if (s.reg_len >= 16 && s.fb_format32 && !hw.has_t210_dc) {
            const libk::Framebuffer32 fb{read_phys_be(s.reg, 8), s.fb_width, s.fb_height};
            if (s.fb_width <= ~u32{0} / 4 && s.fb_stride == s.fb_width * 4 &&
                libk::framebuffer_fits(fb, fb.address, read_phys_be(s.reg + 8, 8)) &&
                libk::framebuffer_fits(fb, hw.ram_base, hw.ram_size)) {
                hw.has_t210_dc = true;
                hw.t210_dc_fb_phys = fb.address;
                hw.t210_dc_fb_width = fb.width;
                hw.t210_dc_fb_height = fb.height;
            }
        }
        break;
    default:
        break;
    }
}

static void probe_prop(void* v, fdt::Property id, const char* name, const u8* data, u32 len,
                       u32 depth) {
    auto& ctx = *static_cast<ProbeCtx*>(v);
    if (depth >= kMaxDepth) {
        return;
    }
    auto& s = ctx.slots[depth];
    if (ctx.reservations && s.reservation == 2 && id == fdt::Property::Reg) {
        // The existing boot consumer uses two address and two size cells.
        // Consume the whole property, not the hardware slot's short reg view.
        for (u32 off = 0; len - off >= 16; off += 16) {
            const u64 base = read_phys_be(data + off, 8);
            const u64 size = read_phys_be(data + off + 8, 8);
            if (base && size) ctx.reservations->append(base, size);
        }
    }
    if (s.selection == 1 && id == fdt::Property::StdoutPath) {
        copy_console_path(ctx.console_path, data, len);
    } else if (s.selection == 2) {
        if (ctx.alias_count == sizeof(ctx.aliases) / sizeof(ctx.aliases[0])) {
            ctx.alias_overflow = true;
        } else {
            ctx.aliases[ctx.alias_count++] = {name, data, len};
        }
    }
    // A disabled ancestor cannot be re-enabled by a child. Do not classify
    // compatible lists or read register arrays for an unusable subtree.
    if (!s.parent_enabled) return;
    switch (id) {
    case fdt::Property::Compatible: {
        const u8 new_kind = compatible_kind(data, len);
        // Don't overwrite kMemory (set from name in begin_node).
        if (s.kind != kMemory) {
            s.kind = new_kind;
        }
        break;
    }
    case fdt::Property::Reg: {
        s.reg = data;
        s.reg_len = len < kRegMax ? len : kRegMax;
        break;
    }
    case fdt::Property::Ranges: {
        s.ranges = data;
        s.ranges_len = len < kRangesMax ? len : kRangesMax;
        break;
    }
    case fdt::Property::Width: if (len >= 4) s.fb_width = libk::be32(data); break;
    case fdt::Property::Height: if (len >= 4) s.fb_height = libk::be32(data); break;
    case fdt::Property::Stride: if (len == 4) s.fb_stride = libk::be32(data); break;
    case fdt::Property::Format: {
        constexpr const char* formats[] = {"a8r8g8b8", "x8r8g8b8", "a8b8g8r8", "x8b8g8r8"};
        for (const auto* format : formats) {
            if (len == 9 && libk::memcmp(data, format, 9) == 0) {
                s.fb_format32 = true;
            }
        }
        break;
    }
    case fdt::Property::Status:
        if (len) {
            s.status_ok = (len == 5 && libk::memcmp(data, "okay", 5) == 0) ||
                          (len == 3 && libk::memcmp(data, "ok", 3) == 0);
        }
        break;
    default: break;
    }
}

} // namespace

// Read the active window while the MMU is off. Invalid or unsupported scanout
// stays unavailable; guessing an address here can overwrite unrelated memory.
static void probe_t210_dc(HwInfo& hw) {
    if (!hw.has_t210_dc) {
        const auto fb = midix::tegra_dc::read_active(hw.ram_base, hw.ram_size);
        hw.has_t210_dc = fb.address != 0;
        hw.t210_dc_fb_phys = fb.address;
        hw.t210_dc_fb_width = fb.width;
        hw.t210_dc_fb_height = fb.height;
    }
    if (hw.has_t210_dc) {
        libk::framebuffer_fill({hw.t210_dc_fb_phys, hw.t210_dc_fb_width, hw.t210_dc_fb_height},
                               0xff00ff00u);
        asm volatile("dsb sy" ::: "memory");
    }
}

// Only the rare aliases-table overflow uses a preliminary selection walk.
// Keep this unbounded in node count, with the same bounded path-string rules.
static bool replay_console_path(const u8* dtb, usize available, char (&path)[256]) {
    struct Selection {
        const char* node;
        const char* prop;
        bool inside;
        char value[256];
    } selection{"chosen", "stdout-path", false, {}};
    auto begin = [](void* v, const char* n, u32 depth) {
        auto& c = *static_cast<Selection*>(v);
        if (depth == 1) {
            c.inside = libk::strcmp(n, c.node) == 0;
        }
        return true;
    };
    auto end = [](void* v, u32 depth) {
        if (depth == 1) {
            static_cast<Selection*>(v)->inside = false;
        }
    };
    auto prop = [](void* v, const char* n, const u8* data, u32 len, u32 depth) {
        auto& c = *static_cast<Selection*>(v);
        if (!c.inside || depth != 1 || libk::strcmp(n, c.prop) != 0 || !len ||
            len > sizeof(c.value) || data[len - 1]) {
            return;
        }
        u32 i = 0;
        for (; i + 1 < len && data[i] && data[i] != ':'; ++i) {
            c.value[i] = data[i];
        }
        c.value[i] = 0;
    };
    if (!fdt::walk(dtb, {&selection, begin, end, prop}, available)) {
        return false;
    }
    if (selection.value[0] && selection.value[0] != '/') {
        char alias[256];
        libk::memcpy(alias, selection.value, sizeof(alias));
        selection.node = "aliases";
        selection.prop = alias;
        selection.value[0] = 0;
        if (!fdt::walk(dtb, {&selection, begin, end, prop}, available)) {
            return false;
        }
    }
    libk::memcpy(path, selection.value, sizeof(path));
    return true;
}

bool probe_dtb(const u8* dtb, usize available, HwInfo& hw, BootReservations* reservations) {
    if (reservations) {
        reservations->source = nullptr;
        reservations->count = reservations->memreserve_count = 0;
        reservations->overflow = false;
    }
    if (!dtb || (reinterpret_cast<PhysAddr>(dtb) & 3) || (available && available < 40) ||
        libk::be32(dtb) != fdt::kMagic) {
        return false;
    }
    HwInfo discovered = hw;
    ProbeCtx ctx{};
    ctx.hw = &discovered;
    bool reservations_valid = false;
    if (reservations) {
        auto entry = [](void* v, u64 base, u64 size) {
            static_cast<BootReservations*>(v)->append(base, size);
            return true;
        };
        reservations_valid = fdt::walk_memreserve(dtb, reservations, entry, available);
        reservations->memreserve_count = reservations->count;
        if (reservations_valid) ctx.reservations = reservations;
    }
    if (!fdt::walk(dtb, {&ctx, probe_begin, probe_end, nullptr, probe_prop}, available)) {
        return false;
    }

    if (ctx.console_path[0] && ctx.console_path[0] != '/') {
        if (ctx.alias_overflow) {
            if (!replay_console_path(dtb, available, ctx.console_path)) return false;
        } else {
            char alias[256];
            libk::memcpy(alias, ctx.console_path, sizeof(alias));
            ctx.console_path[0] = 0;
            for (u32 i = 0; i < ctx.alias_count; ++i) {
                if (libk::strcmp(ctx.aliases[i].name, alias) == 0) {
                    copy_console_path(ctx.console_path, ctx.aliases[i].data, ctx.aliases[i].length);
                }
            }
        }
    }
#ifdef ANKO_BOARD_HUAWEI_ANNE
    if (!ctx.console_path[0]) {
        // Stock /aliases/serial0 and bootargs console=ttyS0 select this UART.
        libk::memcpy(ctx.console_path, "/amba/uart@fdf02000", sizeof("/amba/uart@fdf02000"));
    }
#endif
    for (u32 i = 0; i < ctx.console_count; ++i) {
        const auto& console = ctx.consoles[i];
        if (!ctx.console_path[0] ||
            (console.path_length &&
             libk::strcmp(ctx.console_paths + console.path_offset, ctx.console_path) == 0)) {
            discovered.console_kind = console.kind;
            discovered.console_base = console.base;
            ctx.console_found = true;
            break;
        }
    }
    if (!ctx.console_found && ctx.console_overflow) {
        // Resolve arbitrarily many UARTs without an arbitrarily large boot
        // stack. Rebuild the candidate hardware to avoid duplicating transports.
        discovered = hw;
        ctx.collect_selection = false;
        ctx.reservations = nullptr; // the first walk already collected the batch
        if (!fdt::walk(dtb, {&ctx, probe_begin, probe_end, nullptr, probe_prop}, available)) {
            return false;
        }
    }
#ifdef ANKO_BOARD_HUAWEI_ANNE
    // Only the first bank is supported for initial bring-up. Reserved firmware
    // regions are excluded by reserve_dtb_regions() later.
    if (discovered.ram_size > 128ull * 1024 * 1024) {
        discovered.ram_size = 128ull * 1024 * 1024;
    }
#endif
    hw = discovered;
    if (reservations_valid && !reservations->overflow) reservations->source = dtb;
    return true;
}

const BootReservations& boot_reservations() {
    return g_boot_reservations;
}

void probe(PhysAddr dtb_phys) {
    // Single boot CPU owns this transform; publish only a fully validated
    // result before allocator setup and SMP startup. No DTB bytes are written
    // or retained in HwInfo. SMP cleans the kernel (including g_hw) to PoC and
    // executes DSB SY before CPU_ON; its release/acquire handshake then exposes
    // these values to the secondary CPUs.
    g_hw.boot_el = static_cast<u32>(g_boot_el);
    {
#ifdef ANKO_BOARD_HUAWEI_ANNE
        // Same VA=PA throughout, with no concurrent CPU or alternate mapping.
        // Restore MMU-off boot (and publish dirty kernel lines to PoC) before
        // allocator initialization or any board framebuffer/MMIO probing.
        mm::BootDtbCache cache(dtb_phys);
#endif
        (void)probe_dtb(reinterpret_cast<const u8*>(dtb_phys), 0, g_hw,
                        &g_boot_reservations);
    }

    // T210-specific: read TegraDC0 display registers while MMU is still off.
    // Run even without a valid DTB because the Switch fallback already selects
    // Tegra16550, and the LCD is our only practical no-UART progress signal.
    if (g_hw.console_kind == ConsoleKind::Tegra16550) {
        probe_t210_dc(g_hw);
    }
}

} // namespace platform
