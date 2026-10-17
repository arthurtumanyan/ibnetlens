#ifndef IBT_UTIL_H
#define IBT_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define IBT_NONE UINT32_MAX

typedef struct {
    const char *p;
    size_t n;
} ibt_slice;

void ibt_die(const char *msg) __attribute__((noreturn));

/* Grows *data so that at least `need` elements of size `elt` fit. Aborts on OOM. */
void ibt_grow(void **data, size_t *cap, size_t elt, size_t need);

/* Calls fn(ctx, k) for k in [0, n), one thread per index; index 0 runs on the caller. */
void ibt_parallel_for(unsigned n, void (*fn)(void *ctx, unsigned k), void *ctx);

/* Thread count for `work` units when each thread should get at least `min_per_thread`.
 * A non-zero `requested` count is honoured as is. */
unsigned ibt_pick_threads(unsigned requested, size_t work, size_t min_per_thread);

#endif
