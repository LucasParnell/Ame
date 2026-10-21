// userland/drivers/framebuffer/main.cc — io.framebuffer service (M2).
//
// Owns the physical framebuffer MMIO range granted by the kernel as "fb.memory"
// in BootInfo. Maps it, clears it to a solid color, and serves ring-based
// blit requests from tty.fb0.
//
// Cap policy: holds "fb.memory" CAP_MMIO only. No UART, no log rings direct.
//
// Phase J: when the appliance is present the same drawing path is additionally
// presented to `drv.gpu.linux` (the GPU service hosted by dango.manager). The
// ramfb path is untouched and stays authoritative, so a missing, refused or
// failed appliance degrades to the ordinary local display -- never to no
// display. This service knows nothing about virtio-gpu or DRM.

#include "mochi/syscall.h"
#include "midix/abi/platform.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/framebuffer.h"
#include "midix/font/vga8x16.h"
#include "midix/abi/ring.h"
#include "dango/layout.h"
#include "gpu_linux.h"
#include "mochi/va_layout.h"
#include "mochi/framebuffer_raster.h"
#include "mochi/wait.h"
#include "libk/cpu.h"

// Virtual address layout for io.framebuffer.
using namespace mochi::va::framebuffer;

// Framebuffer dimensions — set per board by CMake (ANKO_FB_WIDTH/HEIGHT).
// QEMU virt: 1024×768.  Switch T210: 1280×720.
#ifndef ANKO_FB_WIDTH
#define ANKO_FB_WIDTH  1024
#endif
#ifndef ANKO_FB_HEIGHT
#define ANKO_FB_HEIGHT 768
#endif
constexpr uint32_t kFbWidth  = ANKO_FB_WIDTH;
constexpr uint32_t kFbHeight = ANKO_FB_HEIGHT;
constexpr uint32_t kFbStride = kFbWidth * 4; // XRGB8888
constexpr uint32_t kRingOrder = MX_DEFAULT_RING_ORDER;
// Bounded startup budget for the appliance handshake. Never a boot dependency:
// if the GPU service is not live within it the terminal keeps drawing to ramfb.
constexpr uint32_t kGpuHandshakeBudget = 256;

// Clear color on startup: dark blue.
constexpr uint32_t kClearColor = 0x001F3F;

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

// Damage accumulated since the last present the appliance accepted. It is
// tracked only when the GPU service endpoint is present, so the ramfb-only
// path (default boot) executes exactly the code it did before. A rectangle is
// half-open and empty when x0 >= x1.
struct DamageRect {
    uint32_t x0 = 0xFFFF'FFFFu;
    uint32_t y0 = 0xFFFF'FFFFu;
    uint32_t x1 = 0;
    uint32_t y1 = 0;

    bool empty() const { return x0 >= x1 || y0 >= y1; }
    void reset() { x0 = 0xFFFF'FFFFu; y0 = 0xFFFF'FFFFu; x1 = 0; y1 = 0; }
    void whole() { x0 = 0; y0 = 0; x1 = kFbWidth; y1 = kFbHeight; }

    // Union in one rectangle, clipped to the framebuffer. An untrusted width is
    // clamped before it is added so it can never wrap the extent.
    void add(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        if (width == 0 || height == 0 || x >= kFbWidth || y >= kFbHeight) return;
        const uint32_t cx1 = x + min_u32(width, kFbWidth - x);
        const uint32_t cy1 = y + min_u32(height, kFbHeight - y);
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (cx1 > x1) x1 = cx1;
        if (cy1 > y1) y1 = cy1;
    }

    MxGpuDamageRect rect() const {
        return MxGpuDamageRect{x0, y0, x1 - x0, y1 - y0};
    }
};

// The pixel span one glyph command paints, using the same clamps as the drawer.
static uint32_t glyph_span_width(const FbDrawGlyphCmd& cmd) {
    return min_u32(cmd.width, 8) * (cmd.scale_x ? min_u32(cmd.scale_x, 8) : 1);
}

static uint32_t glyph_span_height(const FbDrawGlyphCmd& cmd) {
    return min_u32(cmd.height, 16) * (cmd.scale_y ? min_u32(cmd.scale_y, 8) : 1);
}

static void send_info(const mochi::framebuffer::Server& endpoint) {
    const FbInfo info{
        kFbWidth,
        kFbHeight,
        kFbStride,
        FB_FORMAT_XRGB8888,
        0,
        {0, 0, 0},
    };
    (void)endpoint.send_info(info);
}

// Everything one io.framebuffer service thread owns: the mapped pixels, the tty
// endpoint, the damage accumulator, and (when the appliance is present) the
// armed GPU service client. `gpu` is null on the default (ramfb-only) path.
struct ServerState {
    mochi::framebuffer::Raster raster;
    const mochi::framebuffer::Server* endpoint;
    drv::gpu::linux::Client* gpu = nullptr;
    bool gpu_force_full = false;
    DamageRect damage;
};

// Push the frame the appliance must show now. Returns without effect when the
// GPU service is not live, so the local display is always the floor.
static void present_frame(ServerState& state) {
    if (!state.gpu) return;
    state.gpu->poll();
    if (!state.gpu->live()) return;
    if (!state.gpu_force_full && state.damage.empty()) return;
    // This path also runs after draining a batch without FB_PRESENT. Publish
    // Normal NC pixel stores before sending the appliance a present request.
    libk::cpu::data_sync();
    if (state.gpu_force_full) {
        // The first present after the handshake is always a whole frame, so a
        // stale appliance scanout (the bootstrap placeholder) cannot survive.
        if (state.gpu->submit_present(nullptr, true)) {
            state.gpu_force_full = false;
            state.damage.reset();
        }
        return;
    }
    const MxGpuDamageRect rect = state.damage.rect();
    if (state.gpu->submit_present(&rect, false)) {
        state.damage.reset();
    }
}

static bool handle_fb_command(ServerState& state, const RingSlot& slot) {
    const auto& raster = state.raster;
    const mochi::framebuffer::Server& endpoint = *state.endpoint;
    DamageRect* damage = state.gpu ? &state.damage : nullptr;
    if (!mochi::message_valid(slot)) return false;
    switch (slot.type) {
    case FB_INFO:
        send_info(endpoint);
        return true;
    case FB_CLEAR:
        if (slot.length >= sizeof(uint32_t)) {
            const uint32_t color = *reinterpret_cast<const uint32_t*>(slot.payload);
            raster.clear(color);
            if (damage) damage->whole();
        }
        return true;
    case FB_FILL_RECT:
        if (slot.length >= sizeof(FbFillRectCmd)) {
            const auto& cmd = *reinterpret_cast<const FbFillRectCmd*>(slot.payload);
            raster.fill_rect(cmd.x, cmd.y, cmd.width, cmd.height, cmd.color);
            if (damage) damage->add(cmd.x, cmd.y, cmd.width, cmd.height);
        }
        return true;
    case FB_DRAW_GLYPH:
        if (slot.length >= sizeof(FbDrawGlyphCmd)) {
            const auto& cmd = *reinterpret_cast<const FbDrawGlyphCmd*>(slot.payload);
            raster.draw_glyph(cmd);
            if (damage) damage->add(cmd.x, cmd.y, glyph_span_width(cmd), glyph_span_height(cmd));
        }
        return true;
    case FB_DRAW_TEXT:
        if (slot.length >= sizeof(FbDrawTextCmd)) {
            const auto& cmd = *reinterpret_cast<const FbDrawTextCmd*>(slot.payload);
            raster.draw_text(cmd);
            if (damage) {
                const uint32_t count = min_u32(cmd.len, static_cast<uint32_t>(sizeof(cmd.text)));
                const uint32_t scale_x = cmd.scale_x ? min_u32(cmd.scale_x, 8) : 1;
                const uint32_t scale_y = cmd.scale_y ? min_u32(cmd.scale_y, 8) : 1;
                damage->add(cmd.x, cmd.y,
                            count * midix::font::kVga8x16Width * scale_x,
                            midix::font::kVga8x16Height * scale_y);
            }
        }
        return true;
    case FB_COPY_RECT:
        if (slot.length >= sizeof(FbCopyRectCmd)) {
            const auto& cmd = *reinterpret_cast<const FbCopyRectCmd*>(slot.payload);
            raster.copy_rect(cmd);
            if (damage) damage->add(cmd.dst_x, cmd.dst_y,
                                    min_u32(cmd.width, cmd.src_x < kFbWidth ? kFbWidth - cmd.src_x : 0),
                                    min_u32(cmd.height, cmd.src_y < kFbHeight ? kFbHeight - cmd.src_y : 0));
        }
        return true;
    case FB_PRESENT:
        // The ramfb store is normal RAM; the barrier orders the pixels before
        // the appliance (and the display) reads them.
        libk::cpu::data_sync();
        present_frame(state);
        return true;
    default:
        return false;
    }
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);

    (void)mochi::log_map_from_boot(boot, kVaLogRing, "io.framebuffer");

    // Locate the framebuffer memory arena from BootInfo.
    mx_handle_t fb_arena = mochi::boot::handle(boot, "fb.memory");
    if (fb_arena == MX_NULL_HANDLE) {
        mochi::log_raw_marker("io.framebuffer: no fb.memory in BootInfo\n");
        mochi::park();
    }

    // Map the framebuffer pixel arena (CAP_MMIO, NORMAL_NC).
    const mx_status_t map_rc = mx_map_mmio(fb_arena, kVaFbPixels,
                                           MX_RIGHT_READ | MX_RIGHT_WRITE);
    if (map_rc != MX_OK) {
        mochi::log_raw_marker("io.framebuffer: fb.memory map failed\n");
        mochi::park();
    }

    uint64_t pixel_offset = 0;
    if (const auto* entry = mochi::boot::find(boot, "fb.offset")) pixel_offset = entry->aux0;
    MxCapInfo extent{};
    if(mx_cap_info(fb_arena,&extent)!=MX_OK||extent.attr!=MX_MMIO_NORMAL_NC||
       pixel_offset >= 4096 || (pixel_offset & 3) || pixel_offset > extent.size ||
       extent.size-pixel_offset<static_cast<uint64_t>(kFbWidth)*kFbHeight*4) mochi::boot::invalid_boot();
    if(const auto control=mochi::boot::handle(boot,"fb.control")) {
        if(mx_platform_control(control,MX_PLATFORM_CLAIM)!=MX_OK||
           mx_platform_control(control,MX_PLATFORM_DISPLAY_HANDOFF)!=MX_OK) mochi::boot::invalid_boot();
    }
    const mochi::framebuffer::Raster raster{reinterpret_cast<uint32_t*>(kVaFbPixels + pixel_offset),
                                           kFbWidth, kFbHeight};
    raster.clear(kClearColor);
    libk::cpu::data_sync();

    mochi::log_raw_marker("io.framebuffer: ready\n");

    const mochi::framebuffer::Server endpoint{
        mochi::boot::map_ring(boot, {"fb.cmd.ring", kVaCmdRing, "fb.cmd.doorbell"}),
        mochi::boot::map_ring(boot, {"fb.reply.ring", kVaReplyRing, "fb.reply.doorbell"}),
    };
    if (!endpoint.cmd.valid() || endpoint.cmd.doorbell == MX_NULL_HANDLE) {
        mochi::log_raw_marker("io.framebuffer: no command endpoint\n");
    }

    // Phase J: arm the GPU service client when init handed us the endpoint and
    // this framebuffer really is the appliance staging geometry. On every other
    // boot (the default, and switch-t210's 1280x720 panel) the ramfb path
    // stands alone, unchanged.
    drv::gpu::linux::Client gpu_client;
    bool gpu_armed = false;
    {
        const CapHandle gpu_req = mochi::boot::handle(boot, "gpu.cmd.ring");
        const CapHandle gpu_reply = mochi::boot::handle(boot, "gpu.reply.ring");
        const CapHandle gpu_req_db = mochi::boot::handle(boot, "gpu.cmd.doorbell");
        const CapHandle gpu_reply_db = mochi::boot::handle(boot, "gpu.reply.doorbell");
        const bool geometry_ok =
            kFbWidth == dango::kGpuModeWidth &&
            kFbHeight == dango::kGpuModeHeight &&
            kFbStride == dango::kGpuModeWidth * dango::kGpuModeBytesPerPixel;
        if (gpu_req && gpu_reply && gpu_req_db && gpu_reply_db && geometry_ok &&
            mx_map_page(gpu_req, kVaGpuReqRing, MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK &&
            mx_map_page(gpu_reply, kVaGpuReplyRing, MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
            const drv::gpu::linux::Endpoint gpu_ep{
                reinterpret_cast<RingPage*>(kVaGpuReqRing),
                reinterpret_cast<RingPage*>(kVaGpuReplyRing),
                kRingOrder,
                gpu_req_db,
                gpu_reply_db,
            };
            // The window we declare is the appliance staging binding, read from
            // its single home (the shared Dango binding). The service refuses a
            // client whose window disagrees with its own.
            MxGpuServiceRegisterStagingRequest window{};
            window.staging_pa = dango::kStagingGuestIpa;
            window.staging_size = dango::kStagingSize;
            window.width = dango::kGpuModeWidth;
            window.height = dango::kGpuModeHeight;
            window.stride = dango::kGpuModeWidth * dango::kGpuModeBytesPerPixel;
            window.pixel_format = MX_GPU_PIXFMT_XRGB8888;
            window.flags = MX_GPU_SERVICE_BUFFER_FLAG_STAGING;
            gpu_client.attach(gpu_ep, window);
            gpu_armed = gpu_client.attached();
        }
        for (uint32_t i = 0; gpu_armed && i < kGpuHandshakeBudget; ++i) {
            if (gpu_client.live()) break;
            gpu_client.poll();
            mx_yield();
        }
        // Bounded: if the service is late (or absent) we boot on and present
        // opportunistically as the terminal works; the local path is unaffected.
    }
    if (gpu_armed) mochi::log_raw_marker("io.framebuffer: gpu backend armed\n");

    // The first present after arming is always a whole frame, so nothing drawn
    // before the handshake completed can be lost on the appliance.
    ServerState state{raster, &endpoint, gpu_armed ? &gpu_client : nullptr, gpu_armed, {}};

    mochi::boot::ready(boot);
    while (true) {
        if (!endpoint.cmd.valid() || endpoint.cmd.doorbell == MX_NULL_HANDLE) {
            mx_yield();
            continue;
        }

        RingSlot slot{};
        while (endpoint.try_recv(&slot)) {
            (void)handle_fb_command(state, slot);
        }
        // Flush any damage that arrived while the service was still coming up.
        present_frame(state);
        if (endpoint.wait_recv(&slot)) {
            (void)handle_fb_command(state, slot);
        } else {
            mx_yield();
        }
    }
}
