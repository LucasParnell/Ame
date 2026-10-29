#pragma once
/*
 * raster.h — flat-shaded triangle rasterizer shared by the native display path
 * and the guest appliance.
 *
 * One implementation, two users: io.framebuffer legitimately renders into the
 * phone panel, and the guest render service renders into the Dango staging
 * window. Keeping the arithmetic here means the same commands produce the same
 * image on either side, and the host regression can assert exact pixels.
 *
 * This is a software rasterizer. It runs on the CPU of whichever side calls it;
 * it does not program the Mali-T830 and must never be reported as GPU execution.
 *
 * C and C++ compatible: macros and static inline only, no allocation.
 */

/* XRGB8888, matching the framebuffer and staging conventions: little-endian in
 * memory as B, G, R, X. */
#define AME_RASTER_BYTES_PER_PIXEL 4U

struct ame_raster_target {
    unsigned char* pixels; /* first byte of the top-left pixel */
    unsigned int width;    /* pixels */
    unsigned int height;   /* pixels */
    unsigned int pitch;    /* bytes per row; may exceed width * bpp */
};

/* Screen-space vertex, integer pixels, y grows downward. */
struct ame_raster_vertex {
    int x;
    int y;
};

/* Half-open visibility window inside the target. Commands are clipped to it so
 * a renderer can own a sub-rectangle without touching its neighbours. */
struct ame_raster_clip {
    int x0, y0, x1, y1;
};

static inline struct ame_raster_clip ame_raster_full_clip(const struct ame_raster_target* t) {
    struct ame_raster_clip c;
    c.x0 = 0;
    c.y0 = 0;
    c.x1 = (int)t->width;
    c.y1 = (int)t->height;
    return c;
}

static inline int ame_raster_intersect(struct ame_raster_clip* c, struct ame_raster_clip other) {
    if (other.x0 > c->x0) {
        c->x0 = other.x0;
    }
    if (other.y0 > c->y0) {
        c->y0 = other.y0;
    }
    if (other.x1 < c->x1) {
        c->x1 = other.x1;
    }
    if (other.y1 < c->y1) {
        c->y1 = other.y1;
    }
    return c->x1 > c->x0 && c->y1 > c->y0;
}

static inline void ame_raster_store(struct ame_raster_target* t, int x, int y, unsigned int rgba) {
    unsigned char* p =
        t->pixels + (unsigned long)y * t->pitch + (unsigned long)x * AME_RASTER_BYTES_PER_PIXEL;
    /* Little-endian XRGB8888: low byte is blue. */
    p[0] = (unsigned char)(rgba & 0xffu);
    p[1] = (unsigned char)((rgba >> 8) & 0xffu);
    p[2] = (unsigned char)((rgba >> 16) & 0xffu);
    p[3] = (unsigned char)((rgba >> 24) & 0xffu);
}

/* Fill the clip rectangle. Returns the number of pixels written. */
static inline unsigned int ame_raster_clear(struct ame_raster_target* t,
                                            struct ame_raster_clip clip, unsigned int rgba) {
    unsigned int written = 0;
    int x, y;
    if (!t || !t->pixels) {
        return 0;
    }
    if (!ame_raster_intersect(&clip, ame_raster_full_clip(t))) {
        return 0;
    }
    for (y = clip.y0; y < clip.y1; ++y) {
        for (x = clip.x0; x < clip.x1; ++x) {
            ame_raster_store(t, x, y, rgba);
            ++written;
        }
    }
    return written;
}

/*
 * Twice the signed area of the edge (a -> b) measured at (px, py). Sign tells
 * which side of the edge a point falls on; magnitude is twice the subtended
 * area, so the sum over the three edges is twice the triangle's area.
 */
static inline long long ame_raster_edge(const struct ame_raster_vertex* a,
                                        const struct ame_raster_vertex* b, int px, int py) {
    return (long long)(px - a->x) * (b->y - a->y) - (long long)(py - a->y) * (b->x - a->x);
}

/*
 * Flat-shade a triangle into the target, clipped to `clip`.
 *
 * Coverage is an inclusive edge test (all three w >= 0). For opaque flat fills
 * that differs from the top-left rule only in that two triangles sharing an edge
 * both cover it; with one colour per triangle the later draw wins, which is
 * deterministic for a fixed call order. That is enough here and simpler than a
 * fill rule nothing needs — but it means this rasterizer must not be used for
 * blending.
 *
 * Returns the number of pixels written; 0 for a degenerate triangle.
 */
static inline unsigned int ame_raster_triangle(struct ame_raster_target* t,
                                               struct ame_raster_clip clip,
                                               struct ame_raster_vertex v0,
                                               struct ame_raster_vertex v1,
                                               struct ame_raster_vertex v2, unsigned int rgba) {
    struct ame_raster_vertex tmp;
    long long area;
    int min_x, max_x, min_y, max_y, x, y;
    unsigned int written = 0;

    if (!t || !t->pixels) {
        return 0;
    }
    if (!ame_raster_intersect(&clip, ame_raster_full_clip(t))) {
        return 0;
    }

    /* Normalise the winding so the interior is always on the positive side. */
    area = ame_raster_edge(&v0, &v1, v2.x, v2.y);
    if (area == 0) {
        return 0;
    }
    if (area < 0) {
        tmp = v1;
        v1 = v2;
        v2 = tmp;
    }

    min_x = v0.x < v1.x ? (v0.x < v2.x ? v0.x : v2.x) : (v1.x < v2.x ? v1.x : v2.x);
    max_x = v0.x > v1.x ? (v0.x > v2.x ? v0.x : v2.x) : (v1.x > v2.x ? v1.x : v2.x);
    min_y = v0.y < v1.y ? (v0.y < v2.y ? v0.y : v2.y) : (v1.y < v2.y ? v1.y : v2.y);
    max_y = v0.y > v1.y ? (v0.y > v2.y ? v0.y : v2.y) : (v1.y > v2.y ? v1.y : v2.y);

    if (min_x < clip.x0) {
        min_x = clip.x0;
    }
    if (min_y < clip.y0) {
        min_y = clip.y0;
    }
    if (max_x > clip.x1 - 1) {
        max_x = clip.x1 - 1;
    }
    if (max_y > clip.y1 - 1) {
        max_y = clip.y1 - 1;
    }
    if (min_x > max_x || min_y > max_y) {
        return 0;
    }

    for (y = min_y; y <= max_y; ++y) {
        for (x = min_x; x <= max_x; ++x) {
            const long long w0 = ame_raster_edge(&v1, &v2, x, y);
            const long long w1 = ame_raster_edge(&v2, &v0, x, y);
            const long long w2 = ame_raster_edge(&v0, &v1, x, y);
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            ame_raster_store(t, x, y, rgba);
            ++written;
        }
    }
    return written;
}
