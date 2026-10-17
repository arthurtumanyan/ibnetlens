#include "util.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

void ibt_die(const char *msg)
{
    fprintf(stderr, "ibnetlens: %s\n", msg);
    exit(2);
}

void ibt_grow(void **data, size_t *cap, size_t elt, size_t need)
{
    if (need <= *cap)
        return;
    size_t ncap = *cap ? *cap : 64;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2 / elt)
            ibt_die("allocation size overflow");
        ncap *= 2;
    }
    void *p = realloc(*data, ncap * elt);
    if (!p)
        ibt_die("out of memory");
    *data = p;
    *cap = ncap;
}

#define MAX_AUTO_THREADS 16

unsigned ibt_pick_threads(unsigned requested, size_t work, size_t min_per_thread)
{
    if (requested)
        return requested;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned n = cpus < 1 ? 1 : cpus > MAX_AUTO_THREADS ? MAX_AUTO_THREADS : (unsigned)cpus;
    size_t by_work = work / min_per_thread;
    return by_work < n ? (by_work < 1 ? 1 : (unsigned)by_work) : n;
}

typedef struct {
    void (*fn)(void *, unsigned);
    void *ctx;
    unsigned k;
} task;

static void *task_main(void *arg)
{
    task *t = arg;
    t->fn(t->ctx, t->k);
    return NULL;
}

void ibt_parallel_for(unsigned n, void (*fn)(void *, unsigned), void *ctx)
{
    if (n <= 1) {
        fn(ctx, 0);
        return;
    }
    task *tasks = calloc(n, sizeof *tasks);
    pthread_t *th = calloc(n, sizeof *th);
    unsigned char *started = calloc(n, 1);
    if (!tasks || !th || !started)
        ibt_die("out of memory");
    for (unsigned k = 0; k < n; k++)
        tasks[k] = (task){fn, ctx, k};
    for (unsigned k = 1; k < n; k++)
        started[k] = pthread_create(&th[k], NULL, task_main, &tasks[k]) == 0;
    fn(ctx, 0);
    for (unsigned k = 1; k < n; k++) {
        if (started[k])
            pthread_join(th[k], NULL);
        else
            fn(ctx, k);
    }
    free(tasks);
    free(th);
    free(started);
}
