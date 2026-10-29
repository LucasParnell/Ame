#pragma once
/* Synthetic Mali appliance binding. These are IPAs, never physical resources. */
#define AME_MALI_GUEST_BASE 0x60000000ULL
#define AME_MALI_REG_BYTES 0x4000U
#define AME_MALI_JOB_IRQ 49U
#define AME_MALI_MMU_IRQ 50U
#define AME_MALI_GPU_IRQ 51U

/*
 * Render service (guest side).
 *
 * The guest renders into its own staging window and presents through the
 * appliance's existing GPU_PRESENT path. Commands are self-contained records
 * that fit one ring slot payload, so no guest pointer, shader binary or host
 * physical address ever crosses the channel.
 *
 * The rasterizer behind this is shared/include/midix/render/raster.h and runs
 * on the guest CPU. `AME_RENDER_CAPS_GPU_JOBS` is clear until a job really goes
 * to the Mali, so a caller can tell software rendering from executed GPU work.
 *
 * Every struct is sized to one MX_RING_INLINE_PAYLOAD slot (48 bytes) and is
 * asserted below; a command that needs more must be split, not spilled.
 */
#define AME_RENDER_HELLO 0x47520001U
#define AME_RENDER_CLEAR 0x47520002U
#define AME_RENDER_TRIANGLE 0x47520003U
#define AME_RENDER_REPLY 0x47520004U
/* Ask the service to publish everything drawn so far to the panel. */
#define AME_RENDER_PRESENT 0x47520005U

#define AME_RENDER_VERSION 1U
#define AME_RENDER_MAX_WIDTH 1024U
#define AME_RENDER_MAX_HEIGHT 768U

/* Reply status values. Negative mirrors the Midix error convention. */
#define AME_RENDER_OK 0
#define AME_RENDER_ERR_INVALID (-1)
#define AME_RENDER_ERR_STATE (-2)
#define AME_RENDER_ERR_RANGE (-3)

/* ame_render_reply.flags */
#define AME_RENDER_CAPS_GPU_JOBS (1U << 0) /* set only if Mali executed the work */
#define AME_RENDER_CAPS_STAGING (1U << 1)  /* a staging buffer is registered */
#define AME_RENDER_CAPS_PANEL (1U << 2)    /* presents reach a real scanout */

struct ame_render_hello {
    unsigned int version;
    unsigned int width;
    unsigned int height;
};

struct ame_render_clear {
    unsigned int rgba;
};

struct ame_render_triangle {
    /* Screen-space integer vertices, y down. Winding is not significant. */
    int x0, y0, x1, y1, x2, y2;
    unsigned int rgba;
};

struct ame_render_reply {
    int status;
    unsigned int flags;
    unsigned int width, height, stride;
    unsigned long long generation;
    unsigned int pixels; /* pixels written by this command */
    unsigned int gpu_id; /* Mali product id when the device was identified */
};
