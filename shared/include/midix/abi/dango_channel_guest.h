#pragma once
#include "midix/abi/dango_guest_layout.h"
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
    /* Appliance boot generation (spec §13.3). Non-zero and monotonic: the
     * kernel advances it every time the appliance (re)establishes its channel,
     * so a guest that reconnects after a clean reset observes a fresh value and
     * every remote object that names the previous boot is invalid. Read it from
     * the descriptor after map; never cache it across a reset. Consumers must
     * treat a change as "all previously held remote objects are gone". */
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
/* Window bases (and RAM bounds) come from dango_guest_layout.h.             */
/* Every address here is guest-physical; IRQs are GIC SPI numbers (>= 32).    */
/* -------------------------------------------------------------------------- */
#define DANGO_CH_GUEST_IRQ 48U
/* DANGO_CH_VERSION (above) is the major; this is the matching minor. */
#define DANGO_CH_VER_MINOR 0U
#define DANGO_CH_DEFAULT_RING_ORDER 6U
#define DANGO_GIC_SPI_BASE 32U

#define DANGO_GPU_ECAM_BASE 0x3f000000ULL
#define DANGO_GPU_ECAM_SIZE 0x00100000ULL
/* Shared staging window the GPU borrows for scanout/upload buffers. It sits
 * between guest RAM (top 0x48000000) and the GPU MMIO aperture (0x50000000). */

/* Dynamic shared arenas (Phase L). Fixed guest window in which the host
 * publishes the descriptor of every region a native owner has attached to this
 * VM. The `dango_shm` device (compatible "ame,dango-shm-v1") reads the
 * read-only descriptor page and exposes each region to appliance userspace
 * through mmap. Region payloads live at owner-chosen guest IPAs named by the
 * descriptor, never inside this window. Sits above the staging window and
 * below the GPU MMIO aperture. */

/* The one bootstrap scanout mode. The staging window is exactly one XRGB8888
 * frame of it, so the mode and the window size are the same fact. */
#define DANGO_GPU_MODE_WIDTH 1024U
#define DANGO_GPU_MODE_HEIGHT 768U
#define DANGO_GPU_MODE_BYTES_PER_PIXEL 4U
#define DANGO_GPU_FRAME_BYTES \
    ((unsigned long long)DANGO_GPU_MODE_WIDTH * DANGO_GPU_MODE_HEIGHT * \
     DANGO_GPU_MODE_BYTES_PER_PIXEL)
_Static_assert(DANGO_GPU_FRAME_BYTES == DANGO_GPU_STAGING_SIZE,
               "staging window must be exactly one bootstrap frame");
#define DANGO_GPU_BAR4_IPA 0x50000000ULL
#define DANGO_GPU_BAR4_SIZE 0x4000ULL
#define DANGO_GPU_INTX_SPI 12U

/* C ring view: a 256-byte header page, then 64-byte slots, in a 4 KiB-aligned
 * window. H2G and G2H rings are equal-sized and adjacent, so the mmap exposes
 * exactly two DANGO_RING_BYTES regions. */
#define DANGO_RING_HEADER_BYTES 256U
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
/* Offset of the shared staging window within the guest channel mapping
 * (relative to DANGO_CH_GUEST_BASE): the descriptor page, then the two rings,
 * then staging at the next page boundary. DANGO_RING_BYTES is already page
 * aligned, so this reduces to DANGO_RING_PAGE_BYTES + 2 * DANGO_RING_BYTES. */
#define DANGO_CH_STAGING_OFF(order) \
    DANGO_ALIGN_UP(2ULL * DANGO_RING_BYTES(order) + DANGO_RING_HEADER_BYTES, \
                   DANGO_RING_PAGE_BYTES)

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
static_assert(DANGO_GPU_BAR4_IPA == DANGO_GPU_MMIO_BASE, "BAR4 sits at the MMIO window base");
static_assert(DANGO_RING_BYTES(6) == 8192, "ring bytes order 6");
static_assert(DANGO_RING_MAPPING_BYTES(6) == 16384, "ring mapping bytes order 6");
static_assert(DANGO_RING_BYTES(6) == DANGO_RING_MAPPING_BYTES(6) / 2, "mapping is two rings");
static_assert(DANGO_CH_GUEST_IRQ >= DANGO_GIC_SPI_BASE, "channel IRQ is an SPI");
static_assert(DANGO_CH_STAGING_OFF(DANGO_CH_DEFAULT_RING_ORDER) % DANGO_RING_PAGE_BYTES == 0,
              "staging offset is page-aligned");
static_assert(DANGO_CH_STAGING_OFF(DANGO_CH_DEFAULT_RING_ORDER) ==
                  DANGO_RING_PAGE_BYTES + DANGO_RING_MAPPING_BYTES(DANGO_CH_DEFAULT_RING_ORDER),
              "staging follows the descriptor page and both rings");
static_assert(DANGO_GPU_STAGING_BASE + DANGO_GPU_STAGING_SIZE <= DANGO_GPU_MMIO_BASE,
              "staging window stays below the GPU MMIO aperture");
static_assert(DANGO_SHM_GUEST_BASE >= DANGO_GPU_STAGING_BASE + DANGO_GPU_STAGING_SIZE,
              "shared-arena window sits above the staging window");
static_assert(DANGO_SHM_GUEST_BASE + DANGO_SHM_GUEST_SIZE <= DANGO_GPU_MMIO_BASE,
              "shared-arena window stays below the GPU MMIO aperture");
#else
_Static_assert(sizeof(struct dango_ring_hdr) == 256, "ring header ABI");
_Static_assert(sizeof(struct dango_ring_slot) == 64, "ring slot ABI");
_Static_assert(DANGO_GPU_BAR4_IPA == DANGO_GPU_MMIO_BASE, "BAR4 sits at the MMIO window base");
_Static_assert(DANGO_RING_BYTES(6) == 8192, "ring bytes order 6");
_Static_assert(DANGO_RING_MAPPING_BYTES(6) == 16384, "ring mapping bytes order 6");
_Static_assert(DANGO_RING_BYTES(6) == DANGO_RING_MAPPING_BYTES(6) / 2, "mapping is two rings");
_Static_assert(DANGO_CH_GUEST_IRQ >= DANGO_GIC_SPI_BASE, "channel IRQ is an SPI");
_Static_assert(DANGO_CH_STAGING_OFF(DANGO_CH_DEFAULT_RING_ORDER) % DANGO_RING_PAGE_BYTES == 0,
               "staging offset is page-aligned");
_Static_assert(DANGO_CH_STAGING_OFF(DANGO_CH_DEFAULT_RING_ORDER) ==
                   DANGO_RING_PAGE_BYTES + DANGO_RING_MAPPING_BYTES(DANGO_CH_DEFAULT_RING_ORDER),
               "staging follows the descriptor page and both rings");
_Static_assert(DANGO_GPU_STAGING_BASE + DANGO_GPU_STAGING_SIZE <= DANGO_GPU_MMIO_BASE,
               "staging window stays below the GPU MMIO aperture");
_Static_assert(DANGO_SHM_GUEST_BASE >= DANGO_GPU_STAGING_BASE + DANGO_GPU_STAGING_SIZE,
               "shared-arena window sits above the staging window");
_Static_assert(DANGO_SHM_GUEST_BASE + DANGO_SHM_GUEST_SIZE <= DANGO_GPU_MMIO_BASE,
               "shared-arena window stays below the GPU MMIO aperture");
#endif
