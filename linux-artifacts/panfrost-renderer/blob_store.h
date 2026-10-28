#pragma once
/* Bounded disposable blob dictionary and checked snapshot format. No pointers,
 * compiler structs or filesystem paths cross the transport boundary. Caller
 * holds the mutex; Mesa callback threads never perform channel or file I/O. */
#include <stdint.h>
#include <string.h>
#include "midix/abi/protocols/graphics_cache.h"
#define BLOB_COUNT 16U
#define BLOB_BYTES 16384U
struct blob_entry {
    uint32_t size;
    unsigned char key[20], data[BLOB_BYTES];
};
struct blob_store {
    struct blob_entry entries[BLOB_COUNT];
    unsigned next;
    uint64_t generation, hits, misses;
};
static inline uint32_t blob_word(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void blob_word_put(unsigned char* p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = (unsigned char)(n >> (i * 8));
    }
}
static inline uint32_t blob_hash(const unsigned char* p, unsigned size) {
    uint32_t h = 2166136261U;
    for (unsigned i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 16777619U;
    }
    return h;
}
static inline void blob_put(struct blob_store* s, const void* key, unsigned key_size,
                            const void* data, unsigned size) {
    if (key_size != 20 || !key || !data || !size || size > BLOB_BYTES) {
        return;
    }
    unsigned i;
    for (i = 0; i < BLOB_COUNT; ++i) {
        if (s->entries[i].size && !memcmp(s->entries[i].key, key, 20)) {
            break;
        }
    }
    if (i == BLOB_COUNT) {
        i = s->next;
        s->next = (s->next + 1) % BLOB_COUNT;
    }
    struct blob_entry* e = &s->entries[i];
    if (e->size == size && !memcmp(e->key, key, 20) && !memcmp(e->data, data, size)) {
        return;
    }
    e->size = size;
    memcpy(e->key, key, 20);
    memcpy(e->data, data, size);
    ++s->generation;
}
static inline unsigned blob_get(struct blob_store* s, const void* key, unsigned key_size,
                                void* data, unsigned capacity) {
    if (key_size != 20 || !key) {
        return 0;
    }
    for (unsigned i = 0; i < BLOB_COUNT; ++i) {
        const struct blob_entry* e = &s->entries[i];
        if (e->size && !memcmp(e->key, key, 20)) {
            if (data && capacity >= e->size) {
                memcpy(data, e->data, e->size);
            }
            ++s->hits;
            return e->size;
        }
    }
    ++s->misses;
    return 0;
}
static inline unsigned blob_pack(const struct blob_store* s, unsigned char* out) {
    unsigned used = 16, count = 0;
    for (unsigned i = 0; i < BLOB_COUNT; ++i) {
        const struct blob_entry* e = &s->entries[i];
        if (!e->size) {
            continue;
        }
        blob_word_put(out + used, e->size);
        memcpy(out + used + 4, e->key, 20);
        memcpy(out + used + 24, e->data, e->size);
        used += 24 + e->size;
        ++count;
    }
    blob_word_put(out, 0x31424341U);
    blob_word_put(out + 4, used);
    blob_word_put(out + 8, count);
    blob_word_put(out + 12, blob_hash(out + 16, used - 16));
    return used;
}
static inline int blob_unpack(struct blob_store* s, const unsigned char* in, unsigned size) {
    if (size < 16 || size > AME_GFX_CACHE_MAX || blob_word(in) != 0x31424341U ||
        blob_word(in + 4) != size || blob_word(in + 8) > BLOB_COUNT ||
        blob_word(in + 12) != blob_hash(in + 16, size - 16)) {
        return 0;
    }
    unsigned at = 16, count = blob_word(in + 8);
    /* Validate the complete snapshot before modifying the live dictionary. */
    for (unsigned i = 0; i < count; ++i) {
        if (size - at < 24) {
            return 0;
        }
        unsigned n = blob_word(in + at);
        if (!n || n > BLOB_BYTES || n > size - at - 24) {
            return 0;
        }
        at += 24 + n;
    }
    if (at != size) {
        return 0;
    }
    memset(s, 0, sizeof(*s));
    at = 16;
    for (unsigned i = 0; i < count; ++i) {
        unsigned n = blob_word(in + at);
        blob_put(s, in + at + 4, 20, in + at + 24, n);
        at += 24 + n;
    }
    s->generation = 0;
    return 1;
}
