#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "blob_store.h"
static struct blob_store store, restored;
static unsigned char snapshot[AME_GFX_CACHE_MAX], backup[AME_GFX_CACHE_MAX], value[BLOB_BYTES];
int main(void) {
    unsigned char key[20] = {1}, output[BLOB_BYTES];
    memset(value, 0x5a, sizeof(value));
    blob_put(&store, key, sizeof(key), value, sizeof(value));
    assert(blob_get(&store, key, sizeof(key), output, sizeof(output)) == BLOB_BYTES);
    assert(!memcmp(output, value, sizeof(value)));
    unsigned size = blob_pack(&store, snapshot);
    assert(blob_unpack(&restored, snapshot, size));
    assert(blob_get(&restored, key, 20, NULL, 0) == BLOB_BYTES);
    memcpy(backup, snapshot, size);
    /* Every truncation must fail without modifying a previously loaded cache. */
    for (unsigned n = 0; n < size; ++n) {
        assert(!blob_unpack(&restored, snapshot, n));
    }
    snapshot[size - 1] ^= 1;
    assert(!blob_unpack(&restored, snapshot, size));
    assert(blob_get(&restored, key, 20, output, sizeof(output)) == BLOB_BYTES);
    memcpy(snapshot, backup, size);
    blob_word_put(snapshot + 16, BLOB_BYTES + 1);
    blob_word_put(snapshot + 12, blob_hash(snapshot + 16, size - 16));
    assert(!blob_unpack(&restored, snapshot, size));
    for (unsigned n = 0; n < 32; ++n) {
        key[0] = (unsigned char)n;
        blob_put(&store, key, 20, value, BLOB_BYTES);
    }
    size = blob_pack(&store, snapshot);
    assert(size == AME_GFX_CACHE_MAX);
    assert(blob_unpack(&restored, snapshot, size));
    key[0] = 31;
    assert(blob_get(&restored, key, 20, output, sizeof(output)) == BLOB_BYTES);
    key[0] = 1;
    assert(!blob_get(&restored, key, 20, output, sizeof(output)));
    /* Arbitrary bounded bytes must never escape parser bounds. */
    uint32_t random = 1;
    for (unsigned n = 0; n < 10000; ++n) {
        random = random * 1664525U + 1013904223U;
        unsigned at = random % size;
        snapshot[at] ^= (unsigned char)(random >> 24);
        (void)blob_unpack(&restored, snapshot, size);
    }
    puts("bounded shader cache snapshot and corruption probe passed");
}
