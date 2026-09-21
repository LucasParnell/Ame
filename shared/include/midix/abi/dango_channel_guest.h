#pragma once
/* C-compatible wire layout shared by Anko, the Linux driver and userspace.
 * All addresses are guest physical addresses, never host physical addresses.
 * The descriptor page is read-only at stage 2; rings are normal WB, RW, XN,
 * and VmDmaClass::None (never registered in an assignment's DMA domain).
 */
#define DANGO_CH_MAGIC 0x4443484eU
#define DANGO_CH_VERSION 1U
#define DANGO_CH_ARM_SUPPORTED 2U
#define DANGO_CH_HVC_NOTIFY 0xc6000001U
#define DANGO_CH_HVC_ARM 0xc6000002U
#define DANGO_CH_QUERY 0x80504400U /* _IOR('D', 0, struct dango_channel_layout) */
#define DANGO_CH_NOTIFY 0x4401U    /* _IO('D', 1) */

struct dango_channel_desc {
    unsigned int magic;
    unsigned short version_major, version_minor;
    unsigned int ring_order, flags;
    unsigned long long boot_generation;
    unsigned long long host_to_guest_ring_pa, guest_to_host_ring_pa;
    unsigned long long shared_staging_pa, shared_staging_size;
    unsigned int guest_irq, reserved0;
};

/* The descriptor occupies the first 64 bytes of a zeroed 4 KiB page.
 * Ring mappings immediately follow that page, H2G then G2H, each rounded up
 * to 4 KiB. mmap(offset=0) on the Linux device exposes only those two rings.
 */
struct dango_channel_layout {
    struct dango_channel_desc desc;
    unsigned int channel_id, ring_bytes, mapping_bytes, reserved0;
};

#if defined(__cplusplus)
static_assert(sizeof(dango_channel_desc) == 64);
static_assert(sizeof(dango_channel_layout) == 80);
#else
_Static_assert(sizeof(struct dango_channel_desc) == 64, "descriptor ABI");
_Static_assert(sizeof(struct dango_channel_layout) == 80, "layout ABI");
#endif
