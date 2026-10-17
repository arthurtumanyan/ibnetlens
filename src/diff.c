#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ibnetlens.h"

typedef struct {
    uint64_t guid;
    uint32_t idx;
} node_key;

typedef struct {
    uint64_t guid;
    uint16_t port;
    uint32_t idx;
} link_key;

typedef struct {
    FILE *out;
    bool json;
    size_t count;
} sink;

static char kind_letter(ib_kind k)
{
    return k == IB_SWITCH ? 'S' : k == IB_ROUTER ? 'R' : 'H';
}

static void emit(sink *s, char op, const char *what, const char *id, const char *fmt, ...)
{
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    s->count++;
    if (s->json) {
        fprintf(s->out, "{\"op\":\"%s\",\"type\":\"%s\",\"id\":\"%s\",\"detail\":",
                op == '+' ? "added" : op == '-' ? "removed" : "changed", what, id);
        ib_json_str(s->out, detail);
        fputs("}\n", s->out);
    } else {
        fprintf(s->out, "%c %s %s%s%s\n", op, what, id, *detail ? " " : "", detail);
    }
}

static int cmp_node(const void *a, const void *b)
{
    return ib_compare_guid(((const node_key *)a)->guid, ((const node_key *)b)->guid);
}

static int cmp_link(const void *a, const void *b)
{
    const link_key *x = a, *y = b;
    int c = ib_compare_guid(x->guid, y->guid);
    return c ? c : (x->port > y->port) - (x->port < y->port);
}

static node_key *sorted_nodes(const ib_fabric *f)
{
    node_key *k = malloc((f->nnodes + 1) * sizeof *k);
    if (!k)
        ibt_die("out of memory");
    for (size_t i = 0; i < f->nnodes; i++)
        k[i] = (node_key){f->nodes[i].guid, (uint32_t)i};
    qsort(k, f->nnodes, sizeof *k, cmp_node);
    return k;
}

static link_key *sorted_links(const ib_fabric *f)
{
    link_key *k = malloc((f->nports + 1) * sizeof *k);
    if (!k)
        ibt_die("out of memory");
    for (size_t i = 0; i < f->nports; i++)
        k[i] = (link_key){f->nodes[f->ports[i].node].guid, f->ports[i].num, (uint32_t)i};
    qsort(k, f->nports, sizeof *k, cmp_link);
    return k;
}

static void node_id(char *buf, size_t n, const ib_node *nd)
{
    snprintf(buf, n, "%c-%016" PRIx64, kind_letter((ib_kind)nd->kind), nd->guid);
}

static void link_id(char *buf, size_t n, const ib_fabric *f, const ib_port *p)
{
    snprintf(buf, n, "%c-%016" PRIx64 "[%u]", kind_letter((ib_kind)f->nodes[p->node].kind),
             f->nodes[p->node].guid, p->num);
}

static void node_changed(sink *s, const ib_fabric *a, const ib_node *x, const ib_fabric *b, const ib_node *y)
{
    char id[24];
    node_id(id, sizeof id, y);
    if (strcmp(ib_str(a, x->desc), ib_str(b, y->desc)) != 0)
        emit(s, '~', "node", id, "desc '%s' -> '%s'", ib_str(a, x->desc), ib_str(b, y->desc));
    if (x->lid != y->lid)
        emit(s, '~', "node", id, "lid %u -> %u", x->lid, y->lid);
    if (x->kind != y->kind)
        emit(s, '~', "node", id, "type %s -> %s", ib_kind_name((ib_kind)x->kind), ib_kind_name((ib_kind)y->kind));
}

static void link_changed(sink *s, const ib_port *x, const ib_fabric *b, const ib_port *y)
{
    char id[32];
    link_id(id, sizeof id, b, y);
    if (x->peer_guid != y->peer_guid || x->peer_num != y->peer_num)
        emit(s, '~', "link", id, "peer %c-%016" PRIx64 "[%u] -> %c-%016" PRIx64 "[%u]", x->peer_kind, x->peer_guid,
             x->peer_num, y->peer_kind, y->peer_guid, y->peer_num);
    if (x->speed != y->speed || x->width != y->width)
        emit(s, '~', "link", id, "speed %ux%s -> %ux%s", x->width, ib_speed_name((ib_speed)x->speed), y->width,
             ib_speed_name((ib_speed)y->speed));
}

static void diff_nodes(const ib_fabric *a, const ib_fabric *b, sink *s)
{
    node_key *ka = sorted_nodes(a), *kb = sorted_nodes(b);
    size_t i = 0, j = 0;
    char id[24];

    while (i < a->nnodes || j < b->nnodes) {
        int c = i == a->nnodes ? 1 : j == b->nnodes ? -1 : ib_compare_guid(ka[i].guid, kb[j].guid);
        if (c < 0) {
            node_id(id, sizeof id, &a->nodes[ka[i++].idx]);
            emit(s, '-', "node", id, "");
        } else if (c > 0) {
            const ib_node *n = &b->nodes[kb[j++].idx];
            node_id(id, sizeof id, n);
            emit(s, '+', "node", id, "'%s'", ib_str(b, n->desc));
        } else {
            node_changed(s, a, &a->nodes[ka[i].idx], b, &b->nodes[kb[j].idx]);
            i++;
            j++;
        }
    }
    free(ka);
    free(kb);
}

static void diff_links(const ib_fabric *a, const ib_fabric *b, sink *s)
{
    link_key *ka = sorted_links(a), *kb = sorted_links(b);
    size_t i = 0, j = 0;
    char id[32];

    while (i < a->nports || j < b->nports) {
        int c = i == a->nports ? 1 : j == b->nports ? -1 : cmp_link(&ka[i], &kb[j]);
        if (c < 0) {
            link_id(id, sizeof id, a, &a->ports[ka[i++].idx]);
            emit(s, '-', "link", id, "");
        } else if (c > 0) {
            const ib_port *p = &b->ports[kb[j++].idx];
            link_id(id, sizeof id, b, p);
            emit(s, '+', "link", id, "-> %c-%016" PRIx64 "[%u]", p->peer_kind, p->peer_guid, p->peer_num);
        } else {
            link_changed(s, &a->ports[ka[i].idx], b, &b->ports[kb[j].idx]);
            i++;
            j++;
        }
    }
    free(ka);
    free(kb);
}

size_t ib_diff(const ib_fabric *a, const ib_fabric *b, FILE *out, bool json)
{
    sink s = {out, json, 0};
    diff_nodes(a, b, &s);
    diff_links(a, b, &s);
    return s.count;
}
