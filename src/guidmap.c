#include "guidmap.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MIN_CAP 64

static size_t slot_of(uint64_t key, size_t mask)
{
    /* splitmix64 finalizer: GUIDs share long vendor prefixes, so low bits alone are poor */
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebULL;
    key ^= key >> 31;
    return (size_t)key & mask;
}

static void alloc_tables(ibt_guidmap *m, size_t cap)
{
    m->keys = malloc(cap * sizeof *m->keys);
    m->vals = malloc(cap * sizeof *m->vals);
    if (!m->keys || !m->vals)
        ibt_die("out of memory");
    memset(m->vals, 0xff, cap * sizeof *m->vals);
    m->cap = cap;
}

void ibt_guidmap_init(ibt_guidmap *m)
{
    m->keys = NULL;
    m->vals = NULL;
    m->cap = 0;
    m->n = 0;
}

void ibt_guidmap_free(ibt_guidmap *m)
{
    free(m->keys);
    free(m->vals);
    ibt_guidmap_init(m);
}

static void rehash(ibt_guidmap *m)
{
    ibt_guidmap old = *m;
    alloc_tables(m, old.cap ? old.cap * 2 : MIN_CAP);
    size_t mask = m->cap - 1;
    for (size_t i = 0; i < old.cap; i++) {
        if (old.vals[i] == IBT_NONE)
            continue;
        size_t s = slot_of(old.keys[i], mask);
        while (m->vals[s] != IBT_NONE)
            s = (s + 1) & mask;
        m->keys[s] = old.keys[i];
        m->vals[s] = old.vals[i];
    }
    free(old.keys);
    free(old.vals);
}

uint32_t ibt_guidmap_put(ibt_guidmap *m, uint64_t key, uint32_t val)
{
    if ((m->n + 1) * 2 > m->cap)
        rehash(m);
    size_t mask = m->cap - 1;
    size_t s = slot_of(key, mask);
    while (m->vals[s] != IBT_NONE) {
        if (m->keys[s] == key)
            return m->vals[s];
        s = (s + 1) & mask;
    }
    m->keys[s] = key;
    m->vals[s] = val;
    m->n++;
    return IBT_NONE;
}

uint32_t ibt_guidmap_get(const ibt_guidmap *m, uint64_t key)
{
    if (!m->cap)
        return IBT_NONE;
    size_t mask = m->cap - 1;
    size_t s = slot_of(key, mask);
    while (m->vals[s] != IBT_NONE) {
        if (m->keys[s] == key)
            return m->vals[s];
        s = (s + 1) & mask;
    }
    return IBT_NONE;
}

void ibt_guidmap_prefetch(const ibt_guidmap *m, uint64_t key)
{
    if (!m->cap)
        return;
    size_t s = slot_of(key, m->cap - 1);
    __builtin_prefetch(&m->vals[s]);
    __builtin_prefetch(&m->keys[s]);
}

void ibt_guidmap_reserve(ibt_guidmap *m, size_t n)
{
    while (m->cap < n * 2 + 2)
        rehash(m);
}
