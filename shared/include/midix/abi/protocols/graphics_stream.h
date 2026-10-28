#pragma once
#include "midix/abi/protocols/graphics.h"
/* Transport assembly is API-neutral and can be tested without EGL or Linux. */
struct ame_gfx_stream {
    unsigned long long id;
    unsigned int used;
    int valid;
    unsigned char data[AME_GFX_BATCH_BYTES];
};
static inline int ame_gfx_stream_chunk(struct ame_gfx_stream* s, const struct ame_gfx_chunk* c) {
    if (c->offset == 0 && c->id > s->id) {
        s->id = c->id;
        s->used = 0;
        s->valid = 1;
    }
    if (!s->valid || c->id != s->id || c->offset != s->used || !c->size || c->size > 32 ||
        c->offset > AME_GFX_BATCH_BYTES - c->size) {
        s->valid = 0;
        return 0;
    }
    for (unsigned i = 0; i < c->size; ++i) {
        s->data[s->used + i] = c->data[i];
    }
    s->used += c->size;
    return 1;
}
static inline int ame_gfx_stream_submit(struct ame_gfx_stream* s, const struct ame_gfx_submit* r) {
    if (!r->bytes && r->id > s->id) {
        s->id = r->id;
        s->used = 0;
        s->valid = 1;
    }
    int ok = s->valid && s->id == r->id && r->bytes == s->used && r->version == AME_GFX_VERSION &&
             ame_gfx_flags_valid(r->flags);
    s->valid = 0;
    return ok;
}
