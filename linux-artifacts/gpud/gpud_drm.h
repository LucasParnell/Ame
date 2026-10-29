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

/* Teardown/PRIME ioctls outside the shared subset. drm.h: `_IOWR('d', 0xAF,
 * __u32)` (DRM_IOCTL_MODE_RMFB) and `_IOWR('d', 0xB4, __u32)`
 * (DRM_IOCTL_MODE_DESTROY_DUMB); both take a single __u32. DRM_PRIME (Phase M)
 * imports a dma-buf fd as a GEM handle: `struct drm_prime_handle` is
 * `__u32 handle; __u32 flags; __s32 fd;`, `_IOWR('d', 0x2e, ...)`. */
struct gdrm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd; /* __s32 in the UAPI */
};
struct gdrm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

#define GDRM_IOCTL_MODE_RMFB DRM_IOWR(0xAF, uint32_t)
#define GDRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, uint32_t)
#define GDRM_IOCTL_GEM_CLOSE DRM_IOC(1, 'd', 0x09, sizeof(struct gdrm_gem_close))
#define GDRM_IOCTL_PRIME_FD_TO_HANDLE DRM_IOWR(0x2e, struct gdrm_prime_handle)

DRM_ASSERT(GDRM_IOCTL_MODE_RMFB == 0xC00464AFu, "RMFB request");
DRM_ASSERT(GDRM_IOCTL_MODE_DESTROY_DUMB == 0xC00464B4u, "DESTROY_DUMB request");
DRM_ASSERT(GDRM_IOCTL_GEM_CLOSE == 0x40086409u, "GEM_CLOSE request");
DRM_ASSERT(sizeof(struct gdrm_prime_handle) == 12, "drm_prime_handle layout");
DRM_ASSERT(GDRM_IOCTL_PRIME_FD_TO_HANDLE == 0xC00C642Eu, "PRIME_FD_TO_HANDLE request");

/* DRM_CAP_PRIME value bits (drm.h). Recorded by the import probe for evidence
 * only: since Linux 6.6 drm_getcap() reports both bits for *every* driver, so
 * the capability cannot gate an import — only the real PRIME_FD_TO_HANDLE call
 * can. */
#define GDRM_CAP_PRIME 0x5u
#define GDRM_PRIME_CAP_IMPORT 0x1u
DRM_ASSERT(GDRM_CAP_PRIME == 0x5u && GDRM_PRIME_CAP_IMPORT == 0x1u, "DRM_CAP_PRIME ids");

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
int gdrm_card_release_master(struct gdrm_card* card);
int gdrm_card_acquire_master(struct gdrm_card* card);
int gdrm_fb_flush(struct gdrm_card* card, uint32_t fb_id);

/* Fill `outputs`/`modes` from the connector tables (preferred mode first).
 * Returns 0 when at least one output with modes was found. */
int gdrm_card_enumerate(struct gdrm_card *card);

int gdrm_dumb_create(struct gdrm_card *card, uint32_t width, uint32_t height, uint32_t bpp,
                     struct gdrm_dumb *dumb);
void gdrm_dumb_destroy(struct gdrm_card *card, uint32_t handle);

int gdrm_fb_create(struct gdrm_card *card, const struct gdrm_dumb *dumb, uint32_t *fb_id_out);
/* Same framebuffer, but for any GEM handle (an imported dma-buf object as well
 * as a dumb buffer). `pitch`/`bpp`/`depth` describe how the memory is scanned
 * out; the caller has already validated the geometry against the handle. */
int gdrm_fb_create_handle(struct gdrm_card *card, uint32_t handle, uint32_t width, uint32_t height,
                          uint32_t pitch, uint32_t bpp, uint32_t depth, uint32_t *fb_id_out);
void gdrm_fb_destroy(struct gdrm_card *card, uint32_t fb_id);

/* Read a DRM capability (DRM_IOCTL_GET_CAP). Returns the value, or -1 when the
 * ioctl fails. `capability` is a DRM_CAP_* id. */
long gdrm_get_cap(struct gdrm_card *card, uint64_t capability);

/* Import `fd` (a dma-buf) as a GEM handle. Returns 0 and stores the handle, or
 * a negative errno from the ioctl (-EINVAL when the device does not support
 * import at all, -ENODEV when it advertises import but cannot wrap this buffer).
 * A negative errno is returned rather than collapsed to -1 so the caller can
 * record *why* an import was refused. */
long gdrm_prime_import(struct gdrm_card *card, int fd, uint32_t *handle_out);

/* Release a GEM handle created by this file (import or dumb). */
void gdrm_gem_close(struct gdrm_card *card, uint32_t handle);

/* Program `crtc_id` to scan out `fb_id` on `connector_id` in `mode`. */
int gdrm_crtc_program(struct gdrm_card *card, uint32_t crtc_id, uint32_t connector_id,
                      uint32_t fb_id, const struct drm_mode_modeinfo *mode);

/* 1 when the CRTC currently scans `fb_id`, 0 when it does not, -1 on error. */
int gdrm_crtc_scanning(struct gdrm_card *card, uint32_t crtc_id, uint32_t fb_id);

#endif /* MIDIX_GPUD_DRM_H */
