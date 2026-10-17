#include <stdlib.h>
#include <string.h>

#include "ibnetlens.h"

static const char *const speed_names[IB_SPEED_COUNT] = {
    "?", "SDR", "DDR", "QDR", "FDR10", "FDR", "EDR", "HDR", "NDR", "XDR",
};

/* Effective per-lane data rate in Mb/s. */
static const uint64_t lane_mbps[IB_SPEED_COUNT] = {
    0, 2000, 4000, 8000, 10000, 14000, 25000, 50000, 100000, 200000,
};

void ib_fabric_init(ib_fabric *f)
{
    memset(f, 0, sizeof *f);
    ibt_guidmap_init(&f->map);
    /* offset 0 is the empty string */
    ibt_grow((void **)&f->pool, &f->pool_cap, 1, 1);
    f->pool[0] = '\0';
    f->pool_len = 1;
}

void ib_fabric_free(ib_fabric *f)
{
    free(f->nodes);
    free(f->ports);
    free(f->pool);
    free(f->dups);
    free(f->names);
    ibt_guidmap_free(&f->map);
    memset(f, 0, sizeof *f);
}

void ib_fabric_reserve(ib_fabric *f, size_t bytes)
{
    /* typical dumps: one link line per ~90 bytes, one node block per ~250 bytes */
    ibt_grow((void **)&f->nodes, &f->nodes_cap, sizeof *f->nodes, bytes / 250 + 16);
    ibt_grow((void **)&f->ports, &f->ports_cap, sizeof *f->ports, bytes / 90 + 16);
    ibt_grow((void **)&f->pool, &f->pool_cap, 1, bytes / 12 + 16);
}

uint32_t ib_node_add(ib_fabric *f)
{
    if (f->nnodes >= IBT_NONE - 1)
        ibt_die("too many nodes");
    ibt_grow((void **)&f->nodes, &f->nodes_cap, sizeof *f->nodes, f->nnodes + 1);
    memset(&f->nodes[f->nnodes], 0, sizeof *f->nodes);
    return (uint32_t)f->nnodes++;
}

uint32_t ib_port_add(ib_fabric *f)
{
    if (f->nports >= IBT_NONE - 1)
        ibt_die("too many ports");
    ibt_grow((void **)&f->ports, &f->ports_cap, sizeof *f->ports, f->nports + 1);
    ib_port *p = &f->ports[f->nports];
    memset(p, 0, sizeof *p);
    p->peer_node = IBT_NONE;
    p->peer_idx = IBT_NONE;
    return (uint32_t)f->nports++;
}

uint32_t ib_intern(ib_fabric *f, ibt_slice s)
{
    if (s.n == 0)
        return 0;
    if (f->pool_len + s.n + 1 > UINT32_MAX)
        ibt_die("string pool overflow");
    ibt_grow((void **)&f->pool, &f->pool_cap, 1, f->pool_len + s.n + 1);
    uint32_t off = (uint32_t)f->pool_len;
    memcpy(f->pool + off, s.p, s.n);
    f->pool[off + s.n] = '\0';
    f->pool_len += s.n + 1;
    return off;
}

const char *ib_str(const ib_fabric *f, uint32_t off)
{
    return f->pool + off;
}

const char *ib_kind_name(ib_kind k)
{
    switch (k) {
    case IB_SWITCH: return "switch";
    case IB_ROUTER: return "router";
    default: return "ca";
    }
}

const char *ib_speed_name(ib_speed s)
{
    return s < IB_SPEED_COUNT ? speed_names[s] : speed_names[0];
}

ib_speed ib_speed_from(const char *s, size_t n)
{
    for (int i = 1; i < IB_SPEED_COUNT; i++) {
        if (strlen(speed_names[i]) == n && memcmp(speed_names[i], s, n) == 0)
            return (ib_speed)i;
    }
    return IB_SPEED_UNKNOWN;
}

uint64_t ib_rate_mbps(ib_speed s, unsigned width)
{
    return s < IB_SPEED_COUNT ? lane_mbps[s] * width : 0;
}

int ib_compare_guid(uint64_t a, uint64_t b)
{
    return (a > b) - (a < b);
}

static uint64_t hash_name(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static void names_rehash(ib_fabric *f)
{
    size_t ncap = f->names_cap ? f->names_cap * 2 : 64;
    uint32_t *slots = malloc(ncap * sizeof *slots);
    if (!slots)
        ibt_die("out of memory");
    memset(slots, 0xff, ncap * sizeof *slots);
    for (size_t i = 0; i < f->names_cap; i++) {
        if (f->names[i] == IBT_NONE)
            continue;
        size_t s = hash_name(ib_str(f, f->nodes[f->names[i]].name)) & (ncap - 1);
        while (slots[s] != IBT_NONE)
            s = (s + 1) & (ncap - 1);
        slots[s] = f->names[i];
    }
    free(f->names);
    f->names = slots;
    f->names_cap = ncap;
}

uint32_t ib_name_add(ib_fabric *f, uint32_t idx)
{
    if ((f->names_n + 1) * 2 > f->names_cap)
        names_rehash(f);
    const char *name = ib_str(f, f->nodes[idx].name);
    size_t mask = f->names_cap - 1;
    size_t s = hash_name(name) & mask;
    while (f->names[s] != IBT_NONE) {
        if (strcmp(ib_str(f, f->nodes[f->names[s]].name), name) == 0)
            return f->names[s];
        s = (s + 1) & mask;
    }
    f->names[s] = idx;
    f->names_n++;
    return IBT_NONE;
}

uint32_t ib_name_find(const ib_fabric *f, const char *name)
{
    if (!f->names_cap)
        return IBT_NONE;
    size_t mask = f->names_cap - 1;
    size_t s = hash_name(name) & mask;
    while (f->names[s] != IBT_NONE) {
        if (strcmp(ib_str(f, f->nodes[f->names[s]].name), name) == 0)
            return f->names[s];
        s = (s + 1) & mask;
    }
    return IBT_NONE;
}
