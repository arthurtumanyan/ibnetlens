#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "hex.h"
#include "ibnetlens.h"

#define MAX_REPORTED 50

typedef struct {
    const char *p;
    const char *end;
} cursor;

typedef struct {
    uint16_t vendid;
    uint16_t devid;
    uint64_t sysimg;
    uint64_t guid;
    uint64_t portguid;
    bool has_guid;
} pending;

typedef struct {
    size_t line;
    char msg[160];
} wmsg;

/* A switchguid=/caguid=/rtguid= line seen before node `node` (ibsim GUID base assignment). */
typedef struct {
    size_t node;
    uint8_t kind;
    uint64_t value;
} base_event;

/* A node with an ibsim-style ID, and the line it was defined on. */
typedef struct {
    size_t node;
    size_t line;
} named_line;

typedef struct {
    ib_fabric *f;
    const ib_parse_opts *opts;
    size_t line;
    wmsg *wl;               /* first MAX_REPORTED warnings, printed after all chunks finish */
    size_t nwl;
    bool defer_map;         /* parallel chunks register GUIDs after the merge */
    bool saw_named;
    bool named_style;       /* input uses ibsim-style IDs: GUIDs and names are settled after the merge */
    base_event *events;
    size_t nevents, events_cap;
    named_line *named;
    size_t nnamed, named_cap;
    uint32_t cur_node;
    pending pend;
    uint64_t next_guid[3];  /* allocators for nodes without GUIDs, indexed by ib_kind */
    bool base_set[3];
    uint64_t base[3];
} parser;

static void warn(parser *ps, const char *fmt, ...)
{
    ps->f->warnings++;
    if (ps->opts->quiet && !ps->opts->strict)
        return;
    if (ps->nwl >= MAX_REPORTED)
        return;
    wmsg *w = &ps->wl[ps->nwl++];
    w->line = ps->line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->msg, sizeof w->msg, fmt, ap);
    va_end(ap);
}

static bool at_end(const cursor *c)
{
    return c->p >= c->end;
}

static void skip_blank(cursor *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t'))
        c->p++;
}

static bool eat(cursor *c, char ch)
{
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return true;
    }
    return false;
}

static bool eat_lit(cursor *c, const char *lit, size_t n)
{
    if ((size_t)(c->end - c->p) >= n && memcmp(c->p, lit, n) == 0) {
        c->p += n;
        return true;
    }
    return false;
}

#define eat_str(c, lit) eat_lit((c), "" lit, sizeof(lit) - 1)

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* End of the whole input, bounds the 16-byte SIMD loads. Set only while a parse runs and cleared afterwards, so a
 * stale pointer from an earlier input can never admit a read past a later, smaller buffer. */
static _Thread_local const char *buf_limit;

static bool read_uint(cursor *c, int base, uint64_t *out)
{
    if (base == 16) {
        if (!eat_str(c, "0x"))
            eat_str(c, "0X");
        if (c->p + 17 <= buf_limit) {
            uint64_t v;
            size_t n = ibt_hex_prefix(c->p, &v);
            if (n >= 1 && n <= 16 && n <= (size_t)(c->end - c->p)) {
                c->p += n;
                *out = v;
                return true;
            }
            if (n == 0)
                return false;
        }
    }
    uint64_t v = 0;
    size_t digits = 0;
    while (c->p < c->end) {
        int d = hexval(*c->p);
        if (d < 0 || d >= base)
            break;
        if (base == 16 ? (v >> 60) != 0 : v > (UINT64_MAX - (uint64_t)d) / 10)
            return false;
        v = v * (uint64_t)base + (uint64_t)d;
        c->p++;
        digits++;
    }
    *out = v;
    return digits > 0;
}

static bool read_u32(cursor *c, int base, uint32_t *out)
{
    uint64_t v;
    if (!read_uint(c, base, &v) || v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

static bool read_quoted(cursor *c, ibt_slice *out)
{
    if (!eat(c, '"'))
        return false;
    const char *start = c->p;
    const char *q = memchr(start, '"', (size_t)(c->end - start));
    if (!q)
        return false;
    out->p = start;
    out->n = (size_t)(q - start);
    c->p = q + 1;
    return true;
}

static ibt_slice read_word(cursor *c)
{
    ibt_slice w = {c->p, 0};
    while (c->p < c->end && *c->p != ' ' && *c->p != '\t')
        c->p++;
    w.n = (size_t)(c->p - w.p);
    return w;
}

static bool slice_eq(ibt_slice s, const char *lit, size_t n)
{
    return s.n == n && memcmp(s.p, lit, n) == 0;
}

#define slice_is(s, lit) slice_eq((s), "" lit, sizeof(lit) - 1)

/* Node IDs look like "S-0002c903007b78b0": kind letter, dash, hex GUID. */
static bool parse_id(ibt_slice id, char *kind, uint64_t *guid)
{
    if (id.n < 3 || id.n > 18 || id.p[1] != '-')
        return false;
    if (id.p[0] != 'S' && id.p[0] != 'H' && id.p[0] != 'R')
        return false;
    cursor c = {id.p + 2, id.p + id.n};
    if (!read_uint(&c, 16, guid) || !at_end(&c))
        return false;
    *kind = id.p[0];
    return true;
}

static bool parse_paren_guid(cursor *c, uint64_t *out)
{
    if (!eat(c, '('))
        return true;
    return read_uint(c, 16, out) && eat(c, ')');
}

/* Parenthesised groups after a port number. Classic files carry a port GUID where there is one; ibdiagnet's
 * extended files add a port label in front of every port ("[1](1) ..." and "[1](1)(2c9...) ..."). Whether an
 * end of the link has a GUID follows from its node type, so a single group is read as a GUID or as a label
 * accordingly and the label is dropped. */
static bool read_port_groups(cursor *c, bool has_guid, uint64_t *guid)
{
    cursor g[2];
    int n = 0;
    while (c->p < c->end && *c->p == '(') {
        const char *close = memchr(c->p, ')', (size_t)(c->end - c->p));
        if (!close || n == 2)
            return false;
        g[n].p = c->p + 1;
        g[n].end = close;
        n++;
        c->p = close + 1;
    }
    int which = has_guid ? n - 1 : (n == 2 ? 1 : -1);
    if (which < 0)
        return true;
    return read_uint(&g[which], 16, guid) && at_end(&g[which]);
}

static bool parse_keyline(parser *ps, cursor c)
{
    const char *eq = memchr(c.p, '=', (size_t)(c.end - c.p));
    if (!eq)
        return false;
    ibt_slice key = {c.p, (size_t)(eq - c.p)};
    c.p = eq + 1;
    pending *pd = &ps->pend;
    uint64_t v;

    if (slice_is(key, "vendid")) {
        memset(pd, 0, sizeof *pd);
        ps->cur_node = IBT_NONE;
        if (!read_uint(&c, 16, &v) || v > UINT16_MAX)
            return false;
        pd->vendid = (uint16_t)v;
    } else if (slice_is(key, "devid")) {
        if (!read_uint(&c, 16, &v) || v > UINT16_MAX)
            return false;
        pd->devid = (uint16_t)v;
    } else if (slice_is(key, "sysimgguid")) {
        if (!read_uint(&c, 16, &pd->sysimg))
            return false;
    } else if (slice_is(key, "switchguid") || slice_is(key, "caguid") || slice_is(key, "rtguid") ||
               slice_is(key, "hcaguids")) {
        if (!read_uint(&c, 16, &pd->guid))
            return false;
        pd->has_guid = true;
        if (!parse_paren_guid(&c, &pd->portguid))
            return false;
        ib_kind k = slice_is(key, "switchguid") ? IB_SWITCH : slice_is(key, "rtguid") ? IB_ROUTER : IB_CA;
        ps->base[k] = pd->guid;
        ps->base_set[k] = true;
        if (ps->defer_map && ps->named_style) {
            ibt_grow((void **)&ps->events, &ps->events_cap, sizeof *ps->events, ps->nevents + 1);
            ps->events[ps->nevents++] = (base_event){ps->f->nnodes, (uint8_t)k, pd->guid};
        }
    } else if (slice_is(key, "width") || slice_is(key, "speed") || slice_is(key, "speedext") ||
               slice_is(key, "netprefix")) {
        /* ibsim simulation settings, no topology content */
    } else {
        return false;
    }
    return true;
}

static ib_speed parse_speed(ibt_slice w, uint8_t *width)
{
    size_t i = 0;
    unsigned wd = 0;
    while (i < w.n && w.p[i] >= '0' && w.p[i] <= '9' && wd < 1000)
        wd = wd * 10 + (unsigned)(w.p[i++] - '0');
    if (i == 0 || i >= w.n || w.p[i] != 'x')
        return IB_SPEED_UNKNOWN;
    *width = (uint8_t)(wd > 255 ? 255 : wd);
    return ib_speed_from(w.p + i + 1, w.n - i - 1);
}

static bool looks_like_speed(ibt_slice w)
{
    return w.n > 2 && w.p[0] >= '0' && w.p[0] <= '9' && memchr(w.p, 'x', w.n) != NULL;
}

static bool parse_header(parser *ps, cursor c, ib_kind kind)
{
    ib_fabric *f = ps->f;
    uint32_t declared;
    ibt_slice id, desc = {NULL, 0};
    char idkind;
    uint64_t guid;

    skip_blank(&c);
    if (!read_u32(&c, 10, &declared))
        return false;
    skip_blank(&c);
    if (!read_quoted(&c, &id))
        return false;
    bool named = !parse_id(id, &idkind, &guid);
    if (named) {
        if (id.n == 0)
            return false;
        ps->saw_named = true;
        if (ps->defer_map && ps->named_style) {
            guid = 0; /* assigned in finalize_named */
        } else {
            if (ps->base_set[kind]) {
                ps->next_guid[kind] = ps->base[kind];
                ps->base_set[kind] = false;
            }
            guid = ps->next_guid[kind];
            ps->next_guid[kind] += kind == IB_SWITCH ? 1 : (uint64_t)declared + 1;
        }
    }

    uint32_t idx = ib_node_add(f);
    ib_node *n = &f->nodes[idx];
    n->kind = (uint8_t)kind;
    n->guid = guid;
    if (named) {
        n->name = ib_intern(f, id);
        if (ps->defer_map && ps->named_style) {
            ibt_grow((void **)&ps->named, &ps->named_cap, sizeof *ps->named, ps->nnamed + 1);
            ps->named[ps->nnamed++] = (named_line){idx, ps->line};
        } else if (ib_name_add(f, idx) != IBT_NONE) {
            warn(ps, "node ID '%.*s' is defined twice", (int)id.n, id.p);
        }
    }
    n->declared_ports = declared;
    n->first_port = (uint32_t)f->nports;
    n->vendid = ps->pend.vendid;
    n->devid = ps->pend.devid;
    n->sysimg = ps->pend.sysimg;
    n->portguid = ps->pend.portguid;
    if (!named && ps->pend.has_guid && ps->pend.guid != guid)
        warn(ps, "node ID does not match the guid line of the same block");
    memset(&ps->pend, 0, sizeof ps->pend);
    ps->cur_node = idx;

    if (!ps->defer_map) {
        uint32_t prev = ibt_guidmap_put(&f->map, guid, idx);
        if (prev != IBT_NONE) {
            ibt_grow((void **)&f->dups, &f->dups_cap, sizeof *f->dups, f->ndups + 1);
            f->dups[f->ndups].first = prev;
            f->dups[f->ndups].dup = idx;
            f->ndups++;
        }
    }

    skip_blank(&c);
    if (!eat(&c, '#')) {
        f->nodes[idx].desc = f->nodes[idx].name;
        return true;
    }
    skip_blank(&c);
    if (!at_end(&c) && *c.p == '"') {
        if (!read_quoted(&c, &desc))
            return false;
        f->nodes[idx].desc = ib_intern(f, desc);
    }
    n = &f->nodes[idx];
    if (!n->desc)
        n->desc = n->name;
    for (;;) {
        skip_blank(&c);
        if (at_end(&c))
            break;
        ibt_slice w = read_word(&c);
        uint32_t v;
        if (slice_is(w, "enhanced")) {
            n->base_type = IB_BASE_ENHANCED;
        } else if (slice_is(w, "base")) {
            n->base_type = IB_BASE_PORT;
        } else if (slice_is(w, "port") || slice_is(w, "lid") || slice_is(w, "lmc")) {
            skip_blank(&c);
            if (!read_u32(&c, 10, &v))
                return false;
            if (slice_is(w, "port"))
                n->base_port = v;
            else if (slice_is(w, "lid"))
                n->lid = v;
            else
                n->lmc = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    return true;
}

static bool parse_port(parser *ps, cursor c)
{
    ib_fabric *f = ps->f;
    uint32_t num, peer_num;
    ibt_slice id;
    char idkind;
    uint64_t peer_guid = 0, portguid = 0, peer_portguid = 0;
    uint32_t peer_name = 0;

    if (ps->cur_node == IBT_NONE) {
        warn(ps, "link line outside of a node block");
        return true;
    }
    if (!eat(&c, '[') || !read_u32(&c, 10, &num) || num > UINT16_MAX || !eat(&c, ']'))
        return false;
    if (!read_port_groups(&c, f->nodes[ps->cur_node].kind != IB_SWITCH, &portguid))
        return false;
    skip_blank(&c);
    if (!read_quoted(&c, &id))
        return false;
    bool named = !parse_id(id, &idkind, &peer_guid);
    if (named) {
        if (id.n == 0)
            return false;
        idkind = '?';
        peer_name = ib_intern(f, id);
    }
    skip_blank(&c);
    if (named && at_end(&c)) {
        peer_num = 1;
    } else if (named && (*c.p == '#' || *c.p == 'w' || *c.p == 's' || *c.p == 'e')) {
        peer_num = 1;
    } else if (!eat(&c, '[') || !read_u32(&c, 10, &peer_num) || peer_num > UINT16_MAX || !eat(&c, ']')) {
        return false;
    }
    if (!read_port_groups(&c, idkind != 'S', &peer_portguid))
        return false;

    uint32_t pi = ib_port_add(f);
    ib_port *p = &f->ports[pi];
    p->node = ps->cur_node;
    p->num = (uint16_t)num;
    p->peer_num = (uint16_t)peer_num;
    p->peer_guid = peer_guid;
    p->peer_kind = idkind;
    p->peer_name = peer_name;
    p->portguid = portguid;
    p->peer_portguid = peer_portguid;
    f->nodes[ps->cur_node].nports++;

    for (;;) { /* ibsim per-port options such as w=4 */
        skip_blank(&c);
        if (at_end(&c) || *c.p == '#')
            break;
        ibt_slice opt = read_word(&c);
        if (opt.n > 2 && opt.p[0] == 'w' && opt.p[1] == '=') {
            cursor oc = {opt.p + 2, opt.p + opt.n};
            uint32_t w;
            if (read_u32(&oc, 10, &w) && w <= 255)
                p->width = (uint8_t)w;
        }
    }
    if (!eat(&c, '#'))
        return true;
    bool after_desc = false;
    for (;;) {
        skip_blank(&c);
        if (at_end(&c))
            break;
        if (*c.p == '"') {
            ibt_slice skip;
            if (!read_quoted(&c, &skip))
                return false;
            after_desc = true;
            continue;
        }
        ibt_slice w = read_word(&c);
        uint32_t v;
        if (slice_is(w, "lid") || slice_is(w, "lmc")) {
            skip_blank(&c);
            if (!read_u32(&c, 10, &v))
                return false;
            if (slice_is(w, "lid")) {
                if (after_desc)
                    p->peer_lid = v;
                else
                    p->lid = v;
            } else if (!after_desc) {
                p->lmc = (uint8_t)(v > 255 ? 255 : v);
            }
        } else if (looks_like_speed(w)) {
            p->speed = (uint8_t)parse_speed(w, &p->width);
            if (p->speed == IB_SPEED_UNKNOWN)
                warn(ps, "unknown link speed '%.*s'", (int)w.n, w.p);
        }
    }
    return true;
}

static bool parse_line(parser *ps, cursor c)
{
    skip_blank(&c);
    if (at_end(&c) || *c.p == '#')
        return true;
    if (*c.p == '[')
        return parse_port(ps, c);
    if (eat_str(&c, "Switch") && !at_end(&c) && (*c.p == ' ' || *c.p == '\t'))
        return parse_header(ps, c, IB_SWITCH);
    cursor k = c;
    if ((eat_str(&k, "Ca") || eat_str(&k, "Hca")) && !at_end(&k) && (*k.p == ' ' || *k.p == '\t'))
        return parse_header(ps, k, IB_CA);
    k = c;
    if (eat_str(&k, "Rt") && !at_end(&k) && (*k.p == ' ' || *k.p == '\t'))
        return parse_header(ps, k, IB_ROUTER);
    return parse_keyline(ps, c);
}

#define MIN_CHUNK_BYTES (2u << 20)
#define PROGRESS_STEP_BYTES (256u << 10)
#define PROGRESS_TTY_DELAY_MS 200
#define PROGRESS_TICK_MS 100
#define BAR_WIDTH 30
#define RETRY_SERIAL (-2)

typedef struct {
    ib_fabric own;
    ib_fabric *target;
    const char *begin;
    const char *end;
    const char *limit;
    const ib_parse_opts *opts;
    parser ps;
    wmsg wl[MAX_REPORTED];
    int rc;
    bool defer_map;
    bool named_style;
    size_t node_base;
    size_t port_base;
    uint32_t pool_shift;
    _Atomic size_t bytes_done; /* read by the progress thread */
    _Atomic size_t lines_done;
    _Atomic size_t nodes_done;
} chunk;

static void parse_range_body(chunk *ch)
{
    ch->ps = (parser){.f = ch->target,
                      .opts = ch->opts,
                      .wl = ch->wl,
                      .defer_map = ch->defer_map,
                      .named_style = ch->named_style,
                      .cur_node = IBT_NONE,
                      .next_guid = {0x100000, 0x200000, 0x300000}};
    parser *ps = &ch->ps;
    const char *p = ch->begin;
    const char *end = ch->end;
    ch->rc = 0;
    size_t next_report = PROGRESS_STEP_BYTES;

    while (p < end) {
        if ((size_t)(p - ch->begin) >= next_report) {
            atomic_store_explicit(&ch->bytes_done, (size_t)(p - ch->begin), memory_order_relaxed);
            atomic_store_explicit(&ch->lines_done, ps->line, memory_order_relaxed);
            atomic_store_explicit(&ch->nodes_done, ps->f->nnodes, memory_order_relaxed);
            next_report += PROGRESS_STEP_BYTES;
        }
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *next = nl ? nl + 1 : end;
        ps->line++;
        while (le > p && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t'))
            le--;
        if (le > p) {
            cursor c = {p, le};
            if (!parse_line(ps, c)) {
                warn(ps, "malformed line");
                if (ch->opts->strict) {
                    ch->rc = -1;
                    return;
                }
            }
        }
        p = next;
    }
    atomic_store_explicit(&ch->bytes_done, (size_t)(end - ch->begin), memory_order_relaxed);
    atomic_store_explicit(&ch->lines_done, ps->line, memory_order_relaxed);
    atomic_store_explicit(&ch->nodes_done, ps->f->nnodes, memory_order_relaxed);
}

static void parse_range(chunk *ch)
{
    buf_limit = ch->limit;
    parse_range_body(ch);
    buf_limit = NULL;
}

static void parse_task(void *arg, unsigned k)
{
    parse_range((chunk *)arg + k);
}

static bool is_node_header(const char *p, const char *end)
{
    static const char *const words[] = {"Switch", "Hca", "Ca", "Rt"};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        size_t n = strlen(words[i]);
        if ((size_t)(end - p) > n && memcmp(p, words[i], n) == 0 && (p[n] == ' ' || p[n] == '\t'))
            return true;
    }
    return false;
}

/* True when the first node header of the input carries an ibsim-style ID instead of S-/H-/R-<guid>. */
static bool detect_named_style(const char *buf, size_t len)
{
    const char *end = buf + (len < (1u << 20) ? len : (1u << 20));
    const char *p = buf;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        if (is_node_header(p, le)) {
            cursor c = {p, le};
            ibt_slice id;
            char kind;
            uint64_t guid;
            while (c.p < c.end && *c.p != '"')
                c.p++;
            return read_quoted(&c, &id) && !parse_id(id, &kind, &guid);
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return false;
}

/* Lines that set per-node state (vendid, GUID bases): a cut must not separate them from their header. */
static bool is_state_line(const char *s, const char *e)
{
    static const char *const keys[] = {"vendid=", "devid=", "sysimgguid=", "switchguid=", "caguid=", "rtguid=",
                                       "hcaguids="};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        size_t n = strlen(keys[i]);
        if ((size_t)(e - s) >= n && memcmp(s, keys[i], n) == 0)
            return true;
    }
    return false;
}

/* Moves a cut at node header `h` up over the state lines (and blank or comment lines between them) above it. */
static const char *include_state_lines(const char *lo, const char *h)
{
    const char *cut = h;
    const char *q = h;
    while (q > lo) {
        const char *s = q - 1;
        while (s > lo && s[-1] != '\n')
            s--;
        const char *e = q - 1; /* the newline that ends this line */
        while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            e--;
        if (e > s && is_state_line(s, e))
            cut = s;
        else if (e > s && *s != '#')
            break;
        q = s;
    }
    return cut;
}

/* Start of the first block after p: a "vendid=" line, or for ibsim-style input a node header. Node blocks
 * never span such a line. `lo` is the start of the chunk being cut. */
static const char *find_boundary(const char *p, const char *lo, const char *end, bool named_style)
{
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    while (nl && nl + 1 < end) {
        p = nl + 1;
        if (!named_style && (size_t)(end - p) >= 7 && memcmp(p, "vendid=", 7) == 0)
            return p;
        if (named_style && is_node_header(p, end))
            return include_state_lines(lo, p);
        nl = memchr(p, '\n', (size_t)(end - p));
    }
    return NULL;
}

typedef struct {
    size_t line;
    size_t order;
    const char *msg;
} shown_msg;

static int cmp_shown(const void *a, const void *b)
{
    const shown_msg *x = a, *y = b;
    if (x->line != y->line)
        return x->line < y->line ? -1 : 1;
    return (x->order > y->order) - (x->order < y->order);
}

/* Prints the first MAX_REPORTED warnings in file order. `extra` holds warnings found after the merge, with
 * global line numbers. */
static void print_warnings(const chunk *chunks, size_t n, const ib_parse_opts *opts, size_t total,
                           const wmsg *extra, size_t nextra)
{
    size_t cap = nextra, count = 0;
    for (size_t k = 0; k < n; k++)
        cap += chunks[k].ps.nwl;
    shown_msg *all = malloc((cap ? cap : 1) * sizeof *all);
    if (!all)
        ibt_die("out of memory");
    size_t base = 0;
    for (size_t k = 0; k < n; k++) {
        for (size_t i = 0; i < chunks[k].ps.nwl; i++, count++)
            all[count] = (shown_msg){base + chunks[k].wl[i].line, count, chunks[k].wl[i].msg};
        base += chunks[k].ps.line;
    }
    for (size_t i = 0; i < nextra; i++, count++)
        all[count] = (shown_msg){extra[i].line, count, extra[i].msg};
    qsort(all, count, sizeof *all, cmp_shown);

    size_t shown = count < MAX_REPORTED ? count : MAX_REPORTED;
    for (size_t i = 0; i < shown; i++)
        fprintf(stderr, "ibnetlens: %s:%zu: %s\n", opts->name, all[i].line, all[i].msg);
    if (total > shown && !opts->quiet)
        fprintf(stderr, "ibnetlens: %s: %zu more warnings suppressed\n", opts->name, total - shown);
    free(all);
}

typedef struct {
    ib_fabric *f;
    chunk *chunks;
} merge_ctx;

/* Copies one chunk into its final position and rebases the indices it holds. */
static void merge_task(void *arg, unsigned k)
{
    merge_ctx *m = arg;
    ib_fabric *f = m->f;
    chunk *ch = &m->chunks[k + 1]; /* chunk 0 already lives in place */
    ib_fabric *c = &ch->own;

    memcpy(f->nodes + ch->node_base, c->nodes, c->nnodes * sizeof *c->nodes);
    for (size_t i = ch->node_base; i < ch->node_base + c->nnodes; i++) {
        ib_node *n = &f->nodes[i];
        n->first_port += (uint32_t)ch->port_base;
        if (n->desc)
            n->desc += ch->pool_shift;
        if (n->name)
            n->name += ch->pool_shift;
    }
    memcpy(f->ports + ch->port_base, c->ports, c->nports * sizeof *c->ports);
    for (size_t i = ch->port_base; i < ch->port_base + c->nports; i++) {
        f->ports[i].node += (uint32_t)ch->node_base;
        if (f->ports[i].peer_name)
            f->ports[i].peer_name += ch->pool_shift;
    }
    free(c->nodes);
    free(c->ports);
    c->nodes = NULL;
    c->ports = NULL;
}

static void merge_chunks(ib_fabric *f, chunk *chunks, size_t n)
{
    size_t nodes = f->nnodes, ports = f->nports, pool = f->pool_len;
    for (size_t k = 1; k < n; k++) {
        ib_fabric *c = &chunks[k].own;
        chunks[k].node_base = nodes;
        chunks[k].port_base = ports;
        chunks[k].pool_shift = (uint32_t)pool - 1; /* offset 0 is the shared empty string */
        nodes += c->nnodes;
        ports += c->nports;
        pool += c->pool_len - 1;
        f->warnings += c->warnings;
    }
    if (pool > UINT32_MAX || nodes >= IBT_NONE - 1 || ports >= IBT_NONE - 1)
        ibt_die("input too large");

    ibt_grow((void **)&f->nodes, &f->nodes_cap, sizeof *f->nodes, nodes);
    ibt_grow((void **)&f->ports, &f->ports_cap, sizeof *f->ports, ports);
    ibt_grow((void **)&f->pool, &f->pool_cap, 1, pool);
    for (size_t k = 1; k < n; k++) {
        ib_fabric *c = &chunks[k].own;
        memcpy(f->pool + f->pool_len, c->pool + 1, c->pool_len - 1);
        f->pool_len += c->pool_len - 1;
    }

    merge_ctx m = {f, chunks};
    ibt_parallel_for((unsigned)n - 1, merge_task, &m);
    f->nnodes = nodes;
    f->nports = ports;
    for (size_t k = 1; k < n; k++)
        ib_fabric_free(&chunks[k].own);
}

static void register_guids(ib_fabric *f)
{
    ibt_guidmap_reserve(&f->map, f->nnodes);
    for (size_t i = 0; i < f->nnodes; i++) {
        uint32_t prev = ibt_guidmap_put(&f->map, f->nodes[i].guid, (uint32_t)i);
        if (prev == IBT_NONE)
            continue;
        ibt_grow((void **)&f->dups, &f->dups_cap, sizeof *f->dups, f->ndups + 1);
        f->dups[f->ndups].first = prev;
        f->dups[f->ndups].dup = (uint32_t)i;
        f->ndups++;
    }
}

typedef struct {
    chunk *chunks;
    size_t n;
    size_t total_bytes;
    const char *name;
    _Atomic bool done;
} progress_ctx;

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void progress_totals(const progress_ctx *pc, size_t *bytes, size_t *lines, size_t *nodes)
{
    *bytes = *lines = *nodes = 0;
    for (size_t k = 0; k < pc->n; k++) {
        *bytes += atomic_load_explicit(&pc->chunks[k].bytes_done, memory_order_relaxed);
        *lines += atomic_load_explicit(&pc->chunks[k].lines_done, memory_order_relaxed);
        *nodes += atomic_load_explicit(&pc->chunks[k].nodes_done, memory_order_relaxed);
    }
}

static void draw_bar(const progress_ctx *pc, size_t bytes, size_t lines, size_t nodes)
{
    int pct = pc->total_bytes ? (int)(bytes * 100 / pc->total_bytes) : 100;
    int filled = pct * BAR_WIDTH / 100;
    char bar[BAR_WIDTH + 1];
    for (int i = 0; i < BAR_WIDTH; i++)
        bar[i] = i < filled ? '=' : (i == filled ? '>' : ' ');
    bar[BAR_WIDTH] = '\0';
    fprintf(stderr, "\rparsing %3d%% [%s] %.1f/%.1f MB  %zu lines  %zu nodes", pct, bar, (double)bytes / 1048576.0,
            (double)pc->total_bytes / 1048576.0, lines, nodes);
    fflush(stderr);
}

/* On a terminal: a bar that appears after a short delay and is erased at the end.
 * Otherwise (forced with --progress): one plain line per 10% and a final line. */
static void *progress_main(void *arg)
{
    progress_ctx *pc = arg;
    bool tty = isatty(STDERR_FILENO);
    long waited = 0;
    bool drawn = false;
    int next_pct = 10;
    size_t bytes, lines, nodes;

    while (!atomic_load(&pc->done)) {
        sleep_ms(PROGRESS_TICK_MS);
        waited += PROGRESS_TICK_MS;
        progress_totals(pc, &bytes, &lines, &nodes);
        if (tty) {
            if (waited >= PROGRESS_TTY_DELAY_MS && !atomic_load(&pc->done)) {
                draw_bar(pc, bytes, lines, nodes);
                drawn = true;
            }
        } else {
            int pct = pc->total_bytes ? (int)(bytes * 100 / pc->total_bytes) : 100;
            int step = pct / 10 * 10;
            if (step >= next_pct && step < 100) {
                fprintf(stderr, "ibnetlens: %s: parsing %d%%\n", pc->name, step);
                next_pct = step + 10;
            }
        }
    }
    progress_totals(pc, &bytes, &lines, &nodes);
    if (tty && drawn)
        fputs("\r\033[K", stderr);
    else if (!tty)
        fprintf(stderr, "ibnetlens: %s: parsing 100%% (%zu lines, %zu nodes)\n", pc->name, lines, nodes);
    fflush(stderr);
    return NULL;
}

/* Settles what the chunks could not know alone for ibsim-style input: GUIDs follow file order (per-kind
 * counters, moved by switchguid=/caguid=/rtguid= lines) and node IDs must be unique. */
static void finalize_named(ib_fabric *f, chunk *chunks, size_t n, wmsg **extra, size_t *nextra)
{
    uint64_t next[3] = {0x100000, 0x200000, 0x300000};
    uint64_t pending_value[3] = {0, 0, 0};
    bool base_pending[3] = {false, false, false};
    size_t ek = 0, ei = 0;

    for (size_t i = 0; i < f->nnodes; i++) {
        for (;;) {
            while (ek < n && ei >= chunks[ek].ps.nevents) {
                ek++;
                ei = 0;
            }
            if (ek >= n)
                break;
            const base_event *e = &chunks[ek].ps.events[ei];
            if (chunks[ek].node_base + e->node > i)
                break;
            base_pending[e->kind] = true;
            pending_value[e->kind] = e->value;
            ei++;
        }
        ib_node *nd = &f->nodes[i];
        if (!nd->name)
            continue;
        if (base_pending[nd->kind]) {
            next[nd->kind] = pending_value[nd->kind];
            base_pending[nd->kind] = false;
        }
        nd->guid = next[nd->kind];
        next[nd->kind] += nd->kind == IB_SWITCH ? 1 : (uint64_t)nd->declared_ports + 1;
    }

    size_t line_base = 0, cap = 0;
    for (size_t k = 0; k < n; k++) {
        for (size_t i = 0; i < chunks[k].ps.nnamed; i++) {
            size_t node = chunks[k].node_base + chunks[k].ps.named[i].node;
            if (ib_name_add(f, (uint32_t)node) == IBT_NONE)
                continue;
            ibt_grow((void **)extra, &cap, sizeof **extra, *nextra + 1);
            wmsg *w = &(*extra)[(*nextra)++];
            w->line = line_base + chunks[k].ps.named[i].line;
            snprintf(w->msg, sizeof w->msg, "node ID '%.100s' is defined twice", ib_str(f, f->nodes[node].name));
        }
        line_base += chunks[k].ps.line;
    }
}

static int parse_chunks(ib_fabric *f, const char *buf, size_t len, const ib_parse_opts *opts, unsigned nt)
{
    const char *end = buf + len;
    chunk *chunks = calloc(nt, sizeof *chunks);
    if (!chunks)
        ibt_die("out of memory");

    bool named_style = nt > 1 && detect_named_style(buf, len);
    size_t n = 0;
    const char *begin = buf;
    for (unsigned k = 1; k <= nt; k++) {
        const char *cut = end;
        if (k < nt) {
            cut = find_boundary(buf + len / nt * k, begin, end, named_style);
            if (!cut || cut <= begin)
                continue;
        }
        chunks[n].begin = begin;
        chunks[n].end = cut;
        begin = cut;
        n++;
    }
    for (size_t k = 0; k < n; k++) {
        chunk *ch = &chunks[k];
        ch->limit = end;
        ch->opts = opts;
        ch->defer_map = n > 1;
        ch->named_style = named_style;
        if (k == 0) {
            ch->target = f;
        } else {
            ib_fabric_init(&ch->own);
            ch->target = &ch->own;
        }
        ib_fabric_reserve(ch->target, (size_t)(ch->end - ch->begin));
    }
    progress_ctx pc = {chunks, n, len, opts->name, false};
    pthread_t progress_th;
    bool progress_on = opts->progress && pthread_create(&progress_th, NULL, progress_main, &pc) == 0;
    ibt_parallel_for((unsigned)n, parse_task, chunks);
    if (progress_on) {
        atomic_store(&pc.done, true);
        pthread_join(progress_th, NULL);
    }

    int rc = 0;
    size_t used = n, lines = 0, warnings = f->warnings;
    bool named = false;
    for (size_t k = 0; k < n; k++) {
        named = named || (chunks[k].ps.saw_named && !named_style);
        if (chunks[k].rc != 0 && used == n) {
            rc = -1;
            used = k + 1;
        }
    }
    if (n > 1 && named) {
        for (size_t k = 0; k < n; k++) {
            free(chunks[k].ps.events);
            free(chunks[k].ps.named);
            if (k > 0)
                ib_fabric_free(&chunks[k].own);
        }
        free(chunks);
        return RETRY_SERIAL;
    }
    for (size_t k = 0; k < used; k++) {
        lines += chunks[k].ps.line;
        if (k > 0)
            warnings += chunks[k].own.warnings;
    }

    wmsg *extra = NULL;
    size_t nextra = 0;
    if (rc == 0 && n > 1) {
        merge_chunks(f, chunks, n);
        if (named_style)
            finalize_named(f, chunks, n, &extra, &nextra);
        register_guids(f);
        f->warnings += nextra;
    } else {
        for (size_t k = 1; k < n; k++)
            ib_fabric_free(&chunks[k].own);
    }
    print_warnings(chunks, used, opts, warnings + nextra, extra, nextra);
    free(extra);
    for (size_t k = 0; k < n; k++) {
        free(chunks[k].ps.events);
        free(chunks[k].ps.named);
    }
    f->lines = lines;
    f->threads_used = (unsigned)n;
    free(chunks);
    return rc;
}

static int parse_input(ib_fabric *f, const char *buf, size_t len, const ib_parse_opts *opts)
{
    ibt_hex_init();
    unsigned nt = ibt_pick_threads(opts->threads, len, MIN_CHUNK_BYTES);
    if (nt > 1) {
        int rc = parse_chunks(f, buf, len, opts, nt);
        if (rc != RETRY_SERIAL)
            return rc;
        ib_fabric_free(f);
        ib_fabric_init(f);
    }
    return parse_chunks(f, buf, len, opts, 1);
}

int ib_parse(ib_fabric *f, const char *buf, size_t len, const ib_parse_opts *opts)
{
    buf_limit = buf + len; /* detect_named_style reads on this thread before any chunk starts */
    int rc = parse_input(f, buf, len, opts);
    buf_limit = NULL;
    return rc;
}
