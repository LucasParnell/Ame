/*
 * gpud_main.c — dango-gpud, the guest-side GPU protocol server (spec §14).
 *
 * It owns `/dev/dango-channel0` and serves the Midix GPU protocol (spec §13)
 * over its rings, driving the assigned virtio-gpu through raw DRM/KMS ioctls:
 *
 *   Phase H control plane : GPU_HELLO / GPU_ENUM_OUTPUTS / GPU_GET_MODES
 *   Phase I present path  : GPU_BUFFER_REGISTER / GPU_BUFFER_UNREGISTER /
 *                           GPU_SET_MODE / GPU_PRESENT -> GPU_PRESENT_COMPLETE
 *   Phase M import probe  : decide once whether scanout can read a Dango shared
 *                           region directly (imported dma-buf) or must copy it
 *                           into a dumb buffer; the copy is the fallback.
 *
 * The server is event driven. It blocks in the channel `read` doorbell when it
 * has nothing to do, and only ever uses a bounded poll while the guest -> host
 * ring is genuinely full (backpressure), never a spin.
 *
 * Objects (outputs, modes, buffers) are opaque 64-bit ids scoped to the
 * channel's boot_generation and live in fixed-capacity tables: there is no
 * allocation anywhere on the request path. Every id and every staging range is
 * validated before it is used, and no DRM handle is ever exposed to the host —
 * a raw DRM ioctl can never be forwarded.
 *
 * Relies on the agent's global memcpy/memset (cross-TU link contract).
 */
#include <stdint.h>

#include "gpud_channel.h"
#include "gpud_drm.h"
#include "gpud_protocol.h"

#include "midix/abi/dango_channel_guest.h"
#include "midix/abi/dango_shm_guest.h"
#include "midix/abi/linux_guest_console.h"
#include "midix/abi/linux_guest_syscall.h"

void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);

/* Positive syscall errors arrive negated; EINTR is a retry, not a failure. */
#define GPUD_EINTR 4

/* Bootstrap handoff. The host's `drv.gpu.linux` client drives one control-plane
 * session ending in one staged present, then reuses the channel for the
 * boot-time KMS and transport self-tests. Once a present has completed and the
 * host has been quiet for this grace window, the server returns so the caller
 * can hand the DRM card and the channel back to them. The wait is a bounded
 * poll (a real sleep), never a spin. */
#define GPUD_HANDOFF_GRACE_MS 20

/* --- Server state --------------------------------------------------------- */

struct gpud_output {
    uint32_t used;
    uint64_t id;
    uint32_t drm_connector_id;
    uint32_t drm_encoder_id;
    uint32_t drm_crtc_id;
    uint32_t type;
    uint32_t connection;
    uint32_t width_mm;
    uint32_t height_mm;
    uint32_t mode_first;
    uint32_t mode_count;
    char name[8];
};

struct gpud_mode {
    uint32_t used;
    uint64_t id;
    uint32_t output_slot;
    struct drm_mode_modeinfo info;
};

struct gpud_buffer {
    uint32_t used;
    uint64_t id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t flags;
    uint64_t staging_off;
    uint64_t staging_bytes;
    struct gdrm_dumb dumb;
    /* Phase M: when non-zero the scanout object is an imported dma-buf over the
     * shared region, so `dumb` is unused and the present copies nothing. */
    int imported;
    uint32_t import_handle;
    uint32_t fb_id;
    /* CRTC state needed to restore scanout when present finds it dropped. */
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t has_mode;
    struct drm_mode_modeinfo mode;
};

struct gpud_pending {
    uint64_t seq;
    uint32_t buffer_slot;
    uint32_t flags;
};

static struct gpud_channel g_channel;
static struct gdrm_card g_card;
static int g_drm_ready;
static uint64_t g_generation;

static struct gpud_output g_outputs[GPUD_MAX_OUTPUTS];
static struct gpud_mode g_modes[GPUD_MAX_MODES];
static struct gpud_buffer g_buffers[GPUD_MAX_BUFFERS];
static uint32_t g_output_count;

static struct gpud_pending g_pending[GPUD_MAX_PENDING];
static uint32_t g_pending_count;
static uint32_t g_presents_completed;

/* Phase M import state. The verdict is decided once, by the real
 * PRIME_FD_TO_HANDLE call, so a present never re-decides and the hot path stays
 * allocation- and syscall-free. */
enum gpud_import_verdict {
    GPUD_IMPORT_UNAVAILABLE = 0, /* no shared-region device / no live region */
    GPUD_IMPORT_NOT_SHARED,      /* region is not SharedGpu-registered (non-DMA) */
    GPUD_IMPORT_REJECTED,        /* the DRM device refuses a foreign dma-buf */
    GPUD_IMPORT_AVAILABLE,       /* imported: the bounded copy can be dropped */
};

static int g_shm_fd = -1;
static uint32_t g_import_verdict;
static struct dango_shm_region g_import_region;
static unsigned long long g_import_boot_generation;
static unsigned long g_import_cap_prime;
static unsigned long g_import_errno;

static int gpud_generation_ok(uint64_t generation) {
    return generation == g_generation;
}

/* --- Object table --------------------------------------------------------- */

static void gpud_objects_build(void) {
    uint32_t out_index;
    uint32_t mode_index = 0;

    memset(g_outputs, 0, sizeof(g_outputs));
    memset(g_modes, 0, sizeof(g_modes));
    g_output_count = 0;

    for (out_index = 0; out_index < g_card.output_count && out_index < GPUD_MAX_OUTPUTS;
         ++out_index) {
        const struct gdrm_output *source = &g_card.outputs[out_index];
        struct gpud_output *output = &g_outputs[out_index];
        uint32_t i;

        output->used = 1;
        /* Ids are 1-based so that a null (0) id can never name an object. */
        output->id = gpud_object_id(g_generation, MX_GPU_OBJECT_OUTPUT, out_index + 1);
        output->drm_connector_id = source->connector_id;
        output->drm_encoder_id = source->encoder_id;
        output->drm_crtc_id = source->crtc_id;
        output->type = source->type;
        output->connection = source->connection;
        output->width_mm = source->width_mm;
        output->height_mm = source->height_mm;
        output->mode_first = mode_index;
        output->mode_count = 0;
        memcpy(output->name, source->name, sizeof(output->name));

        for (i = 0; i < source->mode_count && mode_index < GPUD_MAX_MODES; ++i, ++mode_index) {
            struct gpud_mode *mode = &g_modes[mode_index];
            mode->used = 1;
            mode->id = gpud_object_id(g_generation, MX_GPU_OBJECT_MODE, mode_index + 1);
            mode->output_slot = out_index;
            mode->info = source->modes[i];
        }
        output->mode_count = mode_index - output->mode_first;
        ++g_output_count;
    }
}

static struct gpud_output *gpud_output_lookup(uint64_t id, uint32_t *slot_out) {
    uint32_t index;
    struct gpud_output *output;
    if (gpud_object_index(id, MX_GPU_OBJECT_OUTPUT, g_generation, &index) != 0 ||
        index > GPUD_MAX_OUTPUTS) {
        return 0;
    }
    output = &g_outputs[index - 1];
    if (!output->used || output->id != id) {
        return 0;
    }
    if (slot_out) {
        *slot_out = index - 1;
    }
    return output;
}

static struct gpud_mode *gpud_mode_lookup(uint64_t id) {
    uint32_t index;
    struct gpud_mode *mode;
    if (gpud_object_index(id, MX_GPU_OBJECT_MODE, g_generation, &index) != 0 ||
        index > GPUD_MAX_MODES) {
        return 0;
    }
    mode = &g_modes[index - 1];
    if (!mode->used || mode->id != id) {
        return 0;
    }
    return mode;
}

static struct gpud_buffer *gpud_buffer_lookup(uint64_t id, uint32_t *slot_out) {
    uint32_t index;
    struct gpud_buffer *buffer;
    if (gpud_object_index(id, MX_GPU_OBJECT_BUFFER, g_generation, &index) != 0 ||
        index > GPUD_MAX_BUFFERS) {
        return 0;
    }
    buffer = &g_buffers[index - 1];
    if (!buffer->used || buffer->id != id) {
        return 0;
    }
    if (slot_out) {
        *slot_out = index - 1;
    }
    return buffer;
}

/* --- Shared-region import (Phase M) -------------------------------------- */

/* One-line verdict, written once per run so the host can assert the documented
 * assumption without parsing the whole boot log. When the assumption is stale
 * the line carries the shared failure vocabulary, which fails the acceptance
 * rows and flags that zero-copy can now be switched on. */
static void gpud_import_report(void) {
    char buf[128];
    char *p = mx_append_str(buf, "dango-gpud: zero-copy probe cap=");
    p = mx_append_u32(p, (unsigned int)g_import_cap_prime);
    p = mx_append_str(p, " verdict=");
    switch (g_import_verdict) {
    case GPUD_IMPORT_NOT_SHARED:
        p = mx_append_str(p, "region-not-sharedgpu copy=retained\n");
        break;
    case GPUD_IMPORT_REJECTED:
        p = mx_append_str(p, "import-rejected errno=");
        p = mx_append_u32(p, (unsigned int)g_import_errno);
        p = mx_append_str(p, " copy=retained\n");
        break;
    case GPUD_IMPORT_AVAILABLE:
        /* The documented reason for keeping the copy no longer holds. Fail the
         * run loudly rather than weakening the assertion; the display still
         * works, and the zero-copy switch is revisited deliberately. */
        p = mx_append_str(p, "import-available zero-copy TEST FAILED (stale assumption)\n");
        break;
    default:
        p = mx_append_str(p, "unavailable copy=retained\n");
        break;
    }
    (void)mx_write(1, buf, (unsigned long)(p - buf));
}

/* Export `region` as a dma-buf and import it as a GEM handle. Returns 0 on
 * success, else the negative errno that refused it (-MX_EINVAL when the export
 * itself failed). */
static long gpud_region_import(const struct dango_shm_region *region,
                               unsigned long long boot_generation, uint32_t *handle_out) {
    struct dango_shm_export exp;
    long rc;

    if (g_shm_fd < 0) {
        return -MX_EINVAL;
    }
    memset(&exp, 0, sizeof(exp));
    exp.id = region->id;
    exp.boot_generation = boot_generation;
    if (mx_ioctl(g_shm_fd, DANGO_SHM_EXPORT_DMABUF, &exp) < 0 || exp.fd < 0) {
        return -MX_EINVAL;
    }
    rc = gdrm_prime_import(&g_card, (int)exp.fd, handle_out);
    (void)mx_close((int)exp.fd);
    return rc;
}

/* Decide, once, whether scanout can read the shared region directly instead of
 * copying from it. The answer is the real PRIME_FD_TO_HANDLE result: the
 * DRM_CAP_PRIME bit cannot gate it (since Linux 6.6 every driver advertises
 * import) and is not consulted here. */
static void gpud_import_probe(void) {
    struct dango_shm_query query;
    uint32_t handle = 0;
    long rc;

    g_import_verdict = GPUD_IMPORT_UNAVAILABLE;
    g_import_errno = 0;
    g_import_cap_prime = 0;
    if (!g_drm_ready) {
        gpud_import_report();
        return;
    }
    /* Record the advertised capability for evidence: since Linux 6.6 the kernel
     * reports import|export for every driver, so it can never gate the import;
     * only the PRIME_FD_TO_HANDLE call below can. */
    g_import_cap_prime = (unsigned long)gdrm_get_cap(&g_card, GDRM_CAP_PRIME);
    if (g_shm_fd < 0) {
        gpud_import_report();
        return;
    }
    memset(&query, 0, sizeof(query));
    query.index = 0;
    if (mx_ioctl(g_shm_fd, DANGO_SHM_QUERY, &query) < 0 || query.count == 0 ||
        query.region.size == 0) {
        gpud_import_report();
        return;
    }
    g_import_region = query.region;
    g_import_boot_generation = query.boot_generation;
    if (!(query.region.flags & DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU)) {
        /* Non-DMA by design: the host has not registered it SharedGpu. */
        g_import_verdict = GPUD_IMPORT_NOT_SHARED;
        gpud_import_report();
        return;
    }
    rc = gpud_region_import(&query.region, query.boot_generation, &handle);
    if (rc == 0) {
        g_import_verdict = GPUD_IMPORT_AVAILABLE;
        gdrm_gem_close(&g_card, handle); /* capability probe only */
    } else {
        /* The documented case: the DRM device refuses a foreign dma-buf. */
        g_import_verdict = GPUD_IMPORT_REJECTED;
        g_import_errno = (unsigned long)(-rc);
    }
    gpud_import_report();
}

/* Use an imported scanout object for `buffer` when the request names the probed
 * region and matches its geometry. Returns 0 when the imported path is taken,
 * -1 when the caller must fall back to the dumb buffer + bounded copy. */
static int gpud_buffer_import(struct gpud_buffer *buffer,
                              const struct MxGpuBufferRegisterRequest *in) {
    uint32_t handle = 0;

    if (g_import_verdict != GPUD_IMPORT_AVAILABLE || in->shm_id != g_import_region.id ||
        in->boot_generation != g_import_boot_generation ||
        (uint64_t)in->stride * in->height != g_import_region.size) {
        return -1;
    }
    if (gpud_region_import(&g_import_region, g_import_boot_generation, &handle) != 0) {
        return -1;
    }
    buffer->imported = 1;
    buffer->import_handle = handle;
    return 0;
}

/* --- Handlers ------------------------------------------------------------- */

static void gpud_handle_hello(const struct dango_ring_slot *request,
                              struct dango_ring_slot *reply) {
    const struct MxGpuHello *hello;
    struct MxGpuHelloReply out;
    const void *payload;

    if (gpud_frame_decode(request, GPU_HELLO, (uint32_t)sizeof(*hello), &payload) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    hello = (const struct MxGpuHello *)payload;
    memset(&out, 0, sizeof(out));
    /* Version negotiation: an incompatible major still gets our version back,
     * so the client can decide whether to fall back. */
    out.status = hello->major == MX_GPU_VERSION_MAJOR ? MX_OK : MX_ERR_BAD_PROTOCOL;
    out.major = MX_GPU_VERSION_MAJOR;
    out.minor = MX_GPU_VERSION_MINOR;
    out.flags = MX_GPU_HELLO_FLAG_NONE;
    out.feature_bits = MX_GPU_FEATURE_STAGING_BUFFER | MX_GPU_FEATURE_ASYNC_PRESENT;
    out.boot_generation = g_generation;
    gpud_frame_reply(reply, GPU_HELLO, request->seq, &out, (uint32_t)sizeof(out));
}

static void gpud_handle_enum_outputs(const struct dango_ring_slot *request,
                                     struct dango_ring_slot *reply) {
    const struct MxGpuEnumOutputsRequest *in;
    struct MxGpuEnumOutputsReply out;
    const struct gpud_output *output;
    const void *payload;

    if (gpud_frame_decode(request, GPU_ENUM_OUTPUTS, (uint32_t)sizeof(*in), &payload) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    in = (const struct MxGpuEnumOutputsRequest *)payload;
    if (!gpud_generation_ok(in->boot_generation)) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->boot_generation);
        return;
    }
    /* No device object exists in the bootstrap profile, so the only valid
     * device reference is the implicit 0; anything else names nothing. */
    if (in->device_id != 0) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, in->device_id, g_generation, 0);
        return;
    }
    if (!g_drm_ready) {
        gpud_frame_error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, 0, g_generation, 0);
        return;
    }
    if (in->index >= g_output_count) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, 0, g_generation, in->index);
        return;
    }
    output = &g_outputs[in->index];
    memset(&out, 0, sizeof(out));
    out.status = MX_OK;
    out.output_id = output->id;
    out.output_count = g_output_count;
    out.output_type = output->type;
    out.connection = output->connection;
    out.width_mm = output->width_mm;
    out.height_mm = output->height_mm;
    out.flags = 0;
    memcpy(out.name, output->name, sizeof(out.name));
    gpud_frame_reply(reply, GPU_ENUM_OUTPUTS, request->seq, &out, (uint32_t)sizeof(out));
}

static void gpud_handle_get_modes(const struct dango_ring_slot *request,
                                  struct dango_ring_slot *reply) {
    const struct MxGpuGetModesRequest *in;
    struct MxGpuGetModesReply out;
    const struct gpud_output *output;
    const struct gpud_mode *mode;
    const void *payload;

    if (gpud_frame_decode(request, GPU_GET_MODES, (uint32_t)sizeof(*in), &payload) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    in = (const struct MxGpuGetModesRequest *)payload;
    if (!gpud_generation_ok(in->boot_generation)) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->boot_generation);
        return;
    }
    output = gpud_output_lookup(in->output_id, 0);
    if (!output) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, in->output_id, g_generation, 0);
        return;
    }
    if (in->index >= output->mode_count) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, output->id, g_generation, in->index);
        return;
    }
    mode = &g_modes[output->mode_first + in->index];
    memset(&out, 0, sizeof(out));
    out.status = MX_OK;
    out.mode_id = mode->id;
    out.mode_count = output->mode_count;
    out.width = mode->info.hdisplay;
    out.height = mode->info.vdisplay;
    out.refresh_mhz = mode->info.vrefresh * 1000u;
    out.pixel_format = MX_GPU_PIXFMT_XRGB8888;
    out.flags = 0;
    memcpy(out.name, mode->info.name, sizeof(out.name));
    gpud_frame_reply(reply, GPU_GET_MODES, request->seq, &out, (uint32_t)sizeof(out));
}

static void gpud_handle_buffer_register(const struct dango_ring_slot *request,
                                        struct dango_ring_slot *reply) {
    const struct MxGpuBufferRegisterRequest *in;
    struct MxGpuBufferRegisterReply out;
    struct gpud_buffer *buffer = 0;
    uint64_t bytes;
    uint32_t slot;
    const void *payload;

    if (gpud_frame_decode(request, GPU_BUFFER_REGISTER, (uint32_t)sizeof(*in), &payload) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    in = (const struct MxGpuBufferRegisterRequest *)payload;
    if (!gpud_generation_ok(in->boot_generation) || in->reserved0 != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->boot_generation);
        return;
    }
    if (g_channel.staging_len == 0) {
        /* No staging window was published: the register path is unavailable. */
        gpud_frame_error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, 0, g_generation, 0);
        return;
    }
    if (in->pixel_format != MX_GPU_PIXFMT_XRGB8888) {
        gpud_frame_error(reply, request, MX_ERR_BAD_TYPE, 0, g_generation, in->pixel_format);
        return;
    }
    if ((in->flags & ~(uint32_t)(MX_GPU_BUFFER_FLAG_STAGING | MX_GPU_BUFFER_FLAG_SCANOUT)) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->flags);
        return;
    }
    if (in->width == 0 || in->height == 0 || (in->stride & 3u) != 0 ||
        (uint64_t)in->stride < (uint64_t)in->width * 4u) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    /* The staging range must lie entirely inside the published window; the
     * offset is pixel aligned so every row maps to a valid pixel. */
    bytes = (uint64_t)in->stride * in->height;
    if ((in->shm_id & 3u) != 0 || in->shm_id > g_channel.staging_len ||
        bytes > g_channel.staging_len - in->shm_id) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->shm_id);
        return;
    }

    for (slot = 0; slot < GPUD_MAX_BUFFERS; ++slot) {
        if (!g_buffers[slot].used) {
            buffer = &g_buffers[slot];
            break;
        }
    }
    if (!buffer) {
        gpud_frame_error(reply, request, MX_ERR_NOMEM, 0, g_generation, 0);
        return;
    }

    memset(buffer, 0, sizeof(*buffer));
    buffer->used = 1;
    buffer->id = gpud_object_id(g_generation, MX_GPU_OBJECT_BUFFER, slot + 1);
    buffer->width = in->width;
    buffer->height = in->height;
    buffer->stride = in->stride;
    buffer->format = in->pixel_format;
    buffer->flags = in->flags;
    buffer->staging_off = in->shm_id;
    buffer->staging_bytes = bytes;
    /* Phase M: prefer scanout read straight from the imported shared region;
     * fall back to a dumb buffer + bounded copy when import is unavailable. */
    if (gpud_buffer_import(buffer, in) != 0 &&
        gdrm_dumb_create(&g_card, in->width, in->height, 32, &buffer->dumb) != 0) {
        memset(buffer, 0, sizeof(*buffer));
        gpud_frame_error(reply, request, MX_ERR_FAULT, 0, g_generation, 0);
        return;
    }

    memset(&out, 0, sizeof(out));
    out.status = MX_OK;
    out.buffer_id = buffer->id;
    out.boot_generation = g_generation;
    gpud_frame_reply(reply, GPU_BUFFER_REGISTER, request->seq, &out, (uint32_t)sizeof(out));
}

static void gpud_handle_buffer_unregister(const struct dango_ring_slot *request,
                                          struct dango_ring_slot *reply) {
    const struct MxGpuBufferUnregisterRequest *in;
    struct MxGpuBufferUnregisterReply out;
    struct gpud_buffer *buffer;
    const void *payload;

    if (gpud_frame_decode(request, GPU_BUFFER_UNREGISTER, (uint32_t)sizeof(*in), &payload) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    in = (const struct MxGpuBufferUnregisterRequest *)payload;
    if (!gpud_generation_ok(in->boot_generation)) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->boot_generation);
        return;
    }
    buffer = gpud_buffer_lookup(in->buffer_id, 0);
    if (!buffer) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, in->buffer_id, g_generation, 0);
        return;
    }
    /* Drop any queued present for this buffer first: its dumb buffer and mmap
     * are about to disappear. */
    {
        uint32_t i = 0;
        while (i < g_pending_count) {
            if (g_pending[i].buffer_slot == (uint32_t)(buffer - g_buffers)) {
                uint32_t j;
                for (j = i; j + 1 < g_pending_count; ++j) {
                    g_pending[j] = g_pending[j + 1];
                }
                --g_pending_count;
            } else {
                ++i;
            }
        }
    }
    if (buffer->fb_id != 0) {
        gdrm_fb_destroy(&g_card, buffer->fb_id);
    }
    if (buffer->imported) {
        gdrm_gem_close(&g_card, buffer->import_handle);
    } else {
        if (buffer->dumb.map) {
            (void)mx_munmap(buffer->dumb.map, buffer->dumb.map_len);
        }
        if (buffer->dumb.handle != 0) {
            gdrm_dumb_destroy(&g_card, buffer->dumb.handle);
        }
    }
    memset(buffer, 0, sizeof(*buffer));

    memset(&out, 0, sizeof(out));
    out.status = MX_OK;
    out.boot_generation = g_generation;
    gpud_frame_reply(reply, GPU_BUFFER_UNREGISTER, request->seq, &out, (uint32_t)sizeof(out));
}

static void gpud_handle_set_mode(const struct dango_ring_slot *request,
                                 struct dango_ring_slot *reply) {
    const struct MxGpuSetModeRequest *in;
    struct MxGpuSetModeReply out;
    struct gpud_output *output;
    struct gpud_mode *mode;
    struct gpud_buffer *buffer;
    const void *payload;

    if (gpud_frame_decode(request, GPU_SET_MODE, (uint32_t)sizeof(*in), &payload) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    in = (const struct MxGpuSetModeRequest *)payload;
    if (!gpud_generation_ok(in->boot_generation) || in->reserved0 != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->boot_generation);
        return;
    }
    if (!g_drm_ready) {
        gpud_frame_error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, 0, g_generation, 0);
        return;
    }
    output = gpud_output_lookup(in->output_id, 0);
    if (!output) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, in->output_id, g_generation, 0);
        return;
    }
    mode = gpud_mode_lookup(in->mode_id);
    if (!mode) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, in->mode_id, g_generation, 0);
        return;
    }
    buffer = gpud_buffer_lookup(in->buffer_id, 0);
    if (!buffer) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, in->buffer_id, g_generation, 0);
        return;
    }
    /* Cross-object invariants: the mode must belong to the named output, the
     * buffer must match the mode's dimensions, and the output needs a CRTC. */
    if (mode->output_slot != (uint32_t)(output - g_outputs) ||
        buffer->width != mode->info.hdisplay || buffer->height != mode->info.vdisplay ||
        output->drm_crtc_id == 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, output->id, g_generation, 0);
        return;
    }
    if (buffer->fb_id == 0) {
        int fb_ok = buffer->imported
                        ? gdrm_fb_create_handle(&g_card, buffer->import_handle, buffer->width,
                                                buffer->height, buffer->stride, 32, 24,
                                                &buffer->fb_id)
                        : gdrm_fb_create(&g_card, &buffer->dumb, &buffer->fb_id);
        if (fb_ok != 0) {
            gpud_frame_error(reply, request, MX_ERR_FAULT, buffer->id, g_generation, 0);
            return;
        }
    }
    if (gdrm_crtc_program(&g_card, output->drm_crtc_id, output->drm_connector_id, buffer->fb_id,
                          &mode->info) != 0) {
        gpud_frame_error(reply, request, MX_ERR_FAULT, output->id, g_generation, 0);
        return;
    }
    buffer->crtc_id = output->drm_crtc_id;
    buffer->connector_id = output->drm_connector_id;
    buffer->mode = mode->info;
    buffer->has_mode = 1;

    memset(&out, 0, sizeof(out));
    out.status = MX_OK;
    out.framebuffer_id = buffer->fb_id;
    out.boot_generation = g_generation;
    gpud_frame_reply(reply, GPU_SET_MODE, request->seq, &out, (uint32_t)sizeof(out));
}

static void gpud_handle_present(const struct dango_ring_slot *request,
                                struct dango_ring_slot *reply) {
    const struct MxGpuPresentRequest *in;
    struct MxGpuPresentReply out;
    struct gpud_buffer *buffer;
    uint32_t slot;
    const void *payload;

    if (gpud_frame_decode(request, GPU_PRESENT, (uint32_t)sizeof(*in), &payload) != 0) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, 0);
        return;
    }
    in = (const struct MxGpuPresentRequest *)payload;
    if (!gpud_generation_ok(in->boot_generation)) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, 0, g_generation, in->boot_generation);
        return;
    }
    buffer = gpud_buffer_lookup(in->buffer_id, &slot);
    if (!buffer) {
        gpud_frame_error(reply, request, MX_ERR_NOT_FOUND, in->buffer_id, g_generation, 0);
        return;
    }
    if (buffer->fb_id == 0 || (in->framebuffer_id != 0 && in->framebuffer_id != buffer->fb_id)) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, buffer->id, g_generation,
                         in->framebuffer_id);
        return;
    }
    if (in->damage_count > MX_GPU_MAX_INLINE_DAMAGE ||
        (in->damage_count == 1 &&
         (in->damage.width == 0 || in->damage.height == 0 ||
          (uint64_t)in->damage.x + in->damage.width > buffer->width ||
          (uint64_t)in->damage.y + in->damage.height > buffer->height))) {
        gpud_frame_error(reply, request, MX_ERR_INVALID, buffer->id, g_generation, in->damage_count);
        return;
    }
    if (g_pending_count >= GPUD_MAX_PENDING) {
        gpud_frame_error(reply, request, MX_ERR_BUSY, buffer->id, g_generation, g_pending_count);
        return;
    }

    /* Enqueue only: the completion is a separate GPU_PRESENT_COMPLETE frame,
     * published after this acknowledgement, carrying the request's seq. */
    g_pending[g_pending_count].seq = request->seq;
    g_pending[g_pending_count].buffer_slot = slot;
    g_pending[g_pending_count].flags = in->flags;
    ++g_pending_count;

    memset(&out, 0, sizeof(out));
    out.status = MX_OK;
    out.buffer_id = buffer->id;
    out.boot_generation = g_generation;
    gpud_frame_reply(reply, GPU_PRESENT, request->seq, &out, (uint32_t)sizeof(out));
}

/* --- Dispatch ------------------------------------------------------------- */

static int gpud_dispatch(const struct dango_ring_slot *request) {
    struct dango_ring_slot reply;

    /* Opcodes this profile does not implement are answered, never ignored: a
     * reserved future opcode reports NOT_IMPLEMENTED, anything else is not part
     * of the GPU protocol at all. */
    if (!gpud_opcode_supported(request->type)) {
        int reserved = request->type >= MX_GPU_OPCODE_FUTURE_BASE &&
                       request->type <= MX_GPU_OPCODE_FUTURE_LIMIT;
        gpud_frame_error(&reply, request, reserved ? MX_ERR_NOT_IMPLEMENTED : MX_ERR_BAD_PROTOCOL,
                         0, g_generation, request->type);
        return gpud_channel_publish(&g_channel, &reply) == 0 ? 0 : -1;
    }

    switch (request->type) {
    case GPU_HELLO:
        gpud_handle_hello(request, &reply);
        break;
    case GPU_ENUM_OUTPUTS:
        gpud_handle_enum_outputs(request, &reply);
        break;
    case GPU_GET_MODES:
        gpud_handle_get_modes(request, &reply);
        break;
    case GPU_BUFFER_REGISTER:
        gpud_handle_buffer_register(request, &reply);
        break;
    case GPU_BUFFER_UNREGISTER:
        gpud_handle_buffer_unregister(request, &reply);
        break;
    case GPU_SET_MODE:
        gpud_handle_set_mode(request, &reply);
        break;
    case GPU_PRESENT:
        gpud_handle_present(request, &reply);
        break;
    default:
        /* Unreachable while gpud_opcode_supported() and this switch agree; the
         * error frame keeps a future drift from silently dropping a request. */
        gpud_frame_error(&reply, request, MX_ERR_BAD_PROTOCOL, 0, g_generation, request->type);
        break;
    }
    return gpud_channel_publish(&g_channel, &reply) == 0 ? 0 : -1;
}

/* Drain every queued host request. Stops without consuming when the guest ->
 * host ring is full so nothing is lost: the request is retried next iteration. */
static int gpud_drain_requests(void) {
    struct dango_ring_slot request;
    int served = 0;

    while (gpud_channel_peek(&g_channel, &request)) {
        if (gpud_channel_full(&g_channel)) {
            break;
        }
        if (gpud_dispatch(&request) != 0) {
            return -1;
        }
        gpud_channel_consume(&g_channel);
        served = 1;
    }
    return served;
}

/* --- Present completion --------------------------------------------------- */

static void gpud_pending_pop(void) {
    uint32_t i;
    for (i = 0; i + 1 < g_pending_count; ++i) {
        g_pending[i] = g_pending[i + 1];
    }
    if (g_pending_count) {
        --g_pending_count;
    }
}

/* Copy the registered staging buffer into its dumb buffer, restore scanout if
 * the CRTC stopped scanning it, then publish GPU_PRESENT_COMPLETE. */
static int gpud_present_finish(const struct gpud_pending *pending) {
    struct gpud_buffer *buffer = &g_buffers[pending->buffer_slot];
    struct MxGpuPresentComplete out;
    struct dango_ring_slot frame;
    const unsigned char *source;
    unsigned char *dest;
    uint64_t row_bytes;
    uint32_t y;
    int64_t status = MX_OK;

    /* The damage feature bit is not advertised, so a present always refreshes
     * the whole framebuffer; the rects above are validated but not clipped. */
    row_bytes = buffer->stride < buffer->dumb.pitch ? buffer->stride : buffer->dumb.pitch;
    if (buffer->imported) {
        /* Phase M: the imported object *is* the shared region the host writes,
         * so scanout reads it directly and no bytes move. The barrier orders
         * the region writes before the completion the host waits on. */
        __asm__ volatile("dmb ish" ::: "memory");
    } else if (row_bytes == 0 || buffer->staging_off + buffer->staging_bytes > g_channel.staging_len ||
               (uint64_t)buffer->height * row_bytes > buffer->dumb.map_len) {
        status = MX_ERR_FAULT;
    } else {
        source = g_channel.staging + buffer->staging_off;
        dest = buffer->dumb.map;
        for (y = 0; y < buffer->height; ++y) {
            memcpy(dest + (unsigned long)y * buffer->dumb.pitch,
                   source + (unsigned long)y * buffer->stride, row_bytes);
        }
        /* Order the pixels before the completion the host waits on. */
        __asm__ volatile("dmb ish" ::: "memory");
    }

    if (status == MX_OK && buffer->has_mode && buffer->crtc_id != 0) {
        int scanning = gdrm_crtc_scanning(&g_card, buffer->crtc_id, buffer->fb_id);
        if (scanning <= 0 &&
            gdrm_crtc_program(&g_card, buffer->crtc_id, buffer->connector_id, buffer->fb_id,
                              &buffer->mode) != 0) {
            status = MX_ERR_FAULT;
        }
    }

    // Writes to a mapped dumb buffer do not themselves upload pixels to a
    // virtio scanout resource. Ask the Linux DRM driver to flush every present,
    // including when the framebuffer and CRTC have not changed.
    if (status == MX_OK && gdrm_fb_flush(&g_card, buffer->fb_id) != 0) {
        status = MX_ERR_FAULT;
    }

    memset(&out, 0, sizeof(out));
    out.status = status;
    out.buffer_id = buffer->id;
    out.boot_generation = g_generation;
    out.flags = pending->flags;
    gpud_frame_reply(&frame, GPU_PRESENT_COMPLETE, pending->seq, &out, (uint32_t)sizeof(out));
    if (gpud_channel_publish(&g_channel, &frame) != 0) {
        /* Ring full: leave the entry queued and retry on the next iteration. */
        return 0;
    }
    if (status == MX_OK) {
        ++g_presents_completed;
        mx_puts("dango-gpud: present complete\n");
    }
    return 1;
}

static int gpud_complete_pending(void) {
    int progress = 0;
    while (g_pending_count) {
        if (gpud_channel_full(&g_channel)) {
            break;
        }
        if (!gpud_present_finish(&g_pending[0])) {
            break;
        }
        gpud_pending_pop();
        progress = 1;
    }
    return progress;
}

/* --- Serve loop ----------------------------------------------------------- */

static void gpud_serve(int bootstrap_handoff) {
    for (;;) {
        int progress = 0;
        int drained;

        /* Completions run before new requests, so a present is always
         * acknowledged in an earlier iteration than it completes. */
        progress |= gpud_complete_pending();
        drained = gpud_drain_requests();
        if (drained < 0) {
            return; /* terminal guest -> host failure */
        }
        progress |= drained > 0;
        if (progress) {
            continue;
        }
        if (bootstrap_handoff && g_presents_completed != 0 && g_pending_count == 0) {
            /* Session complete and nothing queued: wait out the handoff grace
             * for a further request, then return to the caller. */
            long idle = gpud_channel_wait_timeout(&g_channel, GPUD_HANDOFF_GRACE_MS);
            if (idle == 0) {
                return;
            }
            if (idle < 0 && idle != -GPUD_EINTR) {
                return;
            }
            continue;
        }
        if (gpud_channel_full(&g_channel)) {
            /* Unsent frames with no host activity: bounded backpressure wait. */
            (void)gpud_channel_wait_timeout(&g_channel, 2);
            continue;
        }
        {
            long woken = gpud_channel_wait(&g_channel);
            if (woken < 0 && woken != -GPUD_EINTR) {
                return;
            }
        }
    }
}

static int gpud_drm_init(void) {
    if (gdrm_card_open(&g_card, DANGO_DEV_DRM_CARD0) != 0) {
        return -1;
    }
    if (gdrm_card_enumerate(&g_card) != 0) {
        gdrm_card_close(&g_card);
        return -1;
    }
    g_drm_ready = 1;
    gpud_objects_build();
    return 0;
}

int dango_gpud_run(void) {
    if (gpud_channel_open(&g_channel) != 0) {
        mx_puts("dango-gpud: channel open failed\n");
        return -1;
    }
    g_generation = g_channel.layout.desc.boot_generation;
    g_drm_ready = 0;
    memset(g_buffers, 0, sizeof(g_buffers));
    g_pending_count = 0;
    g_presents_completed = 0;
    g_import_verdict = GPUD_IMPORT_UNAVAILABLE;
    if (gpud_drm_init() != 0) {
        mx_puts("dango-gpud: DRM outputs unavailable\n");
    }
    /* Phase M: decide once whether scanout can read a shared region directly.
     * Absent device/region is not fatal: the present path falls back to the
     * bounded copy, which is the only path virtio_gpu supports today. */
    g_shm_fd = (int)mx_openat(MX_AT_FDCWD, DANGO_DEV_SHM, MX_O_RDWR, 0);
    gpud_import_probe();

    mx_puts("dango-gpud: ready\n");
    gpud_serve(1);

    /* Hand the appliance back: release DRM master but keep the card open so the
     * presented framebuffer stays on the CRTC, and close the channel so the
     * boot-time self-tests can open it. Both happen whether the loop ended by a
     * completed session or by a terminal transport error. */
    // The shared-region fd and registered buffers survive the self-test handoff.
    const int released = gdrm_card_release_master(&g_card);
    if (released != 0) {
        mx_puts("dango-gpud: release master FAILED\n");
    }
    gpud_channel_close(&g_channel);
    return g_presents_completed != 0 && released == 0 ? 0 : -1;
}

/* Resume the same registered buffers and ids after KMS/channel self-tests.
 * There is one channel owner at a time; steady-state service has no idle exit. */
int dango_gpud_resume(void) {
    if (!g_drm_ready || gpud_channel_open(&g_channel) != 0) {
        return -1;
    }
    if (g_channel.layout.desc.boot_generation != g_generation ||
        gdrm_card_acquire_master(&g_card) != 0) {
        gpud_channel_close(&g_channel);
        return -1;
    }
    mx_puts("dango-gpud: serving native frames\n");
    gpud_serve(0);
    if (gdrm_card_release_master(&g_card) != 0) {
        mx_puts("dango-gpud: release master FAILED\n");
    }
    if (g_shm_fd >= 0) {
        (void)mx_close(g_shm_fd);
        g_shm_fd = -1;
    }
    gpud_channel_close(&g_channel);
    return -1; /* Steady-state service returns only after a transport failure. */
}
