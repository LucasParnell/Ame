#pragma once
#include "mochi/gpu_diag.h"
#include "midix/abi/protocols/graphics.h"
#include "midix/abi/platform.h"
#include "shader_cache.h"
#include "render_clock.h"

// The VM runner is a separate budgeted thread. This control thread sleeps until
// a native client requests a frame, then returns the guest's real completion.
[[noreturn]] static void mali_render_service(const MxBootInfo* boot, CapHandle channel,
                                             RingPage* tx, RingPage* rx, CapHandle h2g_db,
                                             CapHandle g2h_db) {
    using namespace mochi::gpu_diag;
    auto client = mochi::driver::link(boot, "diag", kControlVa);
    auto display = mochi::driver::link(boot, "display", kDisplayVa);
    if (!client.valid() || !display.valid()) {
        fail("Mali render endpoints missing\n");
    }
    console_log("Mali render service: ready\n");
    const auto gpu_control = mochi::boot::handle(boot, "gpu.platform");
    const bool clock_control =
        gpu_control && mx_platform_control(gpu_control, MX_PLATFORM_CLAIM, 0) == 0;
    if (clock_control) {
        dango_counter("gpu.firmware_mhz",
                      mx_platform_control(gpu_control, MX_PLATFORM_GPU_RATE, 0));
    }
    ShaderCache cache{};
    cache.initialize(boot);
    mx_u64_t broken_batch = 0;
    RenderClockLease clock;
    auto rate = [&](mx_u32_t mhz) {
        return mx_platform_control(gpu_control, MX_PLATFORM_GPU_RATE, mhz);
    };
    for (;;) {
        MxRingSlot slot{};
        clock.expire(mx_clock_ns(), rate);
        if (!mochi::driver::receive(client, slot, clock.deadline())) {
            continue;
        }
        if (slot.type == AME_GFX_CHUNK) {
            ame_gfx_chunk chunk{};
            if (!mochi::message_get(slot, &chunk, AME_GFX_CHUNK)) {
                continue;
            }
            if (!ring_try_push(tx, DANGO_CH_DEFAULT_RING_ORDER, AME_GFX_CHUNK, &chunk,
                               sizeof(chunk), h2g_db) ||
                sys_vm_channel_notify(channel)) {
                broken_batch = chunk.id;
            }
            continue;
        }
        if (slot.type == AME_GFX_SUBMIT || slot.type == AME_GFX_SHARED) {
            ame_gfx_submit request{};
            ame_gfx_shared_submit shared{};
            const bool is_shared = slot.type == AME_GFX_SHARED;
            if (is_shared) {
                if (!mochi::message_get(slot, &shared, AME_GFX_SHARED) || shared.reserved ||
                    shared.slot >= AME_GFX_COMMAND_SLOTS) {
                    continue;
                }
                request = shared.request;
            } else if (!mochi::message_get(slot, &request, AME_GFX_SUBMIT)) {
                continue;
            }
            ame_gfx_reply reply{request.id, MX_ERR_SERVICE_UNAVAILABLE, 0, 0, 0, 0, 0, 0};
            const bool streaming = request.flags & AME_GFX_PRESENT_STREAM;
            const bool restoring = request.flags & AME_GFX_PRESENT_RESTORE;
            if (clock_control && ame_gfx_flags_valid(request.flags) &&
                (request.flags & AME_GFX_PRESENT)) {
                clock.begin(mx_clock_ns(), rate);
            }
            if (request.id != broken_batch && request.version == AME_GFX_VERSION &&
                (request.api == AME_GFX_API_GLES2 || request.api == AME_GFX_API_VULKAN) &&
                request.bytes <= AME_GFX_BATCH_BYTES &&
                ame_gfx_flags_valid(request.flags) &&
                ring_try_push(tx, DANGO_CH_DEFAULT_RING_ORDER,
                              is_shared ? AME_GFX_SHARED : AME_GFX_SUBMIT,
                              is_shared ? static_cast<const void*>(&shared)
                                        : static_cast<const void*>(&request),
                              is_shared ? sizeof(shared) : sizeof(request), h2g_db) &&
                !sys_vm_channel_notify(channel)) {
                mochi::SpscEndpoint guest{};
                guest.rx_ring = rx;
                guest.order = DANGO_CH_DEFAULT_RING_ORDER;
                guest.rx_doorbell = g2h_db;
                const auto deadline = mx_clock_ns() + 30000000000ull;
                while (mochi::driver::receive(guest, slot, deadline)) {
                    ame_gfx_display_drain drain{};
                    if (mochi::message_get(slot, &drain, AME_GFX_DISPLAY_DRAIN)) {
                        ame_gpu_diag_request restore{drain.id, AME_GPU_DIAG_WIDTH,
                                                     AME_GPU_DIAG_HEIGHT, AME_GPU_DIAG_DURATION_MS,
                                                     AME_GPU_DIAG_RESTORE};
                        ame_gpu_diag_reply restored{};
                        drain.status = !drain.reserved &&
                                               exchange(display, AME_GPU_DIAG_PRESENT, restore,
                                                        restored, mx_clock_ns() + 1000000000ull) &&
                                               !restored.status
                                           ? 0
                                           : MX_ERR_FAULT;
                        drain.reserved = 0;
                        if (!ring_try_push(tx, DANGO_CH_DEFAULT_RING_ORDER,
                                           AME_GFX_DISPLAY_DRAIN_REPLY, &drain, sizeof(drain),
                                           h2g_db) ||
                            sys_vm_channel_notify(channel)) {
                            break;
                        }
                        continue;
                    }
                    ame_gfx_cache_io cache_request{};
                    if (mochi::message_get(slot, &cache_request, AME_GFX_CACHE_REQUEST)) {
                        const auto cache_reply = cache.exchange(cache_request);
                        if (!ring_try_push(tx, DANGO_CH_DEFAULT_RING_ORDER, AME_GFX_CACHE_REPLY,
                                           &cache_reply, sizeof(cache_reply), h2g_db) ||
                            sys_vm_channel_notify(channel)) {
                            break;
                        }
                        continue;
                    }
                    ame_gfx_reply done{};
                    if (mochi::message_get(slot, &done, AME_GFX_REPLY) && done.id == request.id) {
                        reply = done;
                        break;
                    }
                }
                // A static diagnostic waits five seconds in the display service;
                // release its boost before that wait. Streaming keeps the lease.
                if ((request.flags & AME_GFX_PRESENT) && !streaming && !restoring) {
                    clock.finish(rate);
                }
                if (!reply.status && (request.flags & AME_GFX_PRESENT)) {
                    ame_gpu_diag_request present{
                        request.id, AME_GPU_DIAG_WIDTH, AME_GPU_DIAG_HEIGHT,
                        AME_GPU_DIAG_DURATION_MS,
                        (request.flags & AME_GFX_PRESENT_RESTORE)  ? AME_GPU_DIAG_RESTORE
                        : (request.flags & AME_GFX_PRESENT_STREAM) ? AME_GPU_DIAG_STREAM
                                                                   : 0};
                    if (reply.present_target >= AME_GPU_DIAG_TARGETS) {
                        reply.status = MX_ERR_FAULT;
                    }
                    if (reply.present_target == 1) {
                        present.reserved |= AME_GPU_DIAG_SLOT1;
                    }
                    ame_gpu_diag_reply shown{};
                    if (reply.status || reply.gpu_id != 0x830 ||
                        !exchange(display, AME_GPU_DIAG_PRESENT, present, shown,
                                  mx_clock_ns() + 7000000000ull) ||
                        shown.status) {
                        reply.status = MX_ERR_FAULT;
                    } else if (shown.flags & AME_GPU_DIAG_SCANOUT) {
                        reply.scanout_count = static_cast<mx_u32_t>(shown.render_ns);
                    }
                }
            }
            if (reply.status || restoring) {
                clock.finish(rate);
            } else if (clock_control && streaming) {
                clock.begin(mx_clock_ns(), rate);
            }
            (void)client.tx().push_bounded(AME_GFX_REPLY, &reply, sizeof(reply),
                                           mx_clock_ns() + 1000000000ull);
            continue;
        }
        ame_gpu_diag_request request{};
        if (!mochi::message_get(slot, &request, AME_GPU_DIAG_TRIANGLE)) {
            continue;
        }
        clock.finish(rate);
        ame_gpu_diag_reply reply{request.id, MX_ERR_INVALID, 0, 0, 0, 0};
        // The native backend accepts GLES command streams. The obsolete CPU
        // triangle/readback operation cannot honor front/back-buffer ownership.
        reply.status = MX_ERR_NOT_IMPLEMENTED;
        (void)client.tx().push_bounded(AME_GPU_DIAG_REPLY, &reply, sizeof(reply),
                                       mx_clock_ns() + 1000000000ull);
    }
}
