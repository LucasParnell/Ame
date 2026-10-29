#pragma once
// Main-thread-only retirement handshake. Cache workers never use this channel.
static struct shader_cache_channel* display_channel;
static int display_retire(void) {
    struct shader_cache_channel* ch = display_channel;
    if (!ch) {
        return -1;
    }
    const uint64_t mask = DANGO_RING_SLOTS(DANGO_CH_DEFAULT_RING_ORDER) - 1;
    struct ame_gfx_display_drain request = {.id = ++ch->sequence};
    uint64_t head = __atomic_load_n(&ch->tx->head, __ATOMIC_RELAXED);
    if (head - __atomic_load_n(&ch->tx->tail, __ATOMIC_ACQUIRE) > mask) {
        return -1;
    }
    struct dango_ring_slot* out = (struct dango_ring_slot*)(ch->tx + 1) + (head & mask);
    *out = (struct dango_ring_slot){
        .type = AME_GFX_DISPLAY_DRAIN, .length = sizeof(request), .seq = head};
    memcpy(out->payload, &request, sizeof(request));
    __atomic_store_n(&ch->tx->head, head + 1, __ATOMIC_RELEASE);
    if (ioctl(ch->fd, DANGO_CH_NOTIFY, 0)) {
        return -1;
    }
    const uint64_t deadline = now_ns() + 1000000000ULL;
    while (now_ns() < deadline) {
        uint64_t tail = __atomic_load_n(&ch->rx->tail, __ATOMIC_RELAXED);
        if (tail != __atomic_load_n(&ch->rx->head, __ATOMIC_ACQUIRE)) {
            struct dango_ring_slot slot = *((struct dango_ring_slot*)(ch->rx + 1) + (tail & mask));
            if (slot.type != AME_GFX_DISPLAY_DRAIN_REPLY || slot.length != sizeof(request)) {
                return -1;
            }
            __atomic_store_n(&ch->rx->tail, tail + 1, __ATOMIC_RELEASE);
            struct ame_gfx_display_drain reply;
            memcpy(&reply, slot.payload, sizeof(reply));
            return reply.id == request.id && !reply.status && !reply.reserved ? 0 : -1;
        }
        struct pollfd wait = {.fd = ch->fd, .events = POLLIN};
        if (poll(&wait, 1, 20) < 0 && errno != EINTR) {
            return -1;
        }
    }
    return -1;
}
