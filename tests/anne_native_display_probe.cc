#include "display_diagnostics.h"
#include <cassert>
#include <cstring>
using namespace anne::native_display;

Pipeline valid() {
    Pipeline p{};
    p.index = 1;
    p.dma = {1, 5 << 3, static_cast<u32>(kLogoBase), 270, 0, 0, 269, 2279, 1, 1079, 2279};
    p.dsi_power = p.dsi_phy = 1; p.dsi_width = 1080; p.dsi_height = 2280;
    p.mctl_enable = p.mctl_itf = p.mctl_dbuf = p.mctl_overlay = 1;
    p.overlay_size = p.overlay_end = p.layer_end[0] = (2279u << 16) | 1079;
    p.overlay_config = p.layer_config[0] = 1;
    p.channel_overlay_enable = 0x102; p.channel_select = 0x18;
    p.dfc_size = (1079u << 16) | 2279;
    return p;
}
int main() {
    assert(!rejection(valid()));
    auto bad = [](Pipeline p, const char* expected) { assert(rejection(p)); assert(!std::strcmp(rejection(p), expected)); };
    Pipeline p = valid(); p.dma.control |= 1u << 8; bad(p, "rdma.iova");
    p = valid(); p.dma.channel = 0; bad(p, "rdma.disabled");
    p = valid(); p.dma.channel |= 4; bad(p, "rdma.compressed");
    p = valid(); p.secure = 1; bad(p, "rdma.secure");
    p = valid(); p.dma.address = kLogoBase - 16; bad(p, "rdma.outside_reserved_logo");
    p = valid(); p.dma.address = kLogoBase + kLogoBytes - 16; bad(p, "rdma.outside_reserved_logo");
    p = valid(); p.dma.address += 4; bad(p, "rdma.alignment");
    p = valid(); p.dma.control |= 1u << 9; bad(p, "rdma.transform");
    p = valid(); p.dma.control = 8 << 3; bad(p, "rdma.format");
    p = valid(); p.dma.stride = 0x10000 + 270; bad(p, "rdma.stride");
    p = valid(); p.dma.x1 = 270; bad(p, "rdma.rectangle");
    p = valid(); p.dma.panel_height = 4559; bad(p, "ldi.geometry");
    p = valid(); p.dsi_power = 0; bad(p, "dsi.inactive_or_geometry");
    p = valid(); p.mctl_overlay = 2; bad(p, "mctl.disconnected");
    p = valid(); p.overlay_end = 0; bad(p, "overlay.geometry_or_disabled");
    p = valid(); p.dfc_padding = 1; bad(p, "dfc.cropped_or_padded");
    p = valid(); p.layer_config[0] = 0x101; bad(p, "overlay.no_routed_fullscreen_layer");
    p = valid(); p.channel_select = 0x08; bad(p, "overlay.no_routed_fullscreen_layer");
    p = valid(); p.channel_overlay_enable = 0x100; bad(p, "overlay.no_routed_fullscreen_layer");
    p = valid(); p.layer_position[0] = 1; bad(p, "overlay.no_routed_fullscreen_layer");
    p = valid(); p.dma.control = 0; p.dma.stride = 135; p.dma.x1 = 134;
    assert(!rejection(p));
    p = valid(); p.dma.address = kLogoBase + kLogoBytes - 4320 * 2280;
    assert(!rejection(p));
}
