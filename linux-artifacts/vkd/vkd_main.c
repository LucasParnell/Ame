/*
 * vkd_main.c — dango-vkd, the guest-side remote Vulkan executor (spec §14.6/§14.7).
 *
 * dango-vkd is the appliance half of the Phase N transport. It owns the Dango
 * channel (`/dev/dango-channel0`, via the shared gpud_channel transport) and
 * executes the bounded command streams the native GPU service forwards:
 *
 *   VK_SESSION_OPEN            negotiate device liveness (flags)
 *   VK_SESSION_SUBMIT          execute a command stream from the shared arena
 *   VK_SESSION_SYNC_WAIT       acknowledge the client sync token
 *   VK_SESSION_SWAPCHAIN_PRESENT  report the bounded copy into the shared buffer
 *   VK_SESSION_CLOSE           tear the session down
 *
 * The service brokers sessions and owns every object id and quota; dango-vkd is
 * the execution seam and only validates that a command range lies inside the
 * shared arena. Every command operand is an arena byte offset (the vocabulary in
 * midix/abi/protocols/vulkan_session.h), so no id translation happens here.
 *
 * The shared arena is the guest-visible Dango staging window, which the channel
 * already maps. Execution goes through vkd_vulkan_* (Mesa lavapipe when the guest
 * image carries it; otherwise a clean "no device" refusal, never a fake result).
 *
 * Relies on the agent's global memcpy/memset (cross-TU link contract), like
 * dango-gpud.
 */
#include <stdint.h>

#include "vkd_vulkan.h"

#include "gpud_channel.h" /* the shared Dango channel transport (single home) */

#include "midix/abi/linux_guest_console.h"
#include "midix/abi/protocols/vulkan_session.h"

void* memcpy(void* dst, const void* src, unsigned long n);
void* memset(void* dst, int c, unsigned long n);

/* Bounded handoff, identical in spirit to dango-gpud: serve while the host keeps
 * working, then return once it has been quiet for a short grace window so the
 * caller regains the channel. A bounded poll, never a spin. */
#define VKD_HANDOFF_GRACE_MS 20

/* One scratch frame for building replies; the request frame is reused in place. */
static void vkd_reply(struct gpud_channel* channel, uint32_t opcode,
                      const struct dango_ring_slot* request, const void* payload, uint32_t bytes) {
    struct dango_ring_slot slot;
    mx_vk_slot_encode(&slot, opcode, request->seq, payload, bytes);
    (void)gpud_channel_publish(channel, &slot);
}

static void vkd_error(struct gpud_channel* channel, const struct dango_ring_slot* request,
                      mx_status_t status, mx_u64_t session_id, mx_u64_t detail) {
    MxVkSessionError err;
    memset(&err, 0, sizeof(err));
    err.status = status;
    err.session_id = session_id;
    err.detail = detail;
    err.opcode = request->type;
    vkd_reply(channel, VK_SESSION_ERROR, request, &err, (uint32_t)sizeof(err));
}

static void vkd_handle(struct gpud_channel* channel, const struct dango_ring_slot* request,
                       uint8_t* arena, uint32_t arena_bytes) {
    switch (request->type) {
    case VK_SESSION_OPEN: {
        MxVkSessionOpenRequest req;
        if (mx_vk_slot_decode(request, VK_SESSION_OPEN, (uint32_t)sizeof(req), &req) != 0) {
            vkd_error(channel, request, MX_ERR_BAD_PROTOCOL, 0, request->length);
            return;
        }
        MxVkSessionOpenReply rep;
        memset(&rep, 0, sizeof(rep));
        rep.status = MX_OK;
        rep.boot_generation = 0; /* the service stamps the real generation */
        rep.arena_bytes = arena_bytes;
        rep.max_objects = MX_VK_SESSION_MAX_OBJECTS;
        rep.max_outstanding = MX_VK_SESSION_MAX_OUTSTANDING;
        rep.flags = MX_VK_SESSION_FLAG_ARENA_READY;
        if (vkd_vulkan_available()) {
            rep.flags |= MX_VK_SESSION_FLAG_VULKAN_LIVE;
        }
        vkd_reply(channel, VK_SESSION_OPEN_REPLY, request, &rep, (uint32_t)sizeof(rep));
        break;
    }
    case VK_SESSION_SUBMIT: {
        MxVkSessionSubmitRequest req;
        if (mx_vk_slot_decode(request, VK_SESSION_SUBMIT, (uint32_t)sizeof(req), &req) != 0) {
            vkd_error(channel, request, MX_ERR_BAD_PROTOCOL, 0, request->length);
            return;
        }
        if (!vkd_vulkan_available()) {
            vkd_error(channel, request, MX_ERR_SERVICE_UNAVAILABLE, req.session_id, 0);
            return;
        }
        if ((uint64_t)req.command_bytes > MX_VK_COMMAND_REGION_BYTES ||
            (uint64_t)req.command_offset + req.command_bytes > arena_bytes) {
            vkd_error(channel, request, MX_ERR_INVALID, req.session_id, req.command_bytes);
            return;
        }
        if (vkd_vulkan_execute(arena, arena_bytes, req.command_offset, req.command_bytes) != 0) {
            vkd_error(channel, request, MX_ERR_INVALID, req.session_id, req.command_bytes);
            return;
        }
        MxVkSessionSubmitReply rep;
        memset(&rep, 0, sizeof(rep));
        rep.status = MX_OK;
        rep.session_id = req.session_id;
        rep.sync_id = req.sync_id;
        rep.command_bytes = req.command_bytes;
        rep.outstanding = 1;
        vkd_reply(channel, VK_SESSION_SUBMIT_REPLY, request, &rep, (uint32_t)sizeof(rep));
        break;
    }
    case VK_SESSION_SYNC_WAIT: {
        MxVkSessionSyncWaitRequest req;
        if (mx_vk_slot_decode(request, VK_SESSION_SYNC_WAIT, (uint32_t)sizeof(req), &req) != 0) {
            vkd_error(channel, request, MX_ERR_BAD_PROTOCOL, 0, request->length);
            return;
        }
        /* Execution is synchronous per submit, so the token is already reached. */
        MxVkSessionSyncWaitReply rep;
        memset(&rep, 0, sizeof(rep));
        rep.status = MX_OK;
        rep.session_id = req.session_id;
        vkd_reply(channel, VK_SESSION_SYNC_WAIT_REPLY, request, &rep, (uint32_t)sizeof(rep));
        break;
    }
    case VK_SESSION_SWAPCHAIN_PRESENT: {
        MxVkSwapchainPresentRequest req;
        if (mx_vk_slot_decode(request, VK_SESSION_SWAPCHAIN_PRESENT, (uint32_t)sizeof(req), &req) != 0) {
            vkd_error(channel, request, MX_ERR_BAD_PROTOCOL, 0, request->length);
            return;
        }
        uint32_t copied = 0;
        if (vkd_vulkan_present(arena, arena_bytes, &copied) != 0) {
            vkd_error(channel, request, MX_ERR_SERVICE_UNAVAILABLE, req.session_id, 0);
            return;
        }
        MxVkSwapchainPresentReply rep;
        memset(&rep, 0, sizeof(rep));
        rep.status = MX_OK;
        rep.swapchain_id = req.swapchain_id;
        rep.copied_bytes = copied;
        rep.image_index = req.image_index;
        vkd_reply(channel, VK_SESSION_SWAPCHAIN_PRESENT_REPLY, request, &rep, (uint32_t)sizeof(rep));
        break;
    }
    case VK_SESSION_CLOSE: {
        MxVkSessionCloseRequest req;
        if (mx_vk_slot_decode(request, VK_SESSION_CLOSE, (uint32_t)sizeof(req), &req) != 0) {
            vkd_error(channel, request, MX_ERR_BAD_PROTOCOL, 0, request->length);
            return;
        }
        MxVkSessionCloseReply rep;
        memset(&rep, 0, sizeof(rep));
        rep.status = MX_OK;
        rep.session_id = req.session_id;
        vkd_reply(channel, VK_SESSION_CLOSE_REPLY, request, &rep, (uint32_t)sizeof(rep));
        break;
    }
    default:
        vkd_error(channel, request, MX_ERR_BAD_PROTOCOL, 0, request->type);
        break;
    }
}

int dango_vkd_run(void) {
    struct gpud_channel channel;
    if (gpud_channel_open(&channel) != 0) {
        mx_puts("dango-vkd: channel open FAILED\n");
        return 0;
    }

    /* Bind the Vulkan executor. Without VKD_HAVE_VULKAN (or a missing ICD) the
     * executor reports no device and every submit is refused cleanly. */
    if (vkd_vulkan_init() == 0 && vkd_vulkan_available()) {
        mx_puts("dango-vkd: lavapipe device ready\n");
    } else {
        mx_puts("dango-vkd: no Vulkan device; submits will be refused\n");
    }

    if (!channel.staging || channel.staging_len <= MX_VK_COMMAND_REGION_BYTES ||
        channel.staging_len > MX_VK_SESSION_MAX_ARENA_BYTES) {
        mx_puts("dango-vkd: invalid staging arena FAILED\n");
        vkd_vulkan_shutdown();
        gpud_channel_close(&channel);
        return 0;
    }
    uint8_t* arena = channel.staging;
    const uint32_t arena_bytes = (uint32_t)channel.staging_len;
    mx_puts("dango-vkd: serving\n");

    for (;;) {
        struct dango_ring_slot request;
        if (!gpud_channel_full(&channel) && gpud_channel_peek(&channel, &request) == 1) {
            vkd_handle(&channel, &request, arena, arena_bytes);
            gpud_channel_consume(&channel);
            continue;
        }
        /* Nothing pending: sleep on the doorbell for a bounded grace window. */
        if (gpud_channel_wait_timeout(&channel, VKD_HANDOFF_GRACE_MS) <= 0) {
            break; /* quiet long enough: hand the channel back */
        }
    }

    vkd_vulkan_shutdown();
    gpud_channel_close(&channel);
    return 1;
}
