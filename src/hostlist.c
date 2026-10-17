#include <stdlib.h>
#include <string.h>

#include "ibnetlens.h"

typedef struct {
    const char *name;
    size_t plen;    /* prefix length; whole name when there is no numeric suffix */
    unsigned width; /* digit count when zero-padded, else 0 */
    unsigned digits;
    uint64_t num;
    bool numeric;
} item;

static item make_item(const char *name)
{
    size_t len = strlen(name);
    size_t i = len;
    while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9')
        i--;
    item it = {name, len, 0, 0, false, 0};
    size_t digits = len - i;
    if (digits == 0 || digits > 18)
        return it;
    it.plen = i;
    it.numeric = true;
    it.digits = (unsigned)digits;
    for (size_t k = i; k < len; k++)
        it.num = it.num * 10 + (uint64_t)(name[k] - '0');
    if (digits > 1 && name[i] == '0')
        it.width = (unsigned)digits;
    return it;
}

static int cmp_item_width(const void *pa, const void *pb, bool by_width)
{
    const item *a = pa, *b = pb;
    size_t n = a->plen < b->plen ? a->plen : b->plen;
    int c = memcmp(a->name, b->name, n);
    if (c)
        return c;
    if (a->plen != b->plen)
        return a->plen < b->plen ? -1 : 1;
    if (a->numeric != b->numeric)
        return a->numeric ? 1 : -1;
    if (by_width && a->width != b->width)
        return a->width < b->width ? -1 : 1;
    return (a->num > b->num) - (a->num < b->num);
}

static int cmp_item_prefix(const void *a, const void *b)
{
    return cmp_item_width(a, b, false);
}

static int cmp_item(const void *a, const void *b)
{
    return cmp_item_width(a, b, true);
}

/* "node100" belongs to the padded group "node001..node100"; needs items sorted by prefix. */
static void adopt_padding(item *items, size_t n)
{
    for (size_t i = 0; i < n;) {
        size_t j = i;
        unsigned mask = 0;
        while (j < n && items[j].plen == items[i].plen && memcmp(items[j].name, items[i].name, items[i].plen) == 0) {
            if (items[j].width)
                mask |= 1u << items[j].width;
            j++;
        }
        for (size_t k = i; mask && k < j; k++) {
            if (items[k].numeric && !items[k].width && (mask & (1u << items[k].digits)))
                items[k].width = items[k].digits;
        }
        i = j;
    }
}

static bool same_group(const item *a, const item *b)
{
    return a->numeric && b->numeric && a->plen == b->plen && a->width == b->width &&
           memcmp(a->name, b->name, a->plen) == 0;
}

static void put_num(FILE *out, uint64_t v, unsigned width)
{
    fprintf(out, "%0*llu", (int)width, (unsigned long long)v);
}

static void put_group(FILE *out, const item *g, size_t n)
{
    if (n == 1) {
        fputs(g[0].name, out);
        return;
    }
    fprintf(out, "%.*s[", (int)g[0].plen, g[0].name);
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j + 1 < n && g[j + 1].num <= g[j].num + 1)
            j++;
        if (i)
            fputc(',', out);
        put_num(out, g[i].num, g[i].width);
        if (j > i) {
            fputc('-', out);
            put_num(out, g[j].num, g[j].width);
        }
        i = j + 1;
    }
    fputc(']', out);
}

void ib_hostlist_print(FILE *out, const char **names, size_t n)
{
    if (n == 0)
        return;
    item *items = malloc(n * sizeof *items);
    if (!items)
        ibt_die("out of memory");
    for (size_t i = 0; i < n; i++)
        items[i] = make_item(names[i]);
    qsort(items, n, sizeof *items, cmp_item_prefix);
    adopt_padding(items, n);
    qsort(items, n, sizeof *items, cmp_item);

    /* drop exact duplicates */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m && strcmp(items[m - 1].name, items[i].name) == 0)
            continue;
        items[m++] = items[i];
    }

    for (size_t i = 0; i < m;) {
        size_t j = i + 1;
        while (j < m && same_group(&items[i], &items[j]))
            j++;
        if (i)
            fputc(',', out);
        put_group(out, items + i, j - i);
        i = j;
    }
    free(items);
}
