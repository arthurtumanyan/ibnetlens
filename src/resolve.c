#include "ibnetlens.h"

static uint32_t find_port(const ib_fabric *f, uint32_t node, uint16_t num)
{
    const ib_node *n = &f->nodes[node];
    for (uint32_t i = 0; i < n->nports; i++) {
        if (f->ports[n->first_port + i].num == num)
            return n->first_port + i;
    }
    return IBT_NONE;
}

#define LOOKAHEAD 12

static void resolve_nodes(ib_fabric *f, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; i++) {
        if (i + LOOKAHEAD < end && !f->ports[i + LOOKAHEAD].peer_name)
            ibt_guidmap_prefetch(&f->map, f->ports[i + LOOKAHEAD].peer_guid);
        ib_port *p = &f->ports[i];
        if (p->peer_name) {
            p->peer_node = ib_name_find(f, ib_str(f, p->peer_name));
            if (p->peer_node != IBT_NONE) {
                const ib_node *peer = &f->nodes[p->peer_node];
                p->peer_guid = peer->guid;
                p->peer_kind = peer->kind == IB_SWITCH ? 'S' : peer->kind == IB_ROUTER ? 'R' : 'H';
            }
        } else {
            p->peer_node = ibt_guidmap_get(&f->map, p->peer_guid);
        }
    }
}

static void resolve_ports(ib_fabric *f, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; i++) {
        if (i + LOOKAHEAD < end) {
            uint32_t pn = f->ports[i + LOOKAHEAD].peer_node;
            if (pn != IBT_NONE) {
                __builtin_prefetch(&f->nodes[pn]);
                __builtin_prefetch(&f->ports[f->nodes[pn].first_port]);
            }
        }
        ib_port *p = &f->ports[i];
        p->peer_idx = p->peer_node == IBT_NONE ? IBT_NONE : find_port(f, p->peer_node, p->peer_num);
    }
}

typedef struct {
    ib_fabric *f;
    unsigned nt;
    bool ports_pass;
} pass_ctx;

static void pass_slice(void *arg, unsigned k)
{
    pass_ctx *c = arg;
    size_t n = c->f->nports;
    size_t begin = n / c->nt * k;
    size_t end = k + 1 == c->nt ? n : n / c->nt * (k + 1);
    if (c->ports_pass)
        resolve_ports(c->f, begin, end);
    else
        resolve_nodes(c->f, begin, end);
}

#define MIN_PORTS_PER_THREAD 40000

void ib_resolve(ib_fabric *f, unsigned threads)
{
    unsigned nt = ibt_pick_threads(threads, f->nports, MIN_PORTS_PER_THREAD);
    if (nt < 2 || f->nports < nt) {
        resolve_nodes(f, 0, f->nports);
        resolve_ports(f, 0, f->nports);
        return;
    }
    /* the second pass reads peer_node values written by every slice of the first */
    pass_ctx c = {f, nt, false};
    ibt_parallel_for(nt, pass_slice, &c);
    c.ports_pass = true;
    ibt_parallel_for(nt, pass_slice, &c);
}
