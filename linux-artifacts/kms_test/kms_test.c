/*
 * linux-artifacts/kms_test/kms_test.c — Phase F standalone Linux KMS self-test.
 *
 * Raw DRM ioctls only: no libdrm, no libc. The appliance userspace stays
 * dependency-free (docs/LINUX_APPLIANCE_MINIMAL.md), and the point of this test
 * is to prove the *guest* Linux stack — device assignment + MMIO + IRQ + DMA +
 * DRM + virtio_gpu — can drive a real modeset while Ame contains no virtio-gpu
 * register logic whatsoever.
 *
 * It opens /dev/dri/card0, reads the resource/connector tables, allocates a
 * dumb buffer, fills a fixed four-colour bar pattern, creates an FB and programs
 * the CRTC (legacy SETCRTC, which virtio_gpu exposes through
 * drm_atomic_helper_set_config). The host asserts the displayed pattern with a
 * QEMU screendump (scripts/test/kms-display.py).
 *
 * The entry point, dango_kms_test(), is linked into the appliance init binary
 * and called by the agent after its DRM probe succeeds.
 *
 * Relies on the agent's global memcpy/memset (cross-TU link contract): the
 * struct assignments below lower to memcpy(), provided by linux-artifacts/agent.
 */

#include <stddef.h>
#include <stdint.h>

#include "midix/abi/dango_channel_guest.h"
#include "midix/abi/linux_drm_uapi.h"
#include "midix/abi/linux_guest_console.h"
#include "midix/abi/linux_guest_syscall.h"

/* Fixed capacities: the appliance exposes exactly one output on one CRTC. */
#define K_MAX_CRTCS 8
#define K_MAX_CONNECTORS 8
#define K_MAX_ENCODERS 8
#define K_MAX_MODES 32

/* Distinctive host-assertable pattern: four equal vertical bars, XRGB8888. */
static const uint32_t k_bars[4] = {
    0x00FF0000u, /* red   */
    0x0000FF00u, /* green */
    0x000000FFu, /* blue  */
    0x00FFFFFFu, /* white */
};

/* Bars cover the visible width; rows stride by `stride` words (the dumb
 * buffer's real pitch), not by `width`, so the pattern is not sheared when the
 * pitch rounds up past width*bytes-per-pixel. */
static void k_fill_pattern(uint32_t* fb, uint32_t width, uint32_t height, uint32_t stride) {
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t* row = fb + (unsigned long)y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t bar = (uint32_t)(((unsigned long)x * 4) / width);
            if (bar > 3) bar = 3;
            row[x] = k_bars[bar];
        }
    }
    /* Order the pattern writes before the modeset publishes the buffer. */
    __asm__ volatile("dmb ish" ::: "memory");
}

static int k_fail(const char* stage) {
    char buf[128];
    char* p = mx_append_str(buf, "dango: kms test FAILED: ");
    p = mx_append_str(p, stage);
    *p++ = '\n';
    (void)mx_write(1, buf, (unsigned long)(p - buf));
    return 0;
}

int dango_kms_test(void) {
    long fd = mx_openat(MX_AT_FDCWD, DANGO_DEV_DRM_CARD0, MX_O_RDWR, 0);
    if (fd < 0) return k_fail("open card0");
    /* A primary node grants master to its first opener; make it explicit so a
     * stale fd elsewhere cannot leave the modeset silently unprivileged. */
    (void)mx_ioctl((int)fd, DRM_IOCTL_SET_MASTER, 0);

    /* --- resources --------------------------------------------------------- */
    struct drm_mode_card_res res;
    mx_zero(&res, sizeof(res));
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        (void)mx_close((int)fd);
        return k_fail("getresources");
    }
    if (res.count_crtcs == 0 || res.count_connectors == 0) {
        (void)mx_close((int)fd);
        return k_fail("no crtc/connector");
    }

    uint32_t crtc_ids[K_MAX_CRTCS];
    uint32_t connector_ids[K_MAX_CONNECTORS];
    uint32_t encoder_ids[K_MAX_ENCODERS];
    uint32_t count_crtcs = res.count_crtcs < K_MAX_CRTCS ? res.count_crtcs : K_MAX_CRTCS;
    uint32_t count_connectors =
        res.count_connectors < K_MAX_CONNECTORS ? res.count_connectors : K_MAX_CONNECTORS;
    uint32_t count_encoders =
        res.count_encoders < K_MAX_ENCODERS ? res.count_encoders : K_MAX_ENCODERS;

    mx_zero(&res, sizeof(res));
    res.crtc_id_ptr = (uint64_t)(unsigned long)crtc_ids;
    res.connector_id_ptr = (uint64_t)(unsigned long)connector_ids;
    res.encoder_id_ptr = (uint64_t)(unsigned long)encoder_ids;
    res.count_crtcs = count_crtcs;
    res.count_connectors = count_connectors;
    res.count_encoders = count_encoders;
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        (void)mx_close((int)fd);
        return k_fail("getresources(2)");
    }
    count_crtcs = res.count_crtcs < K_MAX_CRTCS ? res.count_crtcs : K_MAX_CRTCS;
    count_connectors =
        res.count_connectors < K_MAX_CONNECTORS ? res.count_connectors : K_MAX_CONNECTORS;

    /* --- connector + preferred mode --------------------------------------- */
    uint32_t conn_id = 0;
    uint32_t diag_connection = 0xFFFFFFFFu, diag_modes = 0xFFFFFFFFu;
    struct drm_mode_modeinfo mode;
    mx_zero(&mode, sizeof(mode));
    uint32_t conn_encoder = 0;
    for (uint32_t c = 0; c < count_connectors; ++c) {
        struct drm_mode_get_connector conn;
        mx_zero(&conn, sizeof(conn));
        conn.connector_id = connector_ids[c];
        /* First pass: counts + state. */
        if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (c == 0) { diag_connection = conn.connection; diag_modes = conn.count_modes; }
        /* A headless QEMU display backend can leave the connector status
         * "unknown" (2) even though EDID modes exist; a modeset only needs
         * modes, so gate on modes rather than on the reported status. */
        if (conn.count_modes == 0) continue;
        if (count_crtcs == 0) continue;

        uint32_t modes_cap = conn.count_modes < K_MAX_MODES ? conn.count_modes : K_MAX_MODES;
        struct drm_mode_modeinfo modes[K_MAX_MODES];
        /* Fetch only the mode array. The kernel copies into any pointer whose
         * matching count is non-zero, so the counts the first call reported
         * must be cleared or it will copy to NULL and return -EFAULT. */
        conn.modes_ptr = (uint64_t)(unsigned long)modes;
        conn.count_modes = modes_cap;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        conn.count_props = 0;
        conn.encoders_ptr = 0;
        conn.count_encoders = 0;
        if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.count_modes == 0) continue;
        /* Preferred mode is returned first. */
        mode = modes[0];
        conn_id = conn.connector_id;
        conn_encoder = conn.encoder_id;
        break;
    }
    if (conn_id == 0) {
        char buf[160];
        char* p = mx_append_str(buf, "dango: kms diag crtcs=");
        p = mx_append_u32(p, count_crtcs);
        p = mx_append_str(p, " conns=");
        p = mx_append_u32(p, count_connectors);
        p = mx_append_str(p, " c0_modes=");
        p = mx_append_u32(p, diag_modes);
        p = mx_append_str(p, " c0_conn=");
        p = mx_append_u32(p, diag_connection);
        *p++ = '\n';
        (void)mx_write(1, buf, (unsigned long)(p - buf));
        (void)mx_close((int)fd);
        return k_fail("no connector with modes");
    }

    uint32_t crtc_id = crtc_ids[0];
    if (conn_encoder != 0) {
        struct drm_mode_get_encoder enc;
        mx_zero(&enc, sizeof(enc));
        enc.encoder_id = conn_encoder;
        if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETENCODER, &enc) >= 0 && enc.crtc_id != 0) {
            crtc_id = enc.crtc_id;
        }
    }

    /* --- dumb buffer ------------------------------------------------------- */
    struct drm_mode_create_dumb dumb;
    mx_zero(&dumb, sizeof(dumb));
    dumb.width = mode.hdisplay;
    dumb.height = mode.vdisplay;
    dumb.bpp = 32;
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0 || dumb.handle == 0 ||
        dumb.pitch == 0 || dumb.size == 0) {
        (void)mx_close((int)fd);
        return k_fail("create_dumb");
    }
    /* ADDFB consumes dumb.pitch and mmap exposes it as the row stride, so a
     * pitch that under-covers the width or is not pixel-aligned would corrupt
     * the pattern. Guard the class before touching a single pixel. */
    uint32_t bpp_bytes = dumb.bpp / 8u;
    if (dumb.pitch < dumb.width * bpp_bytes || (dumb.pitch & (bpp_bytes - 1u))) {
        (void)mx_close((int)fd);
        return k_fail("pitch");
    }

    struct drm_mode_map_dumb map;
    mx_zero(&map, sizeof(map));
    map.handle = dumb.handle;
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        (void)mx_close((int)fd);
        return k_fail("map_dumb");
    }

    long mapped = mx_mmap(0, dumb.size, MX_PROT_READ | MX_PROT_WRITE, MX_MAP_SHARED, (int)fd,
                          (long)map.offset);
    if (mapped < 0) {
        (void)mx_close((int)fd);
        return k_fail("mmap");
    }
    k_fill_pattern((uint32_t*)(unsigned long)mapped, dumb.width, dumb.height,
                   dumb.pitch / bpp_bytes);

    /* --- framebuffer + crtc ------------------------------------------------- */
    struct drm_mode_fb_cmd fb;
    mx_zero(&fb, sizeof(fb));
    fb.width = dumb.width;
    fb.height = dumb.height;
    fb.pitch = dumb.pitch;
    fb.bpp = dumb.bpp;
    fb.depth = 24;
    fb.handle = dumb.handle;
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0 || fb.fb_id == 0) {
        (void)mx_close((int)fd);
        return k_fail("addfb");
    }

    struct drm_mode_crtc crtc;
    mx_zero(&crtc, sizeof(crtc));
    crtc.set_connectors_ptr = (uint64_t)(unsigned long)&conn_id;
    crtc.count_connectors = 1;
    crtc.crtc_id = crtc_id;
    crtc.fb_id = fb.fb_id;
    crtc.mode_valid = 1;
    crtc.mode = mode;
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        (void)mx_close((int)fd);
        return k_fail("setcrtc");
    }

    /* --- confirm the CRTC is scanning out our FB -------------------------- */
    struct drm_mode_crtc got;
    mx_zero(&got, sizeof(got));
    got.crtc_id = crtc_id;
    if (mx_ioctl((int)fd, DRM_IOCTL_MODE_GETCRTC, &got) < 0 || got.fb_id != fb.fb_id) {
        (void)mx_close((int)fd);
        return k_fail("crtc not scanning our fb");
    }

    /* Keep the framebuffer alive but hand DRM authority back to gpud after
     * this self-test. The retained file owns the KMS test framebuffer. */
    if (mx_ioctl((int)fd, DRM_IOCTL_DROP_MASTER, 0) != 0) {
        (void)mx_close((int)fd);
        return k_fail("drop_master");
    }
    char buf[160];
    char* p = mx_append_str(buf, "dango: kms fb=");
    p = mx_append_u32(p, fb.fb_id);
    p = mx_append_str(p, " crtc=");
    p = mx_append_u32(p, crtc_id);
    p = mx_append_str(p, " connector=");
    p = mx_append_u32(p, conn_id);
    p = mx_append_str(p, " mode=");
    p = mx_append_u32(p, dumb.width);
    *p++ = 'x';
    p = mx_append_u32(p, dumb.height);
    *p++ = '\n';
    (void)mx_write(1, buf, (unsigned long)(p - buf));
    mx_puts("dango: kms displayed\n");
    return 1;
}
