/*
 * gpud_channel.c — /dev/dango-channel0 transport for dango-gpud.
 *
 * The ring protocol is the Phase G one: `head`/`tail` counters in the 256-byte
 * ring header, 64-byte slots behind it, release/acquire publication. The
 * consumer (this file, for the host -> guest ring) owns its tail; the producer
 * publishes the head and then doorbells through the device.
 *
 * The single mmap carries the two rings and, when the host staged a window, the
 * shared staging buffer at the page-aligned DANGO_CH_STAGING_OFF. The descriptor
 * page is never mapped into userspace: the layout comes from DANGO_CH_QUERY.
 */
#include "gpud_channel.h"

#include "midix/abi/linux_guest_console.h"
#include "midix/abi/linux_guest_syscall.h"

/* Positive syscall errors are returned negated; EINTR must not be terminal. */
#define GPUD_EINTR 4

static void channel_reset(struct gpud_channel *channel) {
    channel->fd = -1;
    channel->mapping = 0;
    channel->mapping_len = 0;
    channel->staging_off = 0;
    channel->rx = 0;
    channel->tx = 0;
    channel->staging = 0;
    channel->staging_len = 0;
}

int gpud_channel_open(struct gpud_channel *channel) {
    const struct dango_channel_layout *layout;
    unsigned long staging_off;
    unsigned long staging_len;
    unsigned long total;
    long fd;
    long mapped;

    if (!channel) {
        return -1;
    }
    channel_reset(channel);

    fd = mx_openat(MX_AT_FDCWD, DANGO_DEV_CHANNEL, MX_O_RDWR, 0);
    if (fd < 0) {
        return -1;
    }
    channel->fd = (int)fd;
    if (mx_ioctl((int)fd, DANGO_CH_QUERY, &channel->layout) != 0) {
        gpud_channel_close(channel);
        return -1;
    }

    layout = &channel->layout;
    /* Pin the exact geometry the profile fixes: a mismatch is a host/driver
     * contract break, not something to adapt to silently. */
    if (layout->desc.magic != DANGO_CH_MAGIC ||
        layout->desc.version_major != DANGO_CH_VERSION ||
        layout->desc.version_minor != DANGO_CH_VER_MINOR ||
        layout->desc.ring_order != DANGO_CH_DEFAULT_RING_ORDER ||
        layout->ring_bytes != DANGO_RING_BYTES(DANGO_CH_DEFAULT_RING_ORDER) ||
        layout->mapping_bytes != DANGO_RING_MAPPING_BYTES(DANGO_CH_DEFAULT_RING_ORDER)) {
        gpud_channel_close(channel);
        return -1;
    }

    staging_off = (unsigned long)DANGO_CH_STAGING_OFF(layout->desc.ring_order);
    staging_len = (unsigned long)layout->desc.shared_staging_size;
    if (staging_len != 0) {
        /* The window is page-aligned and can never exceed the reserved GPU
         * staging window the guest ABI declares. */
        if ((staging_len & (DANGO_RING_PAGE_BYTES - 1u)) != 0 ||
            staging_len > DANGO_GPU_STAGING_SIZE || staging_off == 0) {
            gpud_channel_close(channel);
            return -1;
        }
        total = staging_off + staging_len;
    } else {
        total = layout->mapping_bytes;
    }

    mapped = mx_mmap(0, total, MX_PROT_READ | MX_PROT_WRITE, MX_MAP_SHARED, (int)fd, 0);
    if (mapped < 0) {
        gpud_channel_close(channel);
        return -1;
    }

    channel->mapping = (unsigned char *)(unsigned long)mapped;
    channel->mapping_len = total;
    channel->staging_off = staging_off;
    /* The driver maps the host -> guest ring first, then the guest -> host
     * ring, so the two ring views are exactly `ring_bytes` apart. */
    channel->rx = (struct dango_ring_hdr *)channel->mapping;
    channel->tx = (struct dango_ring_hdr *)(channel->mapping + layout->ring_bytes);
    if (staging_len != 0) {
        channel->staging = channel->mapping + staging_off;
        channel->staging_len = staging_len;
    }
    return 0;
}

void gpud_channel_close(struct gpud_channel *channel) {
    if (!channel) {
        return;
    }
    if (channel->mapping && channel->mapping_len) {
        (void)mx_munmap(channel->mapping, channel->mapping_len);
    }
    if (channel->fd >= 0) {
        (void)mx_close(channel->fd);
    }
    channel_reset(channel);
}

int gpud_channel_publish(struct gpud_channel *channel, const struct dango_ring_slot *slot) {
    struct dango_ring_hdr *tx = channel->tx;
    struct dango_ring_slot *dst;
    uint64_t head;
    uint64_t tail;

    if (!tx || !slot) {
        return -1;
    }
    head = __atomic_load_n(&tx->head, __ATOMIC_RELAXED);
    tail = __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE);
    if (head - tail >= GPUD_RING_SLOTS) {
        return -1; /* full: the caller retries after the host drains */
    }
    dst = (struct dango_ring_slot *)(tx + 1) + (head & GPUD_RING_MASK);
    *dst = *slot; /* 64-byte copy; lowers to the agent's global memcpy */
    __atomic_store_n(&tx->head, head + 1, __ATOMIC_RELEASE);
    return mx_ioctl(channel->fd, DANGO_CH_NOTIFY, 0) == 0 ? 0 : -1;
}

int gpud_channel_peek(const struct gpud_channel *channel, struct dango_ring_slot *slot) {
    const struct dango_ring_hdr *rx = channel->rx;
    uint64_t tail;
    uint64_t head;

    if (!rx || !slot) {
        return 0;
    }
    tail = __atomic_load_n(&rx->tail, __ATOMIC_RELAXED);
    head = __atomic_load_n(&rx->head, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return 0;
    }
    *slot = *((const struct dango_ring_slot *)(rx + 1) + (tail & GPUD_RING_MASK));
    return 1;
}

void gpud_channel_consume(struct gpud_channel *channel) {
    struct dango_ring_hdr *rx = channel->rx;
    uint64_t tail;
    if (!rx) {
        return;
    }
    tail = __atomic_load_n(&rx->tail, __ATOMIC_RELAXED);
    __atomic_store_n(&rx->tail, tail + 1, __ATOMIC_RELEASE);
}

int gpud_channel_full(const struct gpud_channel *channel) {
    const struct dango_ring_hdr *tx = channel->tx;
    uint64_t head;
    uint64_t tail;
    if (!tx) {
        return 1;
    }
    head = __atomic_load_n(&tx->head, __ATOMIC_RELAXED);
    tail = __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE);
    return head - tail >= GPUD_RING_SLOTS;
}

long gpud_channel_wait(struct gpud_channel *channel) {
    /* The device `read` is a doorbell: it returns as soon as the host has
     * published, and blocks otherwise. It is the only sleep in the hot path. */
    uint64_t interrupts;
    return mx_read(channel->fd, &interrupts, sizeof(interrupts));
}

long gpud_channel_wait_timeout(struct gpud_channel *channel, long milliseconds) {
    struct mx_pollfd pollfd;
    struct mx_timespec timeout;
    pollfd.fd = channel->fd;
    pollfd.events = 1; /* POLLIN */
    pollfd.revents = 0;
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_nsec = (milliseconds % 1000) * 1000000L;
    return mx_ppoll(&pollfd, 1, &timeout);
}
