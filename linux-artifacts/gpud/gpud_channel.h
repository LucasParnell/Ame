#pragma once
/*
 * gpud_channel.h — the `/dev/dango-channel0` transport used by dango-gpud.
 *
 * One open endpoint owns the two SPSC rings (host -> guest and guest -> host)
 * and the Phase I shared staging window. Everything is reached through a single
 * mmap: the rings first, then the staging window at the page-aligned
 * DANGO_CH_STAGING_OFF the shared ABI fixes. Publication is a release store to
 * the ring head followed by the channel NOTIFY ioctl; reception blocks on the
 * device's `read` doorbell (a virtual IRQ), never on a poll loop.
 */
#ifndef MIDIX_GPUD_CHANNEL_H
#define MIDIX_GPUD_CHANNEL_H

#include <stdint.h>

#include "midix/abi/dango_channel_guest.h"

/* The profile fixes the ring order; the server refuses any other geometry. */
#define GPUD_RING_SLOTS DANGO_RING_SLOTS(DANGO_CH_DEFAULT_RING_ORDER)
#define GPUD_RING_MASK (GPUD_RING_SLOTS - 1u)

struct gpud_channel {
    int fd;
    struct dango_channel_layout layout;
    unsigned char *mapping;
    unsigned long mapping_len;
    unsigned long staging_off; /* DANGO_CH_STAGING_OFF(ring_order) */
    struct dango_ring_hdr *rx; /* host -> guest ring view */
    struct dango_ring_hdr *tx; /* guest -> host ring view */
    unsigned char *staging;    /* mapping + staging_off, or 0 when absent */
    unsigned long staging_len;
};

/* Open the endpoint, QUERY the layout and mmap the rings (+ staging). Returns 0
 * on success, -1 on any failure (the channel is left closed). */
int gpud_channel_open(struct gpud_channel *channel);

void gpud_channel_close(struct gpud_channel *channel);

/* Publish one completed slot to the host. Returns 0 on success, -1 when the
 * ring is full or the notification failed (the caller must retry later). */
int gpud_channel_publish(struct gpud_channel *channel, const struct dango_ring_slot *slot);

/* Copy the next unconsumed host slot without consuming it. Returns 1 when a
 * slot is available, 0 when the ring is empty. */
int gpud_channel_peek(const struct gpud_channel *channel, struct dango_ring_slot *slot);

/* Advance the consumer tail past the slot returned by the last peek. */
void gpud_channel_consume(struct gpud_channel *channel);

/* 1 when the guest -> host ring has no free slot. */
int gpud_channel_full(const struct gpud_channel *channel);

/* Block until the host publishes (the device read doorbell). Returns the
 * syscall result: >= 0 when woken, < 0 on error (negative errno). */
long gpud_channel_wait(struct gpud_channel *channel);

/* Poll variant used only for bounded ring backpressure: waits up to
 * `milliseconds` for host activity. Returns > 0 ready, 0 timeout, < 0 error. */
long gpud_channel_wait_timeout(struct gpud_channel *channel, long milliseconds);

#endif /* MIDIX_GPUD_CHANNEL_H */
