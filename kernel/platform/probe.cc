// kernel/platform/probe.cc — DTB hardware discovery.
//
// platform::probe() walks the DTB with fdt::walk() and fills g_hw with
// discovered hardware addresses and device presence flags.
// Falls back to board_fallback.cc constants when DTB is absent or corrupt.

#include "anko/hw_info.h"
#include "anko/fdt.h"
#include "libk/string.h"
#include "midix/hw/tegra_dc.h"

extern "C" u64 g_boot_el;

namespace platform {

namespace {

static bool starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

// Check whether a FDT compatible property contains target as one of its
// NUL-delimited strings.
static bool compat_has(const u8* data, u32 len, const char* target) {
    const char* s = reinterpret_cast<const char*>(data);
    u32 off = 0;
    while (off < len) {
        u32 end = off;
        while (end < len && s[end]) {
            ++end;
        }
        if (end == len) {
            return false;
        }
        if (libk::strcmp(s + off, target) == 0) {
            return true;
        }
        while (off < len && s[off]) {
            ++off;
        }
        ++off; // skip NUL
    }
    return false;
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

constexpr u32 kMaxDepth = 8;
constexpr u32 kRegMax = 64; // enough for two address/size pairs (GIC)

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

struct NodeSlot {
    u8 kind;
    bool status_ok; // false if status property was "disabled"
    alignas(4) u8 reg[kRegMax];
    u32 reg_len;
    alignas(4) u8 ranges[kRangesMax]; // PCIe MMIO window description
    u32 ranges_len;
    u32 fb_width;  // kSimpleFb: width in pixels
    u32 fb_height; // kSimpleFb: height in lines
    u32 fb_stride;
    bool fb_format32;
    char path[256];
};

struct ProbeCtx {
    HwInfo* hw;
    NodeSlot slots[kMaxDepth];
    char console_path[256];
    bool console_found;
};

static bool probe_begin(void* v, const char* name, u32 depth) {
    auto& ctx = *static_cast<ProbeCtx*>(v);
    if (depth >= kMaxDepth) {
        return true;
    }
    auto& s = ctx.slots[depth];
    // Detect /memory node by name prefix (e.g. "memory@40000000").
    s.kind = starts_with(name, "memory") ? kMemory : kNone;
    s.status_ok = true;
    s.reg_len = 0;
    s.ranges_len = 0;
    s.fb_width = 0;
    s.fb_height = 0;
    s.fb_stride = 0;
    s.fb_format32 = false;
    u32 pos = 0;
    if (depth) {
        const char* parent = ctx.slots[depth - 1].path;
        while (parent[pos] && pos + 1 < sizeof(s.path)) {
            s.path[pos] = parent[pos];
            ++pos;
        }
        if (pos + 1 < sizeof(s.path)) {
            s.path[pos++] = '/';
        }
    }
    for (u32 i = 0; name[i] && pos + 1 < sizeof(s.path); ++i) {
        s.path[pos++] = name[i];
    }
    s.path[pos] = 0;
    return true;
}

static void probe_end(void* v, u32 depth) {
    auto& ctx = *static_cast<ProbeCtx*>(v);
    if (depth >= kMaxDepth) {
        return;
    }
    const auto& s = ctx.slots[depth];
    if (!s.status_ok) {
        return;
    }
    HwInfo& hw = *ctx.hw;

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
            (!ctx.console_path[0] || libk::strcmp(s.path, ctx.console_path) == 0)) {
            ctx.console_found = true;
            hw.console_kind = ConsoleKind::PL011;
            hw.console_base = read_phys_be(s.reg, s.reg_len);
        }
        break;
    case kTegra:
        if (s.reg_len >= 8 && (!ctx.console_found) &&
            (!ctx.console_path[0] || libk::strcmp(s.path, ctx.console_path) == 0)) {
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
        hw.has_anne_usb = s.reg_len >= 16 && read_phys_be(s.reg,s.reg_len) == 0xff100000 &&
            read_phys_be(s.reg+8,s.reg_len-8) == 0x40000;
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

static void probe_prop(void* v, const char* name, const u8* data, u32 len, u32 depth) {
    auto& ctx = *static_cast<ProbeCtx*>(v);
    if (depth >= kMaxDepth) {
        return;
    }
    auto& s = ctx.slots[depth];

    if (libk::strcmp(name, "compatible") == 0) {
        u8 new_kind = kNone;
        if (compat_has(data, len, "arm,pl011")) {
            new_kind = kPL011;
        } else if (compat_has(data, len, "nvidia,tegra210-uart") ||
                   compat_has(data, len, "nvidia,tegra20-uart")) {
            new_kind = kTegra;
        } else if (compat_has(data, len, "arm,gic-400") ||
                   compat_has(data, len, "arm,cortex-a9-gic") ||
                   compat_has(data, len, "arm,cortex-a15-gic") ||
                   compat_has(data, len, "arm,gic-v2")) {
            new_kind = kGIC;
        } else if (compat_has(data, len, "qemu,fw-cfg-mmio")) {
            new_kind = kFwCfg;
        } else if (compat_has(data, len, "virtio,mmio")) {
            new_kind = kVirtio;
        } else if (compat_has(data, len, "pci-host-ecam-generic")) {
            new_kind = kPCIe;
        } else if (compat_has(data,len,"hisilicon,hi6250-usb-otg")) {
            new_kind = kAnneUsb;
        } else if (compat_has(data, len, "arm,smmu-v3")) {
            new_kind = kSMMU;
        } else if (compat_has(data, len, "simple-framebuffer")) {
            new_kind = kSimpleFb;
        }
        // Don't overwrite kMemory (set from name in begin_node).
        if (s.kind != kMemory) {
            s.kind = new_kind;
        }
    } else if (libk::strcmp(name, "reg") == 0) {
        u32 copy = len < kRegMax ? len : kRegMax;
        libk::memcpy(s.reg, data, copy);
        s.reg_len = copy;
    } else if (libk::strcmp(name, "ranges") == 0) {
        u32 copy = len < kRangesMax ? len : kRangesMax;
        libk::memcpy(s.ranges, data, copy);
        s.ranges_len = copy;
    } else if (libk::strcmp(name, "width") == 0 && len >= 4) {
        s.fb_width = libk::be32(data);
    } else if (libk::strcmp(name, "height") == 0 && len >= 4) {
        s.fb_height = libk::be32(data);
    } else if (libk::strcmp(name, "stride") == 0 && len == 4) {
        s.fb_stride = libk::be32(data);
    } else if (libk::strcmp(name, "format") == 0) {
        constexpr const char* formats[] = {"a8r8g8b8", "x8r8g8b8", "a8b8g8r8", "x8b8g8r8"};
        for (const auto* format : formats) {
            if (len == 9 && libk::memcmp(data, format, 9) == 0) {
                s.fb_format32 = true;
            }
        }
    } else if (libk::strcmp(name, "status") == 0 && len > 0) {
        const char* sv = reinterpret_cast<const char*>(data);
        s.status_ok = ((len == 5 && libk::memcmp(sv, "okay", 5) == 0) ||
                       (len == 3 && libk::memcmp(sv, "ok", 3) == 0));
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
        libk::framebuffer_fill({hw.t210_dc_fb_phys, hw.t210_dc_fb_width,
                               hw.t210_dc_fb_height}, 0xff00ff00u);
        asm volatile("dsb sy" ::: "memory");
    }
}

void probe(PhysAddr dtb_phys) {
    // Always copy boot_el from entry.S — it's runtime state.
    g_hw.boot_el = static_cast<u32>(g_boot_el);

    const auto* dtb = reinterpret_cast<const u8*>(dtb_phys);
    const bool valid_dtb = dtb_phys && !(dtb_phys & 3) && libk::be32(dtb) == fdt::kMagic;
    if (valid_dtb) {
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
        if (!fdt::walk(dtb, {&selection, begin, end, prop})) {
            return;
        }
        if (selection.value[0] && selection.value[0] != '/') {
            char alias[256];
            libk::memcpy(alias, selection.value, sizeof(alias));
            selection.node = "aliases";
            selection.prop = alias;
            selection.value[0] = 0;
            if (!fdt::walk(dtb, {&selection, begin, end, prop})) {
                return;
            }
        }
        HwInfo discovered = g_hw;
        ProbeCtx ctx{};
        ctx.hw = &discovered;
        libk::memcpy(ctx.console_path, selection.value, sizeof(ctx.console_path));
#ifdef ANKO_BOARD_HUAWEI_ANNE
        if (!ctx.console_path[0]) {
            // Stock /aliases/serial0 and bootargs console=ttyS0 select this UART.
            libk::memcpy(ctx.console_path, "/amba/uart@fdf02000", sizeof("/amba/uart@fdf02000"));
        }
#endif
        if (fdt::walk(dtb, {&ctx, probe_begin, probe_end, probe_prop})) {
            g_hw = discovered;
#ifdef ANKO_BOARD_HUAWEI_ANNE
            // Only the first bank is supported for initial bring-up. Reserved
            // firmware regions are excluded by reserve_dtb_regions() later.
            if (g_hw.ram_size > 128ull * 1024 * 1024) g_hw.ram_size = 128ull * 1024 * 1024;
#endif
        }
    }

    // T210-specific: read TegraDC0 display registers while MMU is still off.
    // Run even without a valid DTB because the Switch fallback already selects
    // Tegra16550, and the LCD is our only practical no-UART progress signal.
    if (g_hw.console_kind == ConsoleKind::Tegra16550) {
        probe_t210_dc(g_hw);
    }
}

} // namespace platform
