#pragma once
#include <stdint.h>
struct panvk_triangle;
/* Hardware mode requires the PanVK ICD and Mali-T830. Host mode is explicit
 * and is useful only to validate the Vulkan client's API usage and pixels. */
struct panvk_triangle *panvk_triangle_create(const char *library, int hardware);
int panvk_triangle_record(struct panvk_triangle *, float angle);
int panvk_triangle_frame(struct panvk_triangle *, float angle, uint32_t *argb,
                         uint32_t *checksum);
int panvk_triangle_verify_pixels(const uint32_t *argb);
const char *panvk_triangle_device(const struct panvk_triangle *);
void panvk_triangle_destroy(struct panvk_triangle *);
