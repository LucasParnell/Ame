#pragma once
/* C-compatible wire layout shared by Anko, the Linux driver and userspace.
 * All addresses are guest physical addresses, never host physical addresses.
 * The descriptor page is read-only at stage 2; rings are normal WB, RW, XN,
 * and VmDmaClass::None (never registered in an assignment's DMA domain).
 */
#include <stddef.h>

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

/* -------------------------------------------------------------------------- */
/* Guest platform binding: physical windows, IRQ numbers and C ring view.     */
/* Every address here is guest-physical; IRQs are GIC SPI numbers (>= 32).    */
/* -------------------------------------------------------------------------- */
#define DANGO_CH_GUEST_BASE 0x48000000ULL
#define DANGO_CH_GUEST_SIZE 0x20000ULL
#define DANGO_CH_GUEST_IRQ 48U
/* DANGO_CH_VERSION (above) is the major; this is the matching minor. */
#define DANGO_CH_VER_MINOR 0U
#define DANGO_CH_DEFAULT_RING_ORDER 6U
#define DANGO_GIC_SPI_BASE 32U

#define DANGO_GPU_ECAM_BASE 0x3f000000ULL
#define DANGO_GPU_ECAM_SIZE 0x00100000ULL
#define DANGO_GPU_MMIO_BASE 0x50000000ULL
#define DANGO_GPU_MMIO_SIZE 0x10000000ULL
#define DANGO_GPU_BAR4_IPA 0x50000000ULL
#define DANGO_GPU_BAR4_SIZE 0x4000ULL
#define DANGO_GPU_INTX_SPI 12U

/* C ring view: a 256-byte header page, then 64-byte slots, in a 4 KiB-aligned
 * window. H2G and G2H rings are equal-sized and adjacent, so the mmap exposes
 * exactly two DANGO_RING_BYTES regions. */
#define DANGO_RING_HEADER_BYTES 256U
#define DANGO_RING_TAIL_OFF 64U
#define DANGO_RING_SLOT_BYTES 64U
#define DANGO_RING_PAYLOAD_BYTES 48U
#define DANGO_RING_PAGE_BYTES 4096ULL
#define DANGO_RING_SLOTS(order) (1U << (order))
#define DANGO_ALIGN_UP(value, align) (((value) + (align) - 1ULL) & ~((align) - 1ULL))
#define DANGO_RING_BYTES(order) \
    DANGO_ALIGN_UP(DANGO_RING_HEADER_BYTES + \
                       ((unsigned long long)DANGO_RING_SLOT_BYTES << (order)), \
                   DANGO_RING_PAGE_BYTES)
#define DANGO_RING_MAPPING_BYTES(order) (2ULL * DANGO_RING_BYTES(order))

struct dango_ring_hdr {
    unsigned long long head;
    unsigned char head_pad[56];
    unsigned long long tail;
    unsigned char tail_pad[184];
};

struct dango_ring_slot {
    unsigned int type, length;
    unsigned long long seq;
    unsigned char payload[DANGO_RING_PAYLOAD_BYTES];
};

/* Userspace device paths and the DRM driver identity the GPU probe expects. */
#define DANGO_DEV_DRM_CARD0 "/dev/dri/card0"
#define DANGO_DEV_CHANNEL "/dev/dango-channel0"
#define DANGO_DEV_CONSOLE "/dev/console"
#define DANGO_DEV_PROC_INTERRUPTS "/proc/interrupts"
#define DANGO_GPU_DRIVER "virtio_gpu"
#define DANGO_GPU_DRIVER_LEN 10U

#if defined(__cplusplus)
static_assert(sizeof(dango_ring_hdr) == 256, "ring header ABI");
static_assert(sizeof(dango_ring_slot) == 64, "ring slot ABI");
static_assert(offsetof(struct dango_ring_hdr, tail) == DANGO_RING_TAIL_OFF, "ring tail offset");
static_assert(DANGO_GPU_BAR4_IPA == DANGO_GPU_MMIO_BASE, "BAR4 sits at the MMIO window base");
static_assert(DANGO_RING_BYTES(6) == 8192, "ring bytes order 6");
static_assert(DANGO_RING_MAPPING_BYTES(6) == 16384, "ring mapping bytes order 6");
static_assert(DANGO_RING_BYTES(6) == DANGO_RING_MAPPING_BYTES(6) / 2, "mapping is two rings");
static_assert(DANGO_CH_GUEST_IRQ >= DANGO_GIC_SPI_BASE, "channel IRQ is an SPI");
#else
_Static_assert(sizeof(struct dango_ring_hdr) == 256, "ring header ABI");
_Static_assert(sizeof(struct dango_ring_slot) == 64, "ring slot ABI");
_Static_assert(offsetof(struct dango_ring_hdr, tail) == DANGO_RING_TAIL_OFF, "ring tail offset");
_Static_assert(DANGO_GPU_BAR4_IPA == DANGO_GPU_MMIO_BASE, "BAR4 sits at the MMIO window base");
_Static_assert(DANGO_RING_BYTES(6) == 8192, "ring bytes order 6");
_Static_assert(DANGO_RING_MAPPING_BYTES(6) == 16384, "ring mapping bytes order 6");
_Static_assert(DANGO_RING_BYTES(6) == DANGO_RING_MAPPING_BYTES(6) / 2, "mapping is two rings");
_Static_assert(DANGO_CH_GUEST_IRQ >= DANGO_GIC_SPI_BASE, "channel IRQ is an SPI");
#endif
