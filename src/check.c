#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ibnetlens.h"

#define MAX_RATES 64

typedef struct {
    uint64_t rate;
    size_t count;
} rate_bin;

typedef struct {
    FILE *out;
    bool json;
    size_t errors;
    size_t warnings;
} report;

typedef enum { SEV_WARN, SEV_ERROR } severity;

static char kind_letter(ib_kind k)
{
    return k == IB_SWITCH ? 'S' : k == IB_ROUTER ? 'R' : 'H';
}

static void emit(report *r, severity sev, const char *code, const ib_node *n, int port, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (sev == SEV_ERROR)
        r->errors++;
    else
        r->warnings++;

    const char *sname = sev == SEV_ERROR ? "error" : "warning";
    char id[24] = "";
    if (n)
        snprintf(id, sizeof id, "%c-%016" PRIx64, kind_letter((ib_kind)n->kind), n->guid);

    if (r->json) {
        fprintf(r->out, "{\"severity\":\"%s\",\"code\":\"%s\",\"node\":\"%s\",\"port\":%d,\"message\":", sname,
                code, id, port);
        ib_json_str(r->out, msg);
        fputs("}\n", r->out);
    } else if (port >= 0) {
        fprintf(r->out, "%s %s %s[%d]: %s\n", sname, code, id, port, msg);
    } else {
        fprintf(r->out, "%s %s %s: %s\n", sname, code, id, msg);
    }
}

static void bump(rate_bin *bins, size_t *nbins, uint64_t rate)
{
    for (size_t i = 0; i < *nbins; i++) {
        if (bins[i].rate == rate) {
            bins[i].count++;
            return;
        }
    }
    if (*nbins < MAX_RATES) {
        bins[*nbins].rate = rate;
        bins[*nbins].count = 1;
        (*nbins)++;
    }
}

static uint64_t modal_rate(const rate_bin *bins, size_t n)
{
    uint64_t best = 0;
    size_t best_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (bins[i].count > best_count || (bins[i].count == best_count && bins[i].rate > best)) {
            best = bins[i].rate;
            best_count = bins[i].count;
        }
    }
    return best;
}

/* 0: switch-switch, 1: switch-adapter, 2: anything else */
static int link_class(const ib_fabric *f, const ib_port *p)
{
    if (p->peer_node == IBT_NONE)
        return 2;
    ib_kind a = (ib_kind)f->nodes[p->node].kind, b = (ib_kind)f->nodes[p->peer_node].kind;
    if (a == IB_SWITCH && b == IB_SWITCH)
        return 0;
    if ((a == IB_SWITCH && b == IB_CA) || (a == IB_CA && b == IB_SWITCH))
        return 1;
    return 2;
}

static uint64_t link_rate(const ib_fabric *f, size_t i)
{
    const ib_port *p = &f->ports[i];
    uint64_t r = ib_rate_mbps((ib_speed)p->speed, p->width);
    if (p->peer_idx != IBT_NONE) {
        const ib_port *q = &f->ports[p->peer_idx];
        uint64_t rq = ib_rate_mbps((ib_speed)q->speed, q->width);
        if (rq < r)
            r = rq;
    }
    return r;
}

static bool counts_once(const ib_port *p, size_t i)
{
    return p->peer_idx == IBT_NONE || i < p->peer_idx;
}

static void check_dup_guids(const ib_fabric *f, report *r)
{
    for (size_t i = 0; i < f->ndups; i++) {
        const ib_node *n = &f->nodes[f->dups[i].dup];
        emit(r, SEV_ERROR, "duplicate-guid", n, -1, "GUID already used by '%s'",
             ib_str(f, f->nodes[f->dups[i].first].desc));
    }
}

static void check_links(const ib_fabric *f, report *r)
{
    for (size_t i = 0; i < f->nports; i++) {
        const ib_port *p = &f->ports[i];
        const ib_node *n = &f->nodes[p->node];

        if (p->peer_node == IBT_NONE) {
            if (p->peer_name)
                emit(r, SEV_WARN, "unknown-peer", n, p->num, "remote node '%s' is not in the file",
                     ib_str(f, p->peer_name));
            else
                emit(r, SEV_WARN, "unknown-peer", n, p->num, "remote node %c-%016" PRIx64 " is not in the file",
                     p->peer_kind, p->peer_guid);
            continue;
        }
        const ib_node *peer = &f->nodes[p->peer_node];
        if (kind_letter((ib_kind)peer->kind) != p->peer_kind)
            emit(r, SEV_ERROR, "kind-mismatch", n, p->num, "remote ID says '%c' but the node is a %s",
                 p->peer_kind, ib_kind_name((ib_kind)peer->kind));
        if (p->peer_idx == IBT_NONE) {
            emit(r, SEV_ERROR, "missing-reverse-link", n, p->num, "remote port %u of %c-%016" PRIx64 " is not listed",
                 p->peer_num, p->peer_kind, p->peer_guid);
            continue;
        }
        const ib_port *q = &f->ports[p->peer_idx];
        if (q->peer_guid != n->guid || q->peer_num != p->num)
            emit(r, SEV_ERROR, "asymmetric-link", n, p->num,
                 "remote port reports a different peer (%c-%016" PRIx64 "[%u])", q->peer_kind, q->peer_guid,
                 q->peer_num);
        else if (i < p->peer_idx && p->speed && q->speed && p->width && q->width &&
                 (p->speed != q->speed || p->width != q->width))
            emit(r, SEV_WARN, "speed-mismatch", n, p->num, "local %ux%s, remote %ux%s", p->width,
                 ib_speed_name((ib_speed)p->speed), q->width, ib_speed_name((ib_speed)q->speed));
    }
}

static void check_degraded(const ib_fabric *f, report *r)
{
    rate_bin bins[3][MAX_RATES];
    size_t nbins[3] = {0, 0, 0};
    uint64_t modal[3];

    for (size_t i = 0; i < f->nports; i++) {
        const ib_port *p = &f->ports[i];
        uint64_t rate = link_rate(f, i);
        if (counts_once(p, i) && rate)
            bump(bins[link_class(f, p)], &nbins[link_class(f, p)], rate);
    }
    for (int c = 0; c < 3; c++)
        modal[c] = modal_rate(bins[c], nbins[c]);

    for (size_t i = 0; i < f->nports; i++) {
        const ib_port *p = &f->ports[i];
        uint64_t rate = link_rate(f, i);
        int c = link_class(f, p);
        if (!counts_once(p, i) || !rate || rate >= modal[c])
            continue;
        emit(r, SEV_WARN, "degraded-link", &f->nodes[p->node], p->num,
             "link runs at %" PRIu64 " Gb/s, most links of this class run at %" PRIu64 " Gb/s", rate / 1000,
             modal[c] / 1000);
    }
}

static void check_duplicate_ports(const ib_fabric *f, report *r)
{
    uint8_t *seen = calloc(UINT16_MAX + 1u, 1);
    if (!seen)
        ibt_die("out of memory");
    for (size_t i = 0; i < f->nnodes; i++) {
        const ib_node *n = &f->nodes[i];
        for (uint32_t k = 0; k < n->nports; k++) {
            uint16_t num = f->ports[n->first_port + k].num;
            if (seen[num])
                emit(r, SEV_ERROR, "duplicate-port", n, num, "port is listed more than once");
            seen[num] = 1;
        }
        for (uint32_t k = 0; k < n->nports; k++)
            seen[f->ports[n->first_port + k].num] = 0;
    }
    free(seen);
}

typedef struct {
    uint64_t lid;
    uint32_t node;
} lid_entry;

static int cmp_lid(const void *a, const void *b)
{
    const lid_entry *x = a, *y = b;
    if (x->lid != y->lid)
        return x->lid < y->lid ? -1 : 1;
    return (x->node > y->node) - (x->node < y->node);
}

static void check_lids(const ib_fabric *f, report *r)
{
    lid_entry *e = malloc((f->nnodes + f->nports + 1) * sizeof *e);
    if (!e)
        ibt_die("out of memory");
    size_t n = 0;
    for (size_t i = 0; i < f->nnodes; i++) {
        const ib_node *nd = &f->nodes[i];
        if (nd->kind != IB_CA && nd->lid)
            e[n++] = (lid_entry){nd->lid, (uint32_t)i};
        for (uint32_t k = 0; nd->kind == IB_CA && k < nd->nports; k++) {
            uint32_t lid = f->ports[nd->first_port + k].lid;
            if (lid)
                e[n++] = (lid_entry){lid, (uint32_t)i};
        }
    }
    qsort(e, n, sizeof *e, cmp_lid);
    for (size_t i = 1; i < n; i++) {
        if (e[i].lid == e[i - 1].lid && e[i].node != e[i - 1].node)
            emit(r, SEV_ERROR, "duplicate-lid", &f->nodes[e[i].node], -1, "LID %" PRIu64 " is also used by %c-%016" PRIx64,
                 e[i].lid, kind_letter((ib_kind)f->nodes[e[i - 1].node].kind), f->nodes[e[i - 1].node].guid);
    }
    free(e);
}

size_t ib_check(const ib_fabric *f, FILE *out, bool json)
{
    report r = {out, json, 0, 0};

    check_dup_guids(f, &r);
    check_duplicate_ports(f, &r);
    check_links(f, &r);
    check_degraded(f, &r);
    check_lids(f, &r);

    if (!json)
        fprintf(out, "%zu nodes, %zu link entries: %zu errors, %zu warnings\n", f->nnodes, f->nports, r.errors,
                r.warnings);
    return r.errors + r.warnings;
}
