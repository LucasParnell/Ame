#pragma once
/*
 * gpud_drm.h — raw DRM/KMS control for dango-gpud (no libdrm, no libc).
 *
 * This is the shared home for the appliance's DRM modeset logic: resource and
 * connector enumeration, dumb buffers, framebuffers and CRTC programming. It
 * was factored out of the Phase F KMS self-test so the control plane
 * (GPU_ENUM_OUTPUTS / GPU_GET_MODES) and the present path (GPU_SET_MODE /
 * GPU_PRESENT) drive the same code instead of two drifting copies.
 *
 * Only ioctls from the shared `midix/abi/linux_drm_uapi.h` subset are used; the
 * two teardown ioctls the appliace also needs are declared here with their
 * exact encodings. No DRM object is ever named by a Midix protocol message: the
 * server translates its own opaque ids into these handles internally.
 */
#ifndef MIDIX_GPUD_DRM_H
#define MIDIX_GPUD_DRM_H

#include <stdint.h>

#include "midix/abi/linux_drm_uapi.h"

/* Fixed capacities: the bootstrap profile exposes one output on one CRTC. */
#define GDRM_MAX_CRTCS 8
#define GDRM_MAX_CONNECTORS 8
#define GDRM_MAX_ENCODERS 8
#define GDRM_MAX_MODES 32

/* Teardown ioctls outside the shared subset. drm.h: `_IOWR('d', 0xAF, __u32)`
 * (DRM_IOCTL_MODE_RMFB) and `_IOWR('d', 0xB4, __u32)`
 * (DRM_IOCTL_MODE_DESTROY_DUMB); both take a single __u32. */
#define GDRM_IOCTL_MODE_RMFB DRM_IOWR(0xAF, uint32_t)
#define GDRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, uint32_t)
#define GDRM_IOCTL_DROP_MASTER DRM_IO(0x1f)

DRM_ASSERT(GDRM_IOCTL_MODE_RMFB == 0xC00464AFu, "RMFB request");
DRM_ASSERT(GDRM_IOCTL_MODE_DESTROY_DUMB == 0xC00464B4u, "DESTROY_DUMB request");
DRM_ASSERT(GDRM_IOCTL_DROP_MASTER == 0x0000641Fu, "DROP_MASTER request");

struct gdrm_output {
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    uint32_t type;
    uint32_t connection;
    uint32_t width_mm;
    uint32_t height_mm;
    uint32_t mode_count;
    char name[8];
    struct drm_mode_modeinfo modes[GDRM_MAX_MODES];
};

struct gdrm_card {
    int fd;
    int master;
    uint32_t crtc_count;
    uint32_t connector_count;
    uint32_t output_count;
    uint32_t crtc_ids[GDRM_MAX_CRTCS];
    uint32_t connector_ids[GDRM_MAX_CONNECTORS];
    struct gdrm_output outputs[GDRM_MAX_CONNECTORS];
};

struct gdrm_dumb {
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint64_t size;
    unsigned char *map;
    uint64_t map_len;
};

/* Open `path` (card0), take master and read the resource tables. */
int gdrm_card_open(struct gdrm_card *card, const char *path);
void gdrm_card_close(struct gdrm_card *card);

/* Release DRM master while keeping the card open, so the framebuffer this file
 * created stays on the CRTC (closing would drop it: drm_file_free() calls
 * drm_fb_release()). Used when the server hands the card to the self-tests. */
void gdrm_card_release_master(struct gdrm_card *card);

/* Fill `outputs`/`modes` from the connector tables (preferred mode first).
 * Returns 0 when at least one output with modes was found. */
int gdrm_card_enumerate(struct gdrm_card *card);

int gdrm_dumb_create(struct gdrm_card *card, uint32_t width, uint32_t height, uint32_t bpp,
                     struct gdrm_dumb *dumb);
void gdrm_dumb_destroy(struct gdrm_card *card, uint32_t handle);

int gdrm_fb_create(struct gdrm_card *card, const struct gdrm_dumb *dumb, uint32_t *fb_id_out);
void gdrm_fb_destroy(struct gdrm_card *card, uint32_t fb_id);

/* Program `crtc_id` to scan out `fb_id` on `connector_id` in `mode`. */
int gdrm_crtc_program(struct gdrm_card *card, uint32_t crtc_id, uint32_t connector_id,
                      uint32_t fb_id, const struct drm_mode_modeinfo *mode);

/* 1 when the CRTC currently scans `fb_id`, 0 when it does not, -1 on error. */
int gdrm_crtc_scanning(struct gdrm_card *card, uint32_t crtc_id, uint32_t fb_id);

#endif /* MIDIX_GPUD_DRM_H */
