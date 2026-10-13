#pragma once
#include "anko/types.h"
#include "minimal/framebuffer.h"

namespace anne::native_display {
#ifdef ANKO_ANNE_DIAG_TEST
constexpr u64 kDssBase = 0x47200000;
constexpr u64 kCrgBase = 0x47300000;
// QEMU's DTB is at 0x44000000; keep painted RAM below it.
constexpr u64 kLogoBase = 0x43000000;
constexpr u64 kLogoBytes = 0x1000000;
#else
constexpr u64 kDssBase = 0xe8600000;
constexpr u64 kCrgBase = 0xfff35000;
constexpr u64 kLogoBase = 0x5d000000;
constexpr u64 kLogoBytes = 0x3000000;
#endif
constexpr u64 kDssBytes = 0x80000;
struct Channel { u32 index, offset; };
// Nonzero RDMA entries in the matching EMUI 8 Hi6250 module table.
constexpr Channel kChannels[] = {{0, 0x52000}, {1, 0x53000}, {4, 0x28000},
                                 {5, 0x40000}, {6, 0x50000}, {7, 0x51000}};
struct Pipeline {
    u32 index;
    minimal::Registers dma;
    u32 secure, mask0, mask1, stretch_stride;
    u32 dsi_power, dsi_mode, dsi_phy, dsi_width, dsi_height;
    u32 mctl_enable, mctl_itf, mctl_dbuf, mctl_overlay;
    u32 overlay_size, overlay_config, overlay_start, overlay_end;
    u32 channel_overlay_enable, channel_select;
    u32 layer_position[6], layer_end[6], layer_config[6];
    u32 dfc_size, dfc_clip_h, dfc_clip_v, dfc_padding;
};
// The recovery fbdev address is an IOVA. Native writes require a complete,
// enabled physical linear RGB path inside the preserved logo reservation.
constexpr const char* rejection(const Pipeline& p) {
    const auto& r = p.dma;
    if (!(r.channel & 1)) return "rdma.disabled";
    if (r.channel & 4) return "rdma.compressed";
    if (p.secure & 1) return "rdma.secure";
    if (!(r.ldi & 1)) return "ldi.disabled";
    if ((r.panel_width & 0x1fff) != 1079 || (r.panel_height & 0x1fff) != 2279) return "ldi.geometry";
    if (r.control & (1u << 8)) return "rdma.iova";
    if (r.control & ((1u << 1) | (7u << 9) | (1u << 12) | (1u << 17))) return "rdma.transform";
    const u32 format = (r.control >> 3) & 31;
    if (format != 0 && format != 5 && format != 6) return "rdma.format";
    const u32 bpp = format == 0 ? 2 : 4;
    if (r.stride >> 16 || r.stride * 16 != 1080 * bpp) return "rdma.stride";
    if (r.address & 15) return "rdma.alignment";
    if (r.x0 || r.y0 || r.x1 != (1080 * bpp / 16 - 1) || r.y1 != 2279 ||
        p.mask0 || p.mask1 || p.stretch_stride) return "rdma.rectangle";
    const u64 bytes = u64{1080 * bpp} * 2280;
    if (r.address < kLogoBase || r.address - kLogoBase > kLogoBytes ||
        bytes > kLogoBytes - (r.address - kLogoBase)) return "rdma.outside_reserved_logo";
    if (!(p.dsi_power & 1) || (p.dsi_mode & 1) || !(p.dsi_phy & 1) ||
        p.dsi_width != 1080 || p.dsi_height != 2280) return "dsi.inactive_or_geometry";
    if (!(p.mctl_enable & 1) || (p.mctl_itf & 3) != 1 || (p.mctl_dbuf & 3) != 1 ||
        (p.mctl_overlay & 15) != 1) return "mctl.disconnected";
    constexpr u32 size = (2279u << 16) | 1079;
    if (p.overlay_size != size || !(p.overlay_config & 1) || p.overlay_start ||
        p.overlay_end != size) return "overlay.geometry_or_disabled";
    if (p.dfc_size != ((1079u << 16) | 2279) || p.dfc_clip_h || p.dfc_clip_v || p.dfc_padding)
        return "dfc.cropped_or_padded";
    for (u32 layer = 0; layer < 6; ++layer) {
        if (((p.channel_select >> ((layer + 1) * 4)) & 15) != p.index) continue;
        if ((p.layer_config[layer] & 0x101) != 1 || p.layer_position[layer] ||
            p.layer_end[layer] != size ||
            (p.channel_overlay_enable & (0x100 | (1u << (layer + 1)))) != (0x100 | (1u << (layer + 1)))) continue;
        return nullptr;
    }
    return "overlay.no_routed_fullscreen_layer";
}
void snapshot(const char* phase, bool paint);
bool bind_console();
bool framebuffer(u64& address, u32& width, u32& height);
void handoff_console();
void console_putchar(char c);
void console_flush();
bool console_active();
u64 console_characters();
}
