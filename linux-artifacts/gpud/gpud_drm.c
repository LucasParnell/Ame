/*
 * gpud_drm.c — raw DRM/KMS helpers for dango-gpud.
 *
 * Freestanding: raw ioctls only, no libdrm and no libc. The patterns here
 * (two-call GETRESOURCES/GETCONNECTOR, pitch validation, dumb buffer + mmap,
 * legacy SETCRTC) are the ones the Phase F KMS self-test proved against
 * virtio_gpu; they now live in one place so the control plane and the present
 * path share them.
 *
 * Relies on the agent's global memcpy/memset (cross-TU link contract).
 */
#include "gpud_drm.h"

#include "midix/abi/linux_guest_console.h"
#include "midix/abi/linux_guest_syscall.h"

void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);

static const char *connector_type_name(uint32_t type) {
    /* Short DRM_MODE_CONNECTOR_* names that fit the 8-byte output label. */
    switch (type) {
    case 1:
        return "VGA";
    case 9:
        return "LVDS";
    case 11:
        return "DP";
    case 12:
        return "HDMI-A";
    case 13:
        return "HDMI-B";
    case 14:
        return "TV";
    case 15:
        return "Virtual";
    case 16:
        return "DSI";
    case 18:
        return "DVI-D";
    default:
        return "out";
    }
}

static void output_set_name(char *dst, uint32_t type) {
    const char *name = connector_type_name(type);
    uint32_t i;
    for (i = 0; i < 7 && name[i]; ++i) {
        dst[i] = name[i];
    }
    for (; i < 8; ++i) {
        dst[i] = 0;
    }
}

int gdrm_card_open(struct gdrm_card *card, const char *path) {
    struct drm_mode_card_res res;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    long fd;

    if (!card) {
        return -1;
    }
    memset(card, 0, sizeof(*card));
    card->fd = -1;

    fd = mx_openat(MX_AT_FDCWD, path, MX_O_RDWR, 0);
    if (fd < 0) {
        return -1;
    }
    card->fd = (int)fd;
    /* A primary node grants master to its first opener. Making it explicit lets
     * the present path fail loudly later if another opener already holds it. */
    card->master = mx_ioctl((int)fd, DRM_IOCTL_SET_MASTER, 0) == 0;

    memset(&res, 0, sizeof(res));
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        gdrm_card_close(card);
        return -1;
    }
    if (res.count_crtcs == 0 || res.count_connectors == 0) {
        gdrm_card_close(card);
        return -1;
    }

    count_crtcs = res.count_crtcs < GDRM_MAX_CRTCS ? res.count_crtcs : GDRM_MAX_CRTCS;
    count_connectors =
        res.count_connectors < GDRM_MAX_CONNECTORS ? res.count_connectors : GDRM_MAX_CONNECTORS;

    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uint64_t)(unsigned long)card->crtc_ids;
    res.connector_id_ptr = (uint64_t)(unsigned long)card->connector_ids;
    res.count_crtcs = count_crtcs;
    res.count_connectors = count_connectors;
    /* Encoder ids are only needed to resolve a connector's CRTC, which is done
     * one GETENCODER at a time below; the table itself is not kept. */
    res.count_encoders = 0;
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        gdrm_card_close(card);
        return -1;
    }
    card->crtc_count = res.count_crtcs < GDRM_MAX_CRTCS ? res.count_crtcs : GDRM_MAX_CRTCS;
    card->connector_count =
        res.count_connectors < GDRM_MAX_CONNECTORS ? res.count_connectors : GDRM_MAX_CONNECTORS;
    return 0;
}

void gdrm_card_close(struct gdrm_card *card) {
    if (!card) {
        return;
    }
    if (card->fd >= 0) {
        if (card->master) {
            (void)mx_ioctl(card->fd, GDRM_IOCTL_DROP_MASTER, 0);
        }
        (void)mx_close(card->fd);
    }
    card->fd = -1;
    card->master = 0;
}

void gdrm_card_release_master(struct gdrm_card *card) {
    if (!card || card->fd < 0 || !card->master) {
        return;
    }
    (void)mx_ioctl(card->fd, GDRM_IOCTL_DROP_MASTER, 0);
    card->master = 0;
}

int gdrm_card_enumerate(struct gdrm_card *card) {
    uint32_t i;
    uint32_t outputs = 0;

    if (!card || card->fd < 0) {
        return -1;
    }
    for (i = 0; i < card->connector_count && outputs < GDRM_MAX_CONNECTORS; ++i) {
        struct drm_mode_get_connector conn;
        struct drm_mode_modeinfo modes[GDRM_MAX_MODES];
        struct gdrm_output *output;
        uint32_t cap;

        memset(&conn, 0, sizeof(conn));
        conn.connector_id = card->connector_ids[i];
        /* First pass: counts and connection state. */
        if (mx_ioctl(card->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            continue;
        }
        /* A headless backend can report the connector "unknown" even though
         * EDID modes exist; a modeset only needs modes, so gate on modes. */
        if (conn.count_modes == 0 || card->crtc_count == 0) {
            continue;
        }
        cap = conn.count_modes < GDRM_MAX_MODES ? conn.count_modes : GDRM_MAX_MODES;
        /* Fetch only the mode array: the kernel copies into any pointer whose
         * matching count is non-zero, so the other counts must be cleared or it
         * would fault on a NULL pointer. */
        conn.modes_ptr = (uint64_t)(unsigned long)modes;
        conn.count_modes = cap;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        conn.count_props = 0;
        conn.encoders_ptr = 0;
        conn.count_encoders = 0;
        if (mx_ioctl(card->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            continue;
        }
        if (conn.count_modes == 0) {
            continue;
        }

        output = &card->outputs[outputs];
        memset(output, 0, sizeof(*output));
        output->connector_id = conn.connector_id;
        output->type = conn.connector_type;
        output->connection = conn.connection;
        output->width_mm = conn.mm_width;
        output->height_mm = conn.mm_height;
        output->mode_count = conn.count_modes < GDRM_MAX_MODES ? conn.count_modes : GDRM_MAX_MODES;
        /* Preferred mode is returned first. */
        memcpy(output->modes, modes, (unsigned long)output->mode_count * sizeof(modes[0]));
        output_set_name(output->name, conn.connector_type);

        output->encoder_id = conn.encoder_id;
        output->crtc_id = card->crtc_ids[0];
        if (conn.encoder_id != 0) {
            struct drm_mode_get_encoder encoder;
            memset(&encoder, 0, sizeof(encoder));
            encoder.encoder_id = conn.encoder_id;
            if (mx_ioctl(card->fd, DRM_IOCTL_MODE_GETENCODER, &encoder) >= 0 &&
                encoder.crtc_id != 0) {
                output->crtc_id = encoder.crtc_id;
            }
        }
        ++outputs;
    }
    card->output_count = outputs;
    return outputs != 0 ? 0 : -1;
}

int gdrm_dumb_create(struct gdrm_card *card, uint32_t width, uint32_t height, uint32_t bpp,
                     struct gdrm_dumb *dumb) {
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    uint32_t bytes_per_pixel;
    long mapped;

    if (!card || card->fd < 0 || !dumb || width == 0 || height == 0 || bpp == 0) {
        return -1;
    }
    memset(dumb, 0, sizeof(*dumb));
    memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = height;
    create.bpp = bpp;
    if (mx_ioctl(card->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 || create.handle == 0 ||
        create.pitch == 0 || create.size == 0) {
        return -1;
    }
    /* ADDFB consumes the pitch and mmap exposes it as the row stride, so a
     * pitch that under-covers the width, or is not pixel aligned, would corrupt
     * the image. Guard the class before a single byte is mapped. */
    bytes_per_pixel = create.bpp / 8u;
    if (bytes_per_pixel == 0 ||
        (uint64_t)create.pitch < (uint64_t)create.width * bytes_per_pixel ||
        (create.pitch & (bytes_per_pixel - 1u)) != 0) {
        (void)mx_ioctl(card->fd, GDRM_IOCTL_MODE_DESTROY_DUMB, &create.handle);
        return -1;
    }

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (mx_ioctl(card->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        (void)mx_ioctl(card->fd, GDRM_IOCTL_MODE_DESTROY_DUMB, &create.handle);
        return -1;
    }
    mapped = mx_mmap(0, create.size, MX_PROT_READ | MX_PROT_WRITE, MX_MAP_SHARED, card->fd,
                     (long)map.offset);
    if (mapped < 0) {
        (void)mx_ioctl(card->fd, GDRM_IOCTL_MODE_DESTROY_DUMB, &create.handle);
        return -1;
    }

    dumb->handle = create.handle;
    dumb->width = create.width;
    dumb->height = create.height;
    dumb->bpp = create.bpp;
    dumb->pitch = create.pitch;
    dumb->size = create.size;
    dumb->map = (unsigned char *)(unsigned long)mapped;
    dumb->map_len = create.size;
    return 0;
}

void gdrm_dumb_destroy(struct gdrm_card *card, uint32_t handle) {
    if (!card || card->fd < 0 || handle == 0) {
        return;
    }
    (void)mx_ioctl(card->fd, GDRM_IOCTL_MODE_DESTROY_DUMB, &handle);
}

int gdrm_fb_create(struct gdrm_card *card, const struct gdrm_dumb *dumb, uint32_t *fb_id_out) {
    struct drm_mode_fb_cmd fb;

    if (!card || card->fd < 0 || !dumb) {
        return -1;
    }
    memset(&fb, 0, sizeof(fb));
    fb.width = dumb->width;
    fb.height = dumb->height;
    fb.pitch = dumb->pitch;
    fb.bpp = dumb->bpp;
    fb.depth = 24; /* XRGB8888 scans out as 24-bit colour plus padding */
    fb.handle = dumb->handle;
    if (mx_ioctl(card->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0 || fb.fb_id == 0) {
        return -1;
    }
    if (fb_id_out) {
        *fb_id_out = fb.fb_id;
    }
    return 0;
}

void gdrm_fb_destroy(struct gdrm_card *card, uint32_t fb_id) {
    if (!card || card->fd < 0 || fb_id == 0) {
        return;
    }
    (void)mx_ioctl(card->fd, GDRM_IOCTL_MODE_RMFB, &fb_id);
}

int gdrm_crtc_program(struct gdrm_card *card, uint32_t crtc_id, uint32_t connector_id,
                      uint32_t fb_id, const struct drm_mode_modeinfo *mode) {
    struct drm_mode_crtc crtc;

    if (!card || card->fd < 0 || !mode || crtc_id == 0 || fb_id == 0) {
        return -1;
    }
    memset(&crtc, 0, sizeof(crtc));
    crtc.set_connectors_ptr = (uint64_t)(unsigned long)&connector_id;
    crtc.count_connectors = 1;
    crtc.crtc_id = crtc_id;
    crtc.fb_id = fb_id;
    crtc.x = 0;
    crtc.y = 0;
    crtc.mode_valid = 1;
    crtc.mode = *mode;
    return mx_ioctl(card->fd, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0 ? 0 : -1;
}

int gdrm_crtc_scanning(struct gdrm_card *card, uint32_t crtc_id, uint32_t fb_id) {
    struct drm_mode_crtc got;

    if (!card || card->fd < 0 || crtc_id == 0) {
        return -1;
    }
    memset(&got, 0, sizeof(got));
    got.crtc_id = crtc_id;
    if (mx_ioctl(card->fd, DRM_IOCTL_MODE_GETCRTC, &got) < 0) {
        return -1;
    }
    return got.fb_id == fb_id ? 1 : 0;
}
