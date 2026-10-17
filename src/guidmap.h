#ifndef IBT_GUIDMAP_H
#define IBT_GUIDMAP_H

#include <stddef.h>
#include <stdint.h>

/* Open-addressing map from a 64-bit GUID to a 32-bit index. */
typedef struct {
    uint64_t *keys;
    uint32_t *vals; /* IBT_NONE marks a free slot */
    size_t cap;
    size_t n;
} ibt_guidmap;

void ibt_guidmap_init(ibt_guidmap *m);
void ibt_guidmap_free(ibt_guidmap *m);

/* Grows the table up front so that `n` entries insert without rehashing. */
void ibt_guidmap_reserve(ibt_guidmap *m, size_t n);

/* Inserts key; if already present returns the stored value and leaves it untouched, else IBT_NONE. */
uint32_t ibt_guidmap_put(ibt_guidmap *m, uint64_t key, uint32_t val);
uint32_t ibt_guidmap_get(const ibt_guidmap *m, uint64_t key);

/* Hints the cache lines a later ibt_guidmap_get for `key` will touch. */
void ibt_guidmap_prefetch(const ibt_guidmap *m, uint64_t key);

#endif
