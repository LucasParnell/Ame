#pragma once
/* dango_guest_layout.h — the appliance guest's memory layout, selected at build
 * time so the kernel bounds, the guest DTS and the artifact manifest cannot
 * disagree about where guest RAM ends and where the fixed device windows start.
 *
 * Two layouts exist (docs/GPU_APPLIANCE_GUEST_MEMORY.md):
 *
 *   minimal   the freestanding appliance guest: 128 MiB RAM. Default, and the
 *             only layout the tree boots today.
 *   lavapipe  the libc guest carrying a real Vulkan ICD: 512 MiB RAM, which
 *             needs every fixed window moved above the larger RAM boundary.
 *
 * Selection is a preprocessor symbol (DANGO_GUEST_LAYOUT_LAVAPIPE) driven by the
 * build (ANKO_GUEST_LAYOUT / DANGO_LINUX_GUEST), never by a literal in a source
 * file: a window that drifts inside RAM is a guaranteed stage-2 fault.
 *
 * All addresses are guest-physical. C and C++ compatible; no digit separators
 * (GCC rejects them in the body of an `#if`-false block).
 */

#if defined(DANGO_GUEST_LAYOUT_LAVAPIPE)

/* 512 MiB of guest RAM: the measured standalone lavapipe acceptance size, and
 * the reason the stage-2 table is block-mapped. */
#define DANGO_RAM_BASE 0x40000000ULL
#define DANGO_RAM_SIZE 0x20000000ULL /* 512 MiB */

/* Everything fixed moves above RAM. The GPU MMIO aperture is 256 MiB. */
#define DANGO_CH_GUEST_BASE 0x60000000ULL
#define DANGO_GPU_STAGING_BASE 0x61000000ULL
#define DANGO_SHM_GUEST_BASE 0x62000000ULL
#define DANGO_GPU_MMIO_BASE 0x70000000ULL

/* Static load addresses. The initramfs here is the ~186 MiB libc rootfs, so the
 * DTB sits far above it instead of just after it as in the minimal layout. */
#define DANGO_KERNEL_LOAD 0x40200000ULL
#define DANGO_INITRAMFS_LOAD 0x46000000ULL
#define DANGO_DTB_LOAD 0x5F000000ULL

/* Userland VA span reserved for the initramfs, which the boot maps as one object
 * above the appliance's own fixed mappings. The libc rootfs is ~186 MiB. */
#define DANGO_INITRD_VA_SPAN 0x18000000ULL

#else

/* 128 MiB: the bootstrap appliance guest. */
#define DANGO_RAM_BASE 0x40000000ULL
#define DANGO_RAM_SIZE 0x08000000ULL /* 128 MiB */

#define DANGO_CH_GUEST_BASE 0x48000000ULL
#define DANGO_GPU_STAGING_BASE 0x49000000ULL
#define DANGO_SHM_GUEST_BASE 0x4A000000ULL
#define DANGO_GPU_MMIO_BASE 0x50000000ULL

/* Static load addresses: kernel, then the small appliance initramfs, then the
 * DTB just above it. */
#define DANGO_KERNEL_LOAD 0x40200000ULL
#define DANGO_INITRAMFS_LOAD 0x46000000ULL
#define DANGO_DTB_LOAD 0x47000000ULL

/* The appliance initramfs is tens of KiB, so 64 MiB of VA is ample. */
#define DANGO_INITRD_VA_SPAN 0x04000000ULL

#endif

/* Window sizes are layout-independent: only their bases move. */
#define DANGO_CH_GUEST_SIZE 0x20000ULL      /* descriptor page + two rings */
#define DANGO_GPU_STAGING_SIZE 0x00300000ULL /* 3 MiB = one 1024x768 XRGB8888 frame */
#define DANGO_SHM_GUEST_SIZE 0x00100000ULL   /* 1 MiB descriptor window */
#define DANGO_GPU_MMIO_SIZE 0x10000000ULL   /* 256 MiB aperture */

/* The layout invariants. Stated here, once, against the single source: a window
 * inside RAM, or two windows overlapping, must fail the build, not the guest. */
_Static_assert(DANGO_RAM_SIZE != 0 && (DANGO_RAM_SIZE % 0x200000ULL) == 0,
               "guest RAM must be a whole number of 2 MiB blocks");
_Static_assert(DANGO_CH_GUEST_BASE >= DANGO_RAM_BASE + DANGO_RAM_SIZE,
               "channel window must sit above guest RAM");
_Static_assert(DANGO_GPU_STAGING_BASE >= DANGO_CH_GUEST_BASE + DANGO_CH_GUEST_SIZE,
               "staging window must follow the channel window");
_Static_assert(DANGO_SHM_GUEST_BASE >= DANGO_GPU_STAGING_BASE + DANGO_GPU_STAGING_SIZE,
               "shared-region window must follow the staging window");
_Static_assert(DANGO_GPU_MMIO_BASE >= DANGO_SHM_GUEST_BASE + DANGO_SHM_GUEST_SIZE,
               "MMIO aperture must follow the shared-region window");

/* Load addresses sit inside RAM, ordered kernel < initramfs < DTB, and below the
 * first window. The initramfs-to-DTB gap is checked against the real artifact
 * size at build time (build-manifest.sh), which is what caught a 186 MiB rootfs
 * colliding with a DTB placed just above a small one. */
_Static_assert(DANGO_KERNEL_LOAD >= DANGO_RAM_BASE &&
                   DANGO_KERNEL_LOAD < DANGO_INITRAMFS_LOAD &&
                   DANGO_INITRAMFS_LOAD < DANGO_DTB_LOAD,
               "load addresses must be ordered inside RAM");
_Static_assert(DANGO_DTB_LOAD < DANGO_CH_GUEST_BASE,
               "the DTB must load below the first fixed window");
