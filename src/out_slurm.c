#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "ibnetlens.h"

#define NAME_LEN 24
#define SPINE_LEAF_SUFFIX "h"

typedef struct {
    int *level; /* -1 when the switch has no path to a host */
    char *names;
} plan;

typedef struct {
    uint32_t hosts;
    uint32_t switches;
} port_mix;

static port_mix count_ports(const ib_fabric *f, const ib_node *n)
{
    port_mix m = {0, 0};
    for (uint32_t k = 0; k < n->nports; k++) {
        uint32_t peer = f->ports[n->first_port + k].peer_node;
        if (peer == IBT_NONE)
            continue;
        if (f->nodes[peer].kind == IB_CA)
            m.hosts++;
        else if (f->nodes[peer].kind == IB_SWITCH)
            m.switches++;
    }
    return m;
}

/* A leaf serves at least as many hosts as it has links to other switches. A switch with
 * fewer hosts than uplinks is a spine; its own hosts go to a synthetic leaf below it. */
static bool is_leaf(const ib_fabric *f, const ib_node *n)
{
    port_mix m = count_ports(f, n);
    return m.hosts > 0 && m.hosts >= m.switches;
}

static void bfs_levels(const ib_fabric *f, int *level, uint32_t *queue)
{
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < f->nnodes; i++) {
        if (level[i] == 0)
            queue[tail++] = (uint32_t)i;
        else
            level[i] = -1;
    }
    while (head < tail) {
        uint32_t cur = queue[head++];
        const ib_node *n = &f->nodes[cur];
        for (uint32_t k = 0; k < n->nports; k++) {
            uint32_t peer = f->ports[n->first_port + k].peer_node;
            if (peer == IBT_NONE || f->nodes[peer].kind != IB_SWITCH || level[peer] >= 0)
                continue;
            level[peer] = level[cur] + 1;
            queue[tail++] = peer;
        }
    }
}

/* Multi-source BFS from leaf switches; level = hops to the nearest leaf. */
static int assign_levels(const ib_fabric *f, int *level)
{
    uint32_t *queue = malloc((f->nnodes ? f->nnodes : 1) * sizeof *queue);
    if (!queue)
        ibt_die("out of memory");

    for (size_t i = 0; i < f->nnodes; i++)
        level[i] = f->nodes[i].kind == IB_SWITCH && is_leaf(f, &f->nodes[i]) ? 0 : -1;
    bfs_levels(f, level, queue);

    /* a host-bearing switch that no leaf reaches is its own leaf */
    bool promoted = false;
    for (size_t i = 0; i < f->nnodes; i++) {
        if (level[i] < 0 && f->nodes[i].kind == IB_SWITCH && count_ports(f, &f->nodes[i]).hosts > 0) {
            level[i] = 0;
            promoted = true;
        }
    }
    if (promoted)
        bfs_levels(f, level, queue);

    int max = -1;
    for (size_t i = 0; i < f->nnodes; i++) {
        if (level[i] > max)
            max = level[i];
    }
    free(queue);
    return max;
}

/* First whitespace-delimited token of the HCA description is taken as the host name. */
static char *host_name(const char *desc)
{
    size_t n = strcspn(desc, " \t");
    if (n == 0)
        return NULL;
    char *s = malloc(n + 1);
    if (!s)
        ibt_die("out of memory");
    memcpy(s, desc, n);
    s[n] = '\0';
    return s;
}

static void print_leaf_hosts(const ib_fabric *f, const ib_node *n, FILE *out, const char *exclude)
{
    const char **hosts = malloc((n->nports ? n->nports : 1) * sizeof *hosts);
    if (!hosts)
        ibt_die("out of memory");
    size_t m = 0;
    for (uint32_t k = 0; k < n->nports; k++) {
        uint32_t peer = f->ports[n->first_port + k].peer_node;
        if (peer == IBT_NONE || f->nodes[peer].kind != IB_CA)
            continue;
        const ib_node *pn = &f->nodes[peer];
        const char *desc = ib_str(f, pn->name ? pn->name : pn->desc);
        if (exclude && strstr(desc, exclude))
            continue;
        char *h = host_name(desc);
        if (h)
            hosts[m++] = h;
    }
    ib_hostlist_print(out, hosts, m);
    for (size_t i = 0; i < m; i++)
        free((void *)hosts[i]);
    free(hosts);
}

static void print_children(const ib_fabric *f, const plan *pl, uint32_t idx, FILE *out)
{
    const ib_node *n = &f->nodes[idx];
    const char **kids = malloc((n->nports ? n->nports : 1) * sizeof *kids);
    if (!kids)
        ibt_die("out of memory");
    size_t m = 0;
    for (uint32_t k = 0; k < n->nports; k++) {
        uint32_t peer = f->ports[n->first_port + k].peer_node;
        if (peer != IBT_NONE && f->nodes[peer].kind == IB_SWITCH && pl->level[peer] == pl->level[idx] - 1)
            kids[m++] = pl->names + (size_t)peer * NAME_LEN;
    }
    ib_hostlist_print(out, kids, m);
    if (count_ports(f, n).hosts > 0)
        fprintf(out, "%s%s%s", m ? "," : "", pl->names + (size_t)idx * NAME_LEN, SPINE_LEAF_SUFFIX);
    free(kids);
}

/* Switch names are sw<level>n<index>, which keeps Switches= lists compressible. */
static void name_switches(const ib_fabric *f, plan *pl, int max_level)
{
    size_t *count = calloc((size_t)max_level + 1, sizeof *count);
    size_t *seen = calloc((size_t)max_level + 1, sizeof *seen);
    if (!count || !seen)
        ibt_die("out of memory");
    for (size_t i = 0; i < f->nnodes; i++) {
        if (pl->level[i] >= 0)
            count[pl->level[i]]++;
    }
    for (size_t i = 0; i < f->nnodes; i++) {
        int lv = pl->level[i];
        if (lv < 0)
            continue;
        int width = 2;
        for (size_t c = count[lv]; c >= 100; c /= 10)
            width++;
        snprintf(pl->names + i * NAME_LEN, NAME_LEN, "sw%dn%0*zu", lv, width, ++seen[lv]);
    }
    free(count);
    free(seen);
}

void ib_write_slurm(const ib_fabric *f, FILE *out, const char *exclude)
{
    plan pl;
    pl.level = malloc((f->nnodes ? f->nnodes : 1) * sizeof *pl.level);
    pl.names = calloc(f->nnodes ? f->nnodes : 1, NAME_LEN);
    if (!pl.level || !pl.names)
        ibt_die("out of memory");

    int max_level = assign_levels(f, pl.level);
    if (max_level < 0) {
        fprintf(out, "# no host-facing switches found\n");
    } else {
        name_switches(f, &pl, max_level);
        fprintf(out, "# generated by ibnetlens\n");
    }

    for (int lv = max_level; lv >= 0; lv--) {
        for (size_t i = 0; i < f->nnodes; i++) {
            if (pl.level[i] != lv)
                continue;
            const ib_node *n = &f->nodes[i];
            if (lv > 0 && count_ports(f, n).hosts > 0) {
                fprintf(out, "# %s%s = hosts attached to %s\n", pl.names + i * NAME_LEN, SPINE_LEAF_SUFFIX,
                        pl.names + i * NAME_LEN);
                fprintf(out, "SwitchName=%s%s Nodes=", pl.names + i * NAME_LEN, SPINE_LEAF_SUFFIX);
                print_leaf_hosts(f, n, out, exclude);
                fputc('\n', out);
            }
            fprintf(out, "# %s = %" PRIx64 " %s\n", pl.names + i * NAME_LEN, n->guid, ib_str(f, n->desc));
            fprintf(out, "SwitchName=%s %s=", pl.names + i * NAME_LEN, lv == 0 ? "Nodes" : "Switches");
            if (lv == 0)
                print_leaf_hosts(f, n, out, exclude);
            else
                print_children(f, &pl, (uint32_t)i, out);
            fputc('\n', out);
        }
    }
    free(pl.level);
    free(pl.names);
}
