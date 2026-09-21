// tests/vulkan_transport_probe.cc — host regression for the native Vulkan
// session transport: the client (dango/vulkan.h), the per-client broker
// (vulkan_broker.h) and a *reference* execution backend, wired over an
// in-memory ring. It proves the transport end to end without the guest:
//
//   * one logical session per client, each with its own arena and quota
//     accounting, and no cross-session object access,
//   * host-visible allocation out of the session arena,
//   * a recorded bounded command stream (fill + copy) executed offscreen, with
//     the result validated after a sync wait — the Phase N acceptance shape,
//   * WSI: surface/swapchain with a DRAW rasterised into an acquired image and a
//     present that is a *bounded copy* into the "Dango shared buffer" — the
//     Phase O acceptance shape.
//
// The backend here is a reference stand-in, NOT lavapipe: it executes the small
// command vocabulary deterministically so the transport, quotas, framing and
// sync semantics are exercised for real. The guest `dango-vkd` backend and its
// lavapipe build are separate (see linux-artifacts/vkd and scripts/linux).

#include "dango/vulkan.h"
#include "vulkan_broker.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

using dango::vk::CommandRecorder;
using dango::vk::Memory;
using dango::vk::SessionClient;
using dango::vk::Transport;
using drv::gpu::linux::VulkanBackend;
using drv::gpu::linux::VulkanSessionBroker;

constexpr mx_u32_t kArenaBytes = MX_VK_SESSION_DEFAULT_ARENA_BYTES;
constexpr mx_u64_t kGeneration = 0x0000ABCDEFull;

// A fixed-capacity SPSC frame queue: the ring the client and the broker share.
struct FrameQueue {
    static constexpr mx_u32_t kCap = MX_VK_SESSION_SLOTS;
    MxVkFrame slots[kCap];
    mx_u32_t head = 0;
    mx_u32_t tail = 0;
    bool push(const MxVkFrame& frame) {
        if (head - tail >= kCap) return false;
        slots[head % kCap] = frame;
        ++head;
        return true;
    }
    bool pop(MxVkFrame* out) {
        if (tail == head) return false;
        *out = slots[tail % kCap];
        ++tail;
        return true;
    }
};

// One client endpoint: its two rings, its arena and its client identity.
struct Rig {
    FrameQueue request;
    FrameQueue reply;
    mx_u8_t arena[kArenaBytes];
    mx_u32_t client_id = 0;
    VulkanSessionBroker* broker = nullptr;
};

bool rig_push(void* ctx, const MxVkFrame& slot) {
    auto* rig = static_cast<Rig*>(ctx);
    if (!rig->request.push(slot)) return false;
    // The service runs "concurrently": pump every queued request through the
    // broker now, so the reply is ready before the client pops.
    MxVkFrame request{};
    while (rig->request.pop(&request)) {
        MxVkFrame reply{};
        if (!rig->broker->handle(rig->client_id, request, &reply)) return false;
        if (!rig->reply.push(reply)) return false;
    }
    return true;
}

bool rig_pop(void* ctx, MxVkFrame* out) { return static_cast<Rig*>(ctx)->reply.pop(out); }
bool rig_wait(void* ctx) {
    (void)ctx;
    return true; // replies are already queued by rig_push
}
mx_u8_t* rig_map_arena(void* ctx, mx_u64_t session_id, mx_u32_t bytes) {
    (void)session_id;
    (void)bytes;
    return static_cast<Rig*>(ctx)->arena;
}

Transport make_transport(Rig* rig) {
    Transport transport{};
    transport.ctx = rig;
    transport.push = &rig_push;
    transport.pop = &rig_pop;
    transport.wait = &rig_wait;
    transport.map_arena = &rig_map_arena;
    return transport;
}

// --- Reference backend ------------------------------------------------------
//
// A deterministic software "GPU": it executes the bounded command vocabulary
// over the session arena and copies a swapchain image into a fixed shared
// buffer. This is the stand-in for the guest dango-vkd; the wire protocol it
// answers is exactly the one the real backend must answer.
struct RefBackend {
    struct Map {
        bool used = false;
        mx_u64_t session_id = 0;
        mx_u32_t client_id = 0;
        mx_u8_t* arena = nullptr;
        mx_u32_t bytes = 0;
        mx_u64_t last_sync = 0;
    };
    Map maps[MX_VK_SESSION_MAX_SESSIONS];
    mx_u8_t* client_arenas[MX_VK_SESSION_MAX_SESSIONS] = {};
    mx_u8_t shared[MX_VK_SHARED_BUFFER_MAX_BYTES] = {};
    mx_u32_t shared_bytes = 0;
    mx_u32_t submit_calls = 0;
    mx_u32_t present_calls = 0;
    // Bound render target, set by VK_CMD_BIND_RENDER_TARGET.
    mx_u64_t rt_offset = 0;
    mx_u32_t rt_w = 0;
    mx_u32_t rt_h = 0;
    mx_u32_t rt_stride = 0;

    Map* find(mx_u64_t session_id) {
        for (auto& m : maps) {
            if (m.used && m.session_id == session_id) return &m;
        }
        return nullptr;
    }

    static void fill(mx_u8_t* dst, mx_u64_t size, mx_u32_t value) {
        for (mx_u64_t i = 0; i < size; ++i) dst[i] = static_cast<mx_u8_t>(value >> ((i & 3) * 8));
    }

    // A right triangle with vertices (0,0), (w-1,0), (0,h-1): interior points
    // satisfy x*(h-1) + y*(w-1) < (w-1)*(h-1).
    void raster_triangle(mx_u8_t* target, mx_u32_t color) {
        if (rt_w == 0 || rt_h == 0) return;
        const mx_u64_t a = rt_w - 1;
        const mx_u64_t b = rt_h - 1;
        for (mx_u32_t y = 0; y < rt_h; ++y) {
            for (mx_u32_t x = 0; x < rt_w; ++x) {
                if ((mx_u64_t)x * b + (mx_u64_t)y * a < a * b) {
                    mx_u8_t* px = target + (mx_u64_t)y * rt_stride + (mx_u64_t)x * 4;
                    px[0] = static_cast<mx_u8_t>(color);
                    px[1] = static_cast<mx_u8_t>(color >> 8);
                    px[2] = static_cast<mx_u8_t>(color >> 16);
                    px[3] = 0;
                }
            }
        }
    }

    mx_status_t execute(mx_u64_t session_id, mx_u32_t command_offset, mx_u32_t command_bytes) {
        Map* map = find(session_id);
        if (!map || !map->arena) return MX_ERR_NOT_FOUND;
        if ((mx_u64_t)command_offset + command_bytes > map->bytes) return MX_ERR_INVALID;
        const mx_u8_t* base = map->arena;
        mx_u32_t off = 0;
        while (off + sizeof(MxVkCommandHeader) <= command_bytes) {
            const auto* header = reinterpret_cast<const MxVkCommandHeader*>(base + command_offset + off);
            if (header->size < sizeof(MxVkCommandHeader) || header->size > command_bytes - off) {
                return MX_ERR_BAD_PROTOCOL;
            }
            switch (header->opcode) {
            case VK_CMD_FILL_BUFFER: {
                const auto* cmd = reinterpret_cast<const MxVkCmdFillBuffer*>(header);
                if (cmd->dest_arena_offset + cmd->offset + cmd->size > map->bytes) return MX_ERR_INVALID;
                fill(map->arena + cmd->dest_arena_offset + cmd->offset, cmd->size, cmd->value);
                break;
            }
            case VK_CMD_COPY_BUFFER: {
                const auto* cmd = reinterpret_cast<const MxVkCmdCopyBuffer*>(header);
                if (cmd->src_arena_offset + cmd->size > map->bytes ||
                    cmd->dst_arena_offset + cmd->size > map->bytes) {
                    return MX_ERR_INVALID;
                }
                memcpy(map->arena + cmd->dst_arena_offset, map->arena + cmd->src_arena_offset,
                       cmd->size);
                break;
            }
            case VK_CMD_BIND_RENDER_TARGET: {
                const auto* cmd = reinterpret_cast<const MxVkCmdBindRenderTarget*>(header);
                rt_offset = cmd->arena_offset;
                rt_w = cmd->width;
                rt_h = cmd->height;
                rt_stride = cmd->stride;
                break;
            }
            case VK_CMD_DRAW: {
                const auto* cmd = reinterpret_cast<const MxVkCmdDraw*>(header);
                if (cmd->vertex_count >= 3) {
                    raster_triangle(map->arena + rt_offset, (mx_u32_t)cmd->color_xrgb8888);
                }
                break;
            }
            case VK_CMD_PIPELINE_BARRIER:
            case VK_CMD_DISPATCH:
            case VK_CMD_BIND_VERTEX_BUFFER:
                break; // accepted, no reference side effect
            default:
                return MX_ERR_BAD_PROTOCOL;
            }
            off += header->size;
        }
        return MX_OK;
    }
};

bool ref_open(void* ctx, mx_u64_t session_id, mx_u32_t client_id, mx_u32_t arena_bytes,
              mx_u32_t* flags_out) {
    auto* ref = static_cast<RefBackend*>(ctx);
    RefBackend::Map* slot = nullptr;
    for (auto& m : ref->maps) {
        if (!m.used) {
            slot = &m;
            break;
        }
    }
    if (!slot || client_id >= MX_VK_SESSION_MAX_SESSIONS) return false;
    slot->used = true;
    slot->session_id = session_id;
    slot->client_id = client_id;
    slot->arena = ref->client_arenas[client_id];
    // Never claim more usable bytes than the backing buffer actually has, even
    // if the broker (testing quota capping) granted more.
    slot->bytes = arena_bytes < kArenaBytes ? arena_bytes : kArenaBytes;
    slot->last_sync = 0;
    *flags_out = MX_VK_SESSION_FLAG_ARENA_READY | MX_VK_SESSION_FLAG_VULKAN_LIVE |
                 MX_VK_SESSION_FLAG_WSI;
    return true;
}

void ref_close(void* ctx, mx_u64_t session_id) {
    auto* ref = static_cast<RefBackend*>(ctx);
    if (auto* m = ref->find(session_id)) *m = RefBackend::Map{};
}

mx_status_t ref_submit(void* ctx, mx_u64_t session_id, mx_u32_t command_offset,
                       mx_u32_t command_bytes, mx_u64_t sync_id) {
    auto* ref = static_cast<RefBackend*>(ctx);
    ++ref->submit_calls;
    const mx_status_t rc = ref->execute(session_id, command_offset, command_bytes);
    if (mx_err(rc)) return rc;
    if (auto* m = ref->find(session_id)) m->last_sync = sync_id;
    return MX_OK;
}

mx_status_t ref_sync_wait(void* ctx, mx_u64_t session_id, mx_u64_t sync_id, mx_u64_t /*timeout*/) {
    auto* ref = static_cast<RefBackend*>(ctx);
    RefBackend::Map* m = ref->find(session_id);
    if (!m) return MX_ERR_NOT_FOUND;
    return sync_id <= m->last_sync ? MX_OK : MX_ERR_BUSY;
}

mx_status_t ref_present(void* ctx, mx_u64_t session_id, mx_u64_t /*swapchain_id*/,
                        mx_u32_t /*image_index*/, mx_u64_t image_offset, mx_u32_t image_bytes,
                        mx_u32_t* copied_out) {
    auto* ref = static_cast<RefBackend*>(ctx);
    ++ref->present_calls;
    RefBackend::Map* m = ref->find(session_id);
    if (!m || !m->arena) return MX_ERR_NOT_FOUND;
    mx_u32_t copied = image_bytes;
    if (copied > MX_VK_SHARED_BUFFER_MAX_BYTES) copied = MX_VK_SHARED_BUFFER_MAX_BYTES;
    if (image_offset + copied > m->bytes) return MX_ERR_INVALID;
    // The bounded copy into the Dango shared buffer (spec §15.4).
    memcpy(ref->shared, m->arena + image_offset, copied);
    ref->shared_bytes = copied;
    *copied_out = copied;
    return MX_OK;
}

VulkanBackend make_ref_backend(RefBackend* ref) {
    VulkanBackend backend{};
    backend.ctx = ref;
    backend.open = &ref_open;
    backend.close = &ref_close;
    backend.submit = &ref_submit;
    backend.sync_wait = &ref_sync_wait;
    backend.present = &ref_present;
    return backend;
}

// Send one raw request to a broker and hand back the reply frame.
MxVkFrame raw_call(VulkanSessionBroker& broker, mx_u32_t client_id, mx_u32_t opcode,
                   const void* payload, mx_u32_t bytes) {
    MxVkFrame request{};
    mx_vk_slot_encode(&request, opcode, 1, payload, bytes);
    MxVkFrame reply{};
    const bool handled = broker.handle(client_id, request, &reply);
    assert(handled);
    return reply;
}

// Read one XRGB8888 pixel's red channel out of a little-endian buffer.
mx_u8_t red_at(const mx_u8_t* base, mx_u32_t stride, mx_u32_t x, mx_u32_t y) {
    return base[(mx_u64_t)y * stride + (mx_u64_t)x * 4 + 2];
}

} // namespace

int main() {
    static RefBackend ref;
    static Rig rig_a;
    static Rig rig_b;
    rig_a.client_id = 0;
    rig_b.client_id = 1;
    ref.client_arenas[0] = rig_a.arena;
    ref.client_arenas[1] = rig_b.arena;

    static VulkanSessionBroker broker;
    broker.init(make_ref_backend(&ref), kGeneration);
    rig_a.broker = &broker;
    rig_b.broker = &broker;

    // --- Session A: open, allocate, record, submit, sync, validate ----------
    SessionClient a;
    a.attach(make_transport(&rig_a));
    assert(a.open(/*client_id=*/0, kArenaBytes, /*max_objects=*/8, /*max_outstanding=*/4));
    assert(a.open_ok());
    const mx_u32_t flags = a.info().flags;
    assert((flags & MX_VK_SESSION_FLAG_ARENA_READY) != 0);
    assert((flags & MX_VK_SESSION_FLAG_VULKAN_LIVE) != 0);
    assert((flags & MX_VK_SESSION_FLAG_WSI) != 0);
    assert(broker.session_count() == 1);

    Memory src{};
    Memory dst{};
    assert(a.alloc(4096, 0, MX_VK_MEMORY_TYPE_HOST_VISIBLE, &src));
    assert(a.alloc(4096, 0, MX_VK_MEMORY_TYPE_HOST_VISIBLE, &dst));
    assert(src.valid() && dst.valid() && src.ptr && dst.ptr);
    assert(src.id != dst.id);
    assert(src.arena_offset >= MX_VK_COMMAND_REGION_BYTES);

    // The client writes host-visible memory directly through its arena view.
    for (mx_u32_t i = 0; i < 256; ++i) src.ptr[i] = static_cast<mx_u8_t>((i * 7 + 3) & 0xFF);

    CommandRecorder& recorder = a.recorder();
    // 0xABABABAB fills every byte with 0xAB (the pattern is a 32-bit word).
    assert(recorder.fill_buffer(src, 0, 64, 0xABABABABu));
    assert(recorder.copy_buffer(src, 0, dst, 0, 256));
    assert(!recorder.overflowed());
    const mx_u32_t recorded = recorder.size();
    assert(recorded >= sizeof(MxVkCmdFillBuffer) + sizeof(MxVkCmdCopyBuffer));

    mx_status_t status = MX_ERR_DEAD;
    assert(a.submit(/*sync_id=*/1, &status) && status == MX_OK);
    assert(recorder.empty()); // consumed by the submit
    assert(a.sync_wait(1, 0, &status) && status == MX_OK);
    // Offscreen validation: the fill landed and the copy carried it through.
    for (mx_u32_t i = 0; i < 64; ++i) assert(dst.ptr[i] == 0xAB);
    for (mx_u32_t i = 64; i < 256; ++i) {
        assert(dst.ptr[i] == static_cast<mx_u8_t>((i * 7 + 3) & 0xFF));
    }
    assert(dst.ptr[256] == 0); // nothing beyond the copy was touched

    // --- WSI: surface, swapchain, acquired image, bounded present ----------
    mx_u64_t surface = 0;
    assert(a.surface_create(64, 64, MX_GPU_PIXFMT_XRGB8888, &surface) && surface != 0);
    mx_u64_t swapchain = 0;
    mx_u32_t image_bytes = 0;
    assert(a.swapchain_create(surface, /*image_count=*/2, /*stride=*/64 * 4,
                              MX_GPU_PIXFMT_XRGB8888, &swapchain, &image_bytes) &&
           swapchain != 0 && image_bytes == 64 * 4 * 64);

    mx_u32_t image_index = 0;
    Memory image{};
    assert(a.swapchain_acquire(swapchain, &image_index, &image, &image_bytes) && image.ptr);
    // Clear, then draw a triangle into the bound render target.
    assert(a.recorder().fill_buffer(image, 0, image_bytes, 0));
    assert(a.recorder().bind_render_target(image, 64, 64, 64 * 4));
    assert(a.recorder().draw(/*vertex_count=*/3, /*instance_count=*/1, 0x00FF0000));
    assert(a.submit(/*sync_id=*/2, &status) && status == MX_OK);
    assert(a.sync_wait(2, 0, &status) && status == MX_OK);

    mx_u32_t copied = 0;
    assert(a.swapchain_present(swapchain, image_index, /*full_frame=*/true, &copied));
    assert(copied == image_bytes); // the bounded copy moved the whole image
    assert(ref.shared_bytes == image_bytes);
    // Inside the triangle the pixel was painted red; outside it stayed clear.
    assert(red_at(ref.shared, 64 * 4, 10, 10) == 0xFF);
    assert(red_at(ref.shared, 64 * 4, 60, 60) == 0x00);

    // --- Isolation: client B cannot touch client A's session ---------------
    SessionClient b;
    b.attach(make_transport(&rig_b));
    assert(b.open(/*client_id=*/1, kArenaBytes, 8, 4));
    assert(broker.session_count() == 2);
    assert(!b.free(src)); // B's session does not own A's memory object

    // A raw ALLOC naming A's session, sent as client 1, is refused.
    MxVkSessionAllocRequest steal{};
    steal.session_id = a.session_id();
    steal.size = 64;
    steal.memory_type = MX_VK_MEMORY_TYPE_HOST_VISIBLE;
    MxVkFrame refused = raw_call(broker, /*client_id=*/1, VK_SESSION_ALLOC, &steal, sizeof(steal));
    assert(refused.type == VK_SESSION_ERROR);

    // --- Quota: the broker caps what a client asks for ---------------------
    SessionClient c;
    static Rig rig_c;
    rig_c.client_id = 2;
    rig_c.broker = &broker;
    ref.client_arenas[2] = rig_c.arena;
    c.attach(make_transport(&rig_c));
    assert(c.open(/*client_id=*/2, /*arena_bytes=*/0xFFFFFFFFu, /*max_objects=*/1000,
                  /*max_outstanding=*/1000));
    assert(c.info().arena_bytes == MX_VK_SESSION_MAX_ARENA_BYTES);
    assert(c.info().max_objects == MX_VK_SESSION_MAX_OBJECTS);
    assert(c.info().max_outstanding == MX_VK_SESSION_MAX_OUTSTANDING);

    // --- A default backend brokers sessions but refuses to fake work -------
    static VulkanSessionBroker bare;
    bare.init(VulkanBackend{}, kGeneration);
    static Rig rig_d;
    rig_d.client_id = 0;
    rig_d.broker = &bare;
    SessionClient d;
    d.attach(make_transport(&rig_d));
    assert(d.open(0, kArenaBytes, 8, 4));
    assert((d.info().flags & MX_VK_SESSION_FLAG_ARENA_READY) == 0);
    MxVkSessionAllocRequest lonely{};
    lonely.session_id = d.session_id();
    lonely.size = 64;
    lonely.memory_type = MX_VK_MEMORY_TYPE_HOST_VISIBLE;
    MxVkFrame no_arena = raw_call(bare, 0, VK_SESSION_ALLOC, &lonely, sizeof(lonely));
    assert(no_arena.type == VK_SESSION_ERROR);
    MxVkSessionError err{};
    assert(mx_vk_slot_decode(&no_arena, VK_SESSION_ERROR, sizeof(err), &err) == 0);
    assert(err.status == MX_ERR_SERVICE_UNAVAILABLE);

    // --- Teardown -----------------------------------------------------------
    assert(a.close() && b.close() && c.close() && d.close());
    assert(ref.submit_calls == 2);
    assert(ref.present_calls == 1);

    std::printf("vulkan session transport probe ok\n");
    return 0;
}
