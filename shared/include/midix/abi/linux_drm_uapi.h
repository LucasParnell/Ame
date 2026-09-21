#pragma once
/*
 * linux_drm_uapi.h — the subset of Linux DRM UAPI used by the dango appliance
 * (include/uapi/drm/drm_mode.h, drm.h). C and C++ compatible: structs are plain
 * aggregates and the assertions pick _Static_assert / static_assert per language.
 *
 * Single shared copy: the agent probe and the KMS self-test both consume these
 * layouts and request encodings, and the _Static_asserts below turn any drift
 * in either into a build failure instead of a silent runtime mismatch.
 */
#include <stdint.h>

#if defined(__cplusplus)
#define DRM_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define DRM_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* --- IOCTL encoding (asm-generic/ioctl.h, 4 KiB-page Linux) --------------- */
#define DRM_IOC(dir, type, nr, size) \
    (((unsigned long)(dir) << 30) | ((unsigned long)(size) << 16) | \
     ((unsigned long)(type) << 8) | (unsigned long)(nr))
#define DRM_IO(nr) DRM_IOC(0, 'd', nr, 0)
#define DRM_IOWR(nr, t) DRM_IOC(3, 'd', nr, sizeof(t))

/* --- structs -------------------------------------------------------------- */
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

/* drm_version / drm_get_cap: driver identification + capability probe. */
struct drm_version {
    int major, minor, patch;
    unsigned long name_len;
    char* name;
    unsigned long date_len;
    char* date;
    unsigned long desc_len;
    char* desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

/* --- ioctls --------------------------------------------------------------- */
#define DRM_IOCTL_VERSION DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_MASTER DRM_IO(0x1e)

#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB DRM_IOWR(0xB3, struct drm_mode_map_dumb)

/* --- layout drift checks -------------------------------------------------- */
DRM_ASSERT(sizeof(struct drm_mode_modeinfo) == 68, "drm_mode_modeinfo layout");
DRM_ASSERT(sizeof(struct drm_mode_card_res) == 64, "drm_mode_card_res layout");
DRM_ASSERT(sizeof(struct drm_mode_get_connector) == 80, "drm_mode_get_connector layout");
DRM_ASSERT(sizeof(struct drm_mode_get_encoder) == 20, "drm_mode_get_encoder layout");
DRM_ASSERT(sizeof(struct drm_mode_crtc) == 104, "drm_mode_crtc layout");
DRM_ASSERT(sizeof(struct drm_mode_create_dumb) == 32, "drm_mode_create_dumb layout");
DRM_ASSERT(sizeof(struct drm_mode_map_dumb) == 16, "drm_mode_map_dumb layout");
DRM_ASSERT(sizeof(struct drm_mode_fb_cmd) == 28, "drm_mode_fb_cmd layout");
DRM_ASSERT(sizeof(struct drm_version) == 64, "drm_version layout");
DRM_ASSERT(sizeof(struct drm_get_cap) == 16, "drm_get_cap layout");

/* --- request encoding drift checks (the encoding is load-bearing) --------- */
DRM_ASSERT(DRM_IOCTL_VERSION == 0xC0406400u, "VERSION request");
DRM_ASSERT(DRM_IOCTL_GET_CAP == 0xC010640Cu, "GET_CAP request");
DRM_ASSERT(DRM_IOCTL_SET_MASTER == 0x0000641Eu, "SET_MASTER request");
DRM_ASSERT(DRM_IOCTL_MODE_GETRESOURCES == 0xC04064A0u, "GETRESOURCES request");
DRM_ASSERT(DRM_IOCTL_MODE_GETCONNECTOR == 0xC05064A7u, "GETCONNECTOR request");
DRM_ASSERT(DRM_IOCTL_MODE_GETENCODER == 0xC01464A6u, "GETENCODER request");
DRM_ASSERT(DRM_IOCTL_MODE_GETCRTC == 0xC06864A1u, "GETCRTC request");
DRM_ASSERT(DRM_IOCTL_MODE_SETCRTC == 0xC06864A2u, "SETCRTC request");
DRM_ASSERT(DRM_IOCTL_MODE_ADDFB == 0xC01C64AEu, "ADDFB request");
DRM_ASSERT(DRM_IOCTL_MODE_CREATE_DUMB == 0xC02064B2u, "CREATE_DUMB request");
DRM_ASSERT(DRM_IOCTL_MODE_MAP_DUMB == 0xC01064B3u, "MAP_DUMB request");
