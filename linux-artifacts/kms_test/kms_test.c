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
 */

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* aarch64 syscalls                                                          */
/* ------------------------------------------------------------------------- */

#define K_AT_FDCWD (-100)
#define K_O_RDWR 00000002

#define K_PROT_READ 0x1
#define K_PROT_WRITE 0x2
#define K_MAP_SHARED 0x1

static long k_openat(int dirfd, const char* path, int flags, unsigned long mode) {
    register long x0 asm("x0") = dirfd;
    register const char* x1 asm("x1") = path;
    register long x2 asm("x2") = flags;
    register unsigned long x3 asm("x3") = mode;
    register long x8 asm("x8") = 56; /* openat */
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static long k_close(int fd) {
    register long x0 asm("x0") = fd;
    register long x8 asm("x8") = 57; /* close */
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long k_ioctl(int fd, unsigned long request, void* arg) {
    register long x0 asm("x0") = fd;
    register unsigned long x1 asm("x1") = request;
    register void* x2 asm("x2") = arg;
    register long x8 asm("x8") = 29; /* ioctl */
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long k_write(int fd, const void* buf, unsigned long len) {
    register long x0 asm("x0") = fd;
    register const void* x1 asm("x1") = buf;
    register unsigned long x2 asm("x2") = len;
    register long x8 asm("x8") = 64; /* write */
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long k_mmap(void* addr, unsigned long len, int prot, int flags, int fd, long off) {
    register long x0 asm("x0") = (long)addr;
    register unsigned long x1 asm("x1") = len;
    register long x2 asm("x2") = prot;
    register long x3 asm("x3") = flags;
    register long x4 asm("x4") = fd;
    register long x5 asm("x5") = off;
    register long x8 asm("x8") = 222; /* mmap */
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                 : "memory");
    return x0;
}

/* ------------------------------------------------------------------------- */
/* Console output                                                           */
/* ------------------------------------------------------------------------- */

static unsigned long k_strlen(const char* s) {
    unsigned long n = 0;
    while (s[n]) ++n;
    return n;
}

static void k_puts(const char* s) {
    (void)k_write(1, s, k_strlen(s));
}

/* Append an unsigned decimal to `buf`; returns the new end pointer. */
static char* k_append_u32(char* out, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        *out++ = '0';
        return out;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n) *out++ = tmp[--n];
    return out;
}

static char* k_append_str(char* out, const char* s) {
    while (*s) *out++ = *s++;
    return out;
}

/* ------------------------------------------------------------------------- */
/* DRM UAPI (include/uapi/drm/drm_mode.h, drm.h) — sizes drift-checked below  */
/* ------------------------------------------------------------------------- */

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth;
    uint32_t handle;
};

_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "drm_mode_modeinfo layout");
_Static_assert(sizeof(struct drm_mode_card_res) == 64, "drm_mode_card_res layout");
_Static_assert(sizeof(struct drm_mode_get_connector) == 80, "drm_mode_get_connector layout");
_Static_assert(sizeof(struct drm_mode_get_encoder) == 20, "drm_mode_get_encoder layout");
_Static_assert(sizeof(struct drm_mode_crtc) == 104, "drm_mode_crtc layout");
_Static_assert(sizeof(struct drm_mode_create_dumb) == 32, "drm_mode_create_dumb layout");
_Static_assert(sizeof(struct drm_mode_map_dumb) == 16, "drm_mode_map_dumb layout");
_Static_assert(sizeof(struct drm_mode_fb_cmd) == 28, "drm_mode_fb_cmd layout");

#define K_IOC(dir, type, nr, size) \
    (((unsigned long)(dir) << 30) | ((unsigned long)(size) << 16) | \
     ((unsigned long)(type) << 8) | (unsigned long)(nr))
#define K_IO(nr) K_IOC(0, 'd', nr, 0)
#define K_IOWR(nr, t) K_IOC(3, 'd', nr, sizeof(t))

#define K_DRM_IOCTL_SET_MASTER K_IO(0x1e)
#define K_DRM_IOCTL_MODE_GETRESOURCES K_IOWR(0xA0, struct drm_mode_card_res)
#define K_DRM_IOCTL_MODE_GETCRTC K_IOWR(0xA1, struct drm_mode_crtc)
#define K_DRM_IOCTL_MODE_SETCRTC K_IOWR(0xA2, struct drm_mode_crtc)
#define K_DRM_IOCTL_MODE_GETENCODER K_IOWR(0xA6, struct drm_mode_get_encoder)
#define K_DRM_IOCTL_MODE_GETCONNECTOR K_IOWR(0xA7, struct drm_mode_get_connector)
#define K_DRM_IOCTL_MODE_ADDFB K_IOWR(0xAE, struct drm_mode_fb_cmd)
#define K_DRM_IOCTL_MODE_CREATE_DUMB K_IOWR(0xB2, struct drm_mode_create_dumb)
#define K_DRM_IOCTL_MODE_MAP_DUMB K_IOWR(0xB3, struct drm_mode_map_dumb)

/* The request encoding is load-bearing; drift here would be silent. */
_Static_assert(K_DRM_IOCTL_MODE_GETRESOURCES == 0xC04064A0u, "GETRESOURCES request");
_Static_assert(K_DRM_IOCTL_MODE_GETCONNECTOR == 0xC05064A7u, "GETCONNECTOR request");
_Static_assert(K_DRM_IOCTL_MODE_GETENCODER == 0xC01464A6u, "GETENCODER request");
_Static_assert(K_DRM_IOCTL_MODE_GETCRTC == 0xC06864A1u, "GETCRTC request");
_Static_assert(K_DRM_IOCTL_MODE_SETCRTC == 0xC06864A2u, "SETCRTC request");
_Static_assert(K_DRM_IOCTL_MODE_ADDFB == 0xC01C64AEu, "ADDFB request");
_Static_assert(K_DRM_IOCTL_MODE_CREATE_DUMB == 0xC02064B2u, "CREATE_DUMB request");
_Static_assert(K_DRM_IOCTL_MODE_MAP_DUMB == 0xC01064B3u, "MAP_DUMB request");

#define K_DRM_MODE_CONNECTED 1

/* Fixed capacities: the appliance exposes exactly one output on one CRTC. */
#define K_MAX_CRTCS 8
#define K_MAX_CONNECTORS 8
#define K_MAX_ENCODERS 8
#define K_MAX_MODES 32

/* ------------------------------------------------------------------------- */
/* Test                                                                     */
/* ------------------------------------------------------------------------- */

/* Distinctive host-assertable pattern: four equal vertical bars, XRGB8888. */
static const uint32_t k_bars[4] = {
    0x00FF0000u, /* red   */
    0x0000FF00u, /* green */
    0x000000FFu, /* blue  */
    0x00FFFFFFu, /* white */
};

static void k_fill_pattern(uint32_t* fb, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t* row = fb + (unsigned long)y * width;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t bar = (uint32_t)(((unsigned long)x * 4) / width);
            if (bar > 3) bar = 3;
            row[x] = k_bars[bar];
        }
    }
    /* Order the pattern writes before the modeset publishes the buffer. */
    asm volatile("dmb ish" ::: "memory");
}

static int k_fail(const char* stage) {
    char buf[128];
    char* p = k_append_str(buf, "dango: kms test FAILED: ");
    p = k_append_str(p, stage);
    *p++ = '\n';
    (void)k_write(1, buf, (unsigned long)(p - buf));
    return 0;
}

int dango_kms_test(void) {
    long fd = k_openat(K_AT_FDCWD, "/dev/dri/card0", K_O_RDWR, 0);
    if (fd < 0) return k_fail("open card0");
    /* A primary node grants master to its first opener; make it explicit so a
     * stale fd elsewhere cannot leave the modeset silently unprivileged. */
    (void)k_ioctl((int)fd, K_DRM_IOCTL_SET_MASTER, 0);

    /* --- resources --------------------------------------------------------- */
    struct drm_mode_card_res res;
    for (unsigned long i = 0; i < sizeof(res); ++i) ((char*)&res)[i] = 0;
    if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        (void)k_close((int)fd);
        return k_fail("getresources");
    }
    if (res.count_crtcs == 0 || res.count_connectors == 0) {
        (void)k_close((int)fd);
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

    for (unsigned long i = 0; i < sizeof(res); ++i) ((char*)&res)[i] = 0;
    res.crtc_id_ptr = (uint64_t)(unsigned long)crtc_ids;
    res.connector_id_ptr = (uint64_t)(unsigned long)connector_ids;
    res.encoder_id_ptr = (uint64_t)(unsigned long)encoder_ids;
    res.count_crtcs = count_crtcs;
    res.count_connectors = count_connectors;
    res.count_encoders = count_encoders;
    if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        (void)k_close((int)fd);
        return k_fail("getresources(2)");
    }
    count_crtcs = res.count_crtcs < K_MAX_CRTCS ? res.count_crtcs : K_MAX_CRTCS;
    count_connectors =
        res.count_connectors < K_MAX_CONNECTORS ? res.count_connectors : K_MAX_CONNECTORS;

    /* --- connector + preferred mode --------------------------------------- */
    uint32_t conn_id = 0;
    uint32_t diag_connection = 0xFFFFFFFFu, diag_modes = 0xFFFFFFFFu;
    struct drm_mode_modeinfo mode;
    for (unsigned long i = 0; i < sizeof(mode); ++i) ((char*)&mode)[i] = 0;
    uint32_t conn_encoder = 0;
    for (uint32_t c = 0; c < count_connectors; ++c) {
        struct drm_mode_get_connector conn;
        for (unsigned long i = 0; i < sizeof(conn); ++i) ((char*)&conn)[i] = 0;
        conn.connector_id = connector_ids[c];
        /* First pass: counts + state. */
        if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
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
        if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.count_modes == 0) continue;
        /* Preferred mode is returned first. */
        mode = modes[0];
        conn_id = conn.connector_id;
        conn_encoder = conn.encoder_id;
        break;
    }
    if (conn_id == 0) {
        char buf[160];
        char* p = k_append_str(buf, "dango: kms diag crtcs=");
        p = k_append_u32(p, count_crtcs);
        p = k_append_str(p, " conns=");
        p = k_append_u32(p, count_connectors);
        p = k_append_str(p, " c0_modes=");
        p = k_append_u32(p, diag_modes);
        p = k_append_str(p, " c0_conn=");
        p = k_append_u32(p, diag_connection);
        *p++ = '\n';
        (void)k_write(1, buf, (unsigned long)(p - buf));
        (void)k_close((int)fd);
        return k_fail("no connector with modes");
    }

    uint32_t crtc_id = crtc_ids[0];
    if (conn_encoder != 0) {
        struct drm_mode_get_encoder enc;
        for (unsigned long i = 0; i < sizeof(enc); ++i) ((char*)&enc)[i] = 0;
        enc.encoder_id = conn_encoder;
        if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_GETENCODER, &enc) >= 0 && enc.crtc_id != 0) {
            crtc_id = enc.crtc_id;
        }
    }

    /* --- dumb buffer ------------------------------------------------------- */
    struct drm_mode_create_dumb dumb;
    for (unsigned long i = 0; i < sizeof(dumb); ++i) ((char*)&dumb)[i] = 0;
    dumb.width = mode.hdisplay;
    dumb.height = mode.vdisplay;
    dumb.bpp = 32;
    if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0 || dumb.handle == 0 ||
        dumb.pitch == 0 || dumb.size == 0) {
        (void)k_close((int)fd);
        return k_fail("create_dumb");
    }

    struct drm_mode_map_dumb map;
    for (unsigned long i = 0; i < sizeof(map); ++i) ((char*)&map)[i] = 0;
    map.handle = dumb.handle;
    if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        (void)k_close((int)fd);
        return k_fail("map_dumb");
    }

    long mapped = k_mmap(0, dumb.size, K_PROT_READ | K_PROT_WRITE, K_MAP_SHARED, (int)fd,
                         (long)map.offset);
    if (mapped < 0) {
        (void)k_close((int)fd);
        return k_fail("mmap");
    }
    k_fill_pattern((uint32_t*)(unsigned long)mapped, dumb.width, dumb.height);

    /* --- framebuffer + crtc ------------------------------------------------- */
    struct drm_mode_fb_cmd fb;
    for (unsigned long i = 0; i < sizeof(fb); ++i) ((char*)&fb)[i] = 0;
    fb.width = dumb.width;
    fb.height = dumb.height;
    fb.pitch = dumb.pitch;
    fb.bpp = dumb.bpp;
    fb.depth = 24;
    fb.handle = dumb.handle;
    if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_ADDFB, &fb) < 0 || fb.fb_id == 0) {
        (void)k_close((int)fd);
        return k_fail("addfb");
    }

    struct drm_mode_crtc crtc;
    for (unsigned long i = 0; i < sizeof(crtc); ++i) ((char*)&crtc)[i] = 0;
    crtc.set_connectors_ptr = (uint64_t)(unsigned long)&conn_id;
    crtc.count_connectors = 1;
    crtc.crtc_id = crtc_id;
    crtc.fb_id = fb.fb_id;
    crtc.mode_valid = 1;
    crtc.mode = mode;
    if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        (void)k_close((int)fd);
        return k_fail("setcrtc");
    }

    /* --- confirm the CRTC is scanning out our FB -------------------------- */
    struct drm_mode_crtc got;
    for (unsigned long i = 0; i < sizeof(got); ++i) ((char*)&got)[i] = 0;
    got.crtc_id = crtc_id;
    if (k_ioctl((int)fd, K_DRM_IOCTL_MODE_GETCRTC, &got) < 0 || got.fb_id != fb.fb_id) {
        (void)k_close((int)fd);
        return k_fail("crtc not scanning our fb");
    }

    /* Keep the fd open: closing would release master. The caller keeps the
     * process (and the modeset) alive for the host capture. */
    char buf[160];
    char* p = k_append_str(buf, "dango: kms fb=");
    p = k_append_u32(p, fb.fb_id);
    p = k_append_str(p, " crtc=");
    p = k_append_u32(p, crtc_id);
    p = k_append_str(p, " connector=");
    p = k_append_u32(p, conn_id);
    p = k_append_str(p, " mode=");
    p = k_append_u32(p, dumb.width);
    *p++ = 'x';
    p = k_append_u32(p, dumb.height);
    *p++ = '\n';
    (void)k_write(1, buf, (unsigned long)(p - buf));
    k_puts("dango: kms displayed\n");
    return 1;
}
