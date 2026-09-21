#pragma once
/* dango_shm_guest.h — the Dango shared-region guest ABI (Phases L + M).
 *
 * One C-compatible contract, shared by the host that publishes the region
 * descriptor, the built-in guest driver (`linux-artifacts/guest-driver/
 * dango_shm.c`) and the appliance userspace that consumes it
 * (`linux-artifacts/gpud/`). It carries no Linux kernel types and no DRM types:
 * a region is named by an opaque, boot-generation-scoped id and described by
 * guest-physical geometry only. All addresses are guest physical, never host
 * physical (the same rule as dango_channel_guest.h).
 *
 * The host publishes a read-only descriptor page inside the DANGO_SHM_GUEST_*
 * window (dango_channel_guest.h). The `dango_shm` device reads it and exposes
 * each region to appliance userspace through one device node:
 *
 *   /dev/dango-shm0
 *     DANGO_SHM_QUERY          enumerate live regions (ids + geometry)
 *     mmap(mmap_offset)        map one region into userspace
 *     DANGO_SHM_EXPORT_DMABUF  export a *registered* region as a dma-buf fd
 *
 * DMA rule (spec §8.4, §12.3): a region is NOT reachable by the assigned device
 * until the host explicitly registers it `VmDmaClass::SharedGpu` in Anko. That
 * registration is the only thing that sets
 * DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU. The descriptor page is read-only at
 * stage 2, so the guest cannot invent a region or set the bit itself, and the
 * guest driver refuses to export a region without it. There is deliberately no
 * "shared by default" flag: the non-DMA state is the zero flag word, so a bug
 * that maps or exports before registration fails closed.
 */
#include "midix/abi/dango_channel_guest.h"

#define DANGO_SHM_DESC_MAGIC 0x44485344U /* 'DHSD' */
#define DANGO_SHM_VER_MAJOR 1U
#define DANGO_SHM_VER_MINOR 0U

/* Built-in driver device node. Owned by the appliance userspace (mode 0600), the
 * same trust model as DANGO_DEV_CHANNEL. */
#define DANGO_DEV_SHM "/dev/dango-shm0"

/* Wire capacity of the descriptor page. Must be >= Anko's shared-arena table
 * (kernel/include/anko/vm.h kMaxSharedArenas); a host that attaches more than
 * this publishes the first DANGO_SHM_MAX_REGIONS in `count` order. The whole
 * descriptor is asserted to fit one page below. */
#define DANGO_SHM_MAX_REGIONS 16U

/* Region flags. DMA_SHARED_GPU is set by the host only after Anko's explicit
 * SharedGpu registration succeeded (vm_shared.cc shared_arena_register_dma).
 * While it is clear the region is VmDmaClass::None: the guest CPU may read/write
 * it, no device may DMA from it, and the guest driver refuses to export it. */
#define DANGO_SHM_REGION_FLAG_NONE 0U
#define DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU (1U << 0)
#define DANGO_SHM_REGION_FLAG_MASK (DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU)

/* Export flags (reserved, must be zero today). */
#define DANGO_SHM_EXPORT_FLAG_NONE 0U

/* One region, as published in the descriptor page. `guest_pa` is the region's
 * base in this VM's guest physical address space. */
struct dango_shm_region {
    unsigned long long id;       /* host shared_id, 1-based; 0 is never a region */
    unsigned long long guest_pa; /* guest-physical base, PAGE_SIZE aligned */
    unsigned long long size;     /* bytes, PAGE_SIZE multiple */
    unsigned int flags;          /* DANGO_SHM_REGION_FLAG_* */
    unsigned int reserved0;      /* must be zero */
};

/* The read-only descriptor page: a 4 KiB page whose first bytes are this header
 * followed by `count` region entries. `boot_generation` matches the channel's;
 * a change invalidates every previously held region id. */
struct dango_shm_desc {
    unsigned int magic; /* DANGO_SHM_DESC_MAGIC */
    unsigned short version_major, version_minor;
    unsigned int count;     /* live regions, <= DANGO_SHM_MAX_REGIONS */
    unsigned int reserved0; /* must be zero */
    unsigned long long boot_generation;
    struct dango_shm_region regions[DANGO_SHM_MAX_REGIONS];
};

/* DANGO_SHM_QUERY: enumerate regions. `index` in; `region`, `count` and
 * `boot_generation` out. The `mmap_offset` in the reply is the byte offset to
 * pass to mmap(2) on DANGO_DEV_SHM (a per-slot page, never 0). */
struct dango_shm_query {
    struct dango_shm_region region; /* out */
    unsigned int index;             /* in */
    unsigned int count;             /* out: live regions */
    unsigned long long boot_generation; /* out */
    unsigned long long mmap_offset;     /* out */
};

/* DANGO_SHM_EXPORT_DMABUF: export a region as a dma-buf fd. Only accepted when
 * the region carries DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU; otherwise the driver
 * returns -EPERM without touching the region. */
struct dango_shm_export {
    unsigned long long id;              /* in */
    unsigned long long boot_generation; /* in */
    int fd;                             /* out: dma-buf fd */
    unsigned int flags;                 /* in: DANGO_SHM_EXPORT_FLAG_* */
};

/* ioctl request encodings (_IOWR('S', nr, struct)). Pinned so a drift in either
 * the driver or userspace is a build failure, not a silent ENOTTY at runtime. */
#define DANGO_SHM_QUERY 0xc0385300U         /* _IOWR('S', 0, struct dango_shm_query) */
#define DANGO_SHM_EXPORT_DMABUF 0xc0185301U /* _IOWR('S', 1, struct dango_shm_export) */

#if defined(__cplusplus)
static_assert(sizeof(struct dango_shm_region) == 32, "region ABI");
static_assert(sizeof(struct dango_shm_desc) == 536, "descriptor ABI");
static_assert(sizeof(struct dango_shm_query) == 56, "query ABI");
static_assert(sizeof(struct dango_shm_export) == 24, "export ABI");
#else
_Static_assert(sizeof(struct dango_shm_region) == 32, "region ABI");
_Static_assert(sizeof(struct dango_shm_desc) == 536, "descriptor ABI");
_Static_assert(sizeof(struct dango_shm_query) == 56, "query ABI");
_Static_assert(sizeof(struct dango_shm_export) == 24, "export ABI");
#endif

/* The non-DMA state is the zero flag word, and the only accepted flag is the
 * SharedGpu registration marker: nothing can be described as shareable by
 * omission, and an unknown bit is rejected rather than ignored. */
_Static_assert(DANGO_SHM_REGION_FLAG_NONE == 0U, "the non-DMA state is the zero flag word");
_Static_assert(DANGO_SHM_REGION_FLAG_MASK == DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU,
               "the SharedGpu bit is the only accepted flag");

/* The descriptor must fit the first page of the DANGO_SHM_GUEST window, and the
 * window must be big enough to hold it. Both are fixed at compile time. */
_Static_assert(sizeof(struct dango_shm_desc) <= DANGO_RING_PAGE_BYTES,
               "descriptor page fits 4 KiB");
_Static_assert(sizeof(struct dango_shm_desc) <= DANGO_SHM_GUEST_SIZE,
               "descriptor fits the shared-arena window");
_Static_assert(DANGO_SHM_MAX_REGIONS > 0U, "at least one region must fit");
