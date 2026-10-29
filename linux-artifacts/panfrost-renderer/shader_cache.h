#pragma once
#include <pthread.h>
#include "blob_store.h"
static struct blob_store shader_blobs;
static pthread_mutex_t shader_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char shader_snapshot[AME_GFX_CACHE_MAX];
static uint64_t shader_saved_generation;
static int shader_cache_loaded;
static void cache_set(const void* key, EGLsizeiANDROID key_size, const void* value,
                      EGLsizeiANDROID size) {
    if (key_size != 20 || size <= 0 || size > BLOB_BYTES) {
        return;
    }
    pthread_mutex_lock(&shader_cache_mutex);
    blob_put(&shader_blobs, key, (unsigned)key_size, value, (unsigned)size);
    pthread_mutex_unlock(&shader_cache_mutex);
}
static EGLsizeiANDROID cache_get(const void* key, EGLsizeiANDROID key_size, void* value,
                                 EGLsizeiANDROID size) {
    if (key_size != 20 || size < 0) {
        return 0;
    }
    pthread_mutex_lock(&shader_cache_mutex);
    unsigned found = blob_get(&shader_blobs, key, (unsigned)key_size, value, (unsigned)size);
    pthread_mutex_unlock(&shader_cache_mutex);
    return found;
}
static void shader_cache_install(EGLDisplay display) {
    PFNEGLSETBLOBCACHEFUNCSANDROIDPROC set =
        (PFNEGLSETBLOBCACHEFUNCSANDROIDPROC)eglGetProcAddress("eglSetBlobCacheFuncsANDROID");
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (set && extensions && strstr(extensions, "EGL_ANDROID_blob_cache")) {
        set(display, cache_set, cache_get);
        printf("panfrost cache: bounded EGL blob callbacks installed\n");
    }
}
struct shader_cache_channel {
    int fd;
    struct dango_ring_hdr *rx, *tx;
    unsigned char* transfer;
    uint64_t sequence;
};
static int cache_exchange(struct shader_cache_channel* ch, struct ame_gfx_cache_io* io) {
    const uint64_t mask = DANGO_RING_SLOTS(DANGO_CH_DEFAULT_RING_ORDER) - 1;
    io->id = ++ch->sequence;
    uint64_t head = __atomic_load_n(&ch->tx->head, __ATOMIC_RELAXED);
    if (head - __atomic_load_n(&ch->tx->tail, __ATOMIC_ACQUIRE) > mask) {
        return 0;
    }
    struct dango_ring_slot* out = (struct dango_ring_slot*)(ch->tx + 1) + (head & mask);
    *out =
        (struct dango_ring_slot){.type = AME_GFX_CACHE_REQUEST, .length = sizeof(*io), .seq = head};
    memcpy(out->payload, io, sizeof(*io));
    __atomic_store_n(&ch->tx->head, head + 1, __ATOMIC_RELEASE);
    if (ioctl(ch->fd, DANGO_CH_NOTIFY, 0)) {
        return 0;
    }
    uint64_t deadline = now_ns() + 10000000000ULL;
    while (now_ns() < deadline) {
        uint64_t tail = __atomic_load_n(&ch->rx->tail, __ATOMIC_RELAXED);
        if (tail != __atomic_load_n(&ch->rx->head, __ATOMIC_ACQUIRE)) {
            struct dango_ring_slot slot = *((struct dango_ring_slot*)(ch->rx + 1) + (tail & mask));
            // The manager serializes one request; consuming another message
            // here would silently lose a graphics submission.
            if (slot.type != AME_GFX_CACHE_REPLY || slot.length != sizeof(*io)) {
                return 0;
            }
            __atomic_store_n(&ch->rx->tail, tail + 1, __ATOMIC_RELEASE);
            struct ame_gfx_cache_io reply;
            memcpy(&reply, slot.payload, sizeof(reply));
            if (reply.id != io->id || reply.operation != io->operation ||
                reply.offset != io->offset || reply.reserved || reply.status || !reply.size ||
                reply.size > io->size || reply.total > AME_GFX_CACHE_MAX ||
                reply.offset > reply.total || reply.size > reply.total - reply.offset) {
                return 0;
            }
            *io = reply;
            return 1;
        }
        struct pollfd wait = {.fd = ch->fd, .events = POLLIN};
        if (poll(&wait, 1, 100) < 0 && errno != EINTR) {
            return 0;
        }
    }
    return 0;
}
static void shader_cache_checkpoint(struct shader_cache_channel* ch) {
    if (!shader_cache_loaded) {
        shader_cache_loaded = 1;
        unsigned offset = 0, total = 0;
        do {
            struct ame_gfx_cache_io io = {
                .operation = AME_GFX_CACHE_LOAD, .offset = offset, .size = AME_GFX_CACHE_TRANSFER};
            if (!cache_exchange(ch, &io) || (offset && io.total != total)) {
                offset = 0;
                break;
            }
            total = io.total;
            memcpy(shader_snapshot + offset, ch->transfer, io.size);
            offset += io.size;
        } while (offset < total);
        if (offset) {
            pthread_mutex_lock(&shader_cache_mutex);
            int valid = blob_unpack(&shader_blobs, shader_snapshot, offset);
            pthread_mutex_unlock(&shader_cache_mutex);
            printf("panfrost cache: snapshot %s bytes=%u\n", valid ? "loaded" : "rejected", offset);
        }
    }
    pthread_mutex_lock(&shader_cache_mutex);
    uint64_t generation = shader_blobs.generation;
    unsigned size =
        generation != shader_saved_generation ? blob_pack(&shader_blobs, shader_snapshot) : 0;
    uint64_t hits = shader_blobs.hits, misses = shader_blobs.misses;
    pthread_mutex_unlock(&shader_cache_mutex);
    if (!size) {
        return;
    }
    for (unsigned offset = 0; offset < size;) {
        unsigned count =
            size - offset > AME_GFX_CACHE_TRANSFER ? AME_GFX_CACHE_TRANSFER : size - offset;
        memcpy(ch->transfer, shader_snapshot + offset, count);
        struct ame_gfx_cache_io io = {
            .operation = AME_GFX_CACHE_SAVE, .offset = offset, .size = count, .total = size};
        if (!cache_exchange(ch, &io) || io.total != size || io.size != count) {
            return;
        }
        offset += count;
    }
    shader_saved_generation = generation;
    printf("panfrost cache: saved bytes=%u hits=%llu misses=%llu\n", size, (unsigned long long)hits,
           (unsigned long long)misses);
}
