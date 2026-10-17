#include <inttypes.h>

#include "ibnetlens.h"

void ib_json_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(out, "\\%c", c);
        else if (c < 0x20)
            fprintf(out, "\\u%04x", c);
        else
            fputc(c, out);
    }
    fputc('"', out);
}

static void csv_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s; s++) {
        if (*s == '"')
            fputc('"', out);
        fputc(*s, out);
    }
    fputc('"', out);
}

static uint32_t node_lid(const ib_fabric *f, const ib_node *n)
{
    if (n->kind != IB_CA)
        return n->lid;
    return n->nports ? f->ports[n->first_port].lid : 0;
}

void ib_write_json(const ib_fabric *f, FILE *out)
{
    fputs("{\"nodes\":[", out);
    for (size_t i = 0; i < f->nnodes; i++) {
        const ib_node *n = &f->nodes[i];
        fprintf(out, "%s\n{\"guid\":\"0x%016" PRIx64 "\",\"type\":\"%s\",\"desc\":", i ? "," : "", n->guid,
                ib_kind_name((ib_kind)n->kind));
        ib_json_str(out, ib_str(f, n->desc));
        if (n->name) {
            fputs(",\"name\":", out);
            ib_json_str(out, ib_str(f, n->name));
        }
        fprintf(out, ",\"sysimgguid\":\"0x%016" PRIx64 "\",\"vendid\":%u,\"devid\":%u,\"lid\":%u,\"lmc\":%u",
                n->sysimg, n->vendid, n->devid, node_lid(f, n), n->lmc);
        if (n->kind == IB_SWITCH)
            fprintf(out, ",\"portguid\":\"0x%016" PRIx64 "\"", n->portguid);
        fputs(",\"ports\":[", out);
        for (uint32_t k = 0; k < n->nports; k++) {
            const ib_port *p = &f->ports[n->first_port + k];
            fprintf(out, "%s{\"num\":%u,\"peer\":\"0x%016" PRIx64 "\",\"peer_port\":%u,\"speed\":\"%s\","
                         "\"width\":%u,\"lid\":%u,\"peer_lid\":%u}",
                    k ? "," : "", p->num, p->peer_guid, p->peer_num, ib_speed_name((ib_speed)p->speed), p->width,
                    p->lid, p->peer_lid);
        }
        fputs("]}", out);
    }
    fputs("\n]}\n", out);
}

void ib_write_csv_nodes(const ib_fabric *f, FILE *out)
{
    fputs("guid,type,desc,sysimgguid,vendid,devid,lid,lmc,links\n", out);
    for (size_t i = 0; i < f->nnodes; i++) {
        const ib_node *n = &f->nodes[i];
        fprintf(out, "0x%016" PRIx64 ",%s,", n->guid, ib_kind_name((ib_kind)n->kind));
        csv_str(out, ib_str(f, n->desc));
        fprintf(out, ",0x%016" PRIx64 ",%u,%u,%u,%u,%u\n", n->sysimg, n->vendid, n->devid, node_lid(f, n),
                n->lmc, n->nports);
    }
}

void ib_write_csv_links(const ib_fabric *f, FILE *out)
{
    fputs("guid,port,peer_guid,peer_port,speed,width,lid,peer_lid\n", out);
    for (size_t i = 0; i < f->nports; i++) {
        const ib_port *p = &f->ports[i];
        fprintf(out, "0x%016" PRIx64 ",%u,0x%016" PRIx64 ",%u,%s,%u,%u,%u\n", f->nodes[p->node].guid, p->num,
                p->peer_guid, p->peer_num, ib_speed_name((ib_speed)p->speed), p->width, p->lid, p->peer_lid);
    }
}

static void dot_label(FILE *out, const char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', out);
        fputc(*s, out);
    }
}

void ib_write_dot(const ib_fabric *f, FILE *out)
{
    fputs("graph ibnetlens {\n  node [fontsize=10];\n", out);
    for (size_t i = 0; i < f->nnodes; i++) {
        const ib_node *n = &f->nodes[i];
        char letter = n->kind == IB_SWITCH ? 'S' : n->kind == IB_ROUTER ? 'R' : 'H';
        fprintf(out, "  \"%c-%016" PRIx64 "\" [shape=%s,label=\"", letter, n->guid,
                n->kind == IB_SWITCH ? "box" : "ellipse");
        dot_label(out, ib_str(f, n->desc));
        fputs("\"];\n", out);
    }
    for (size_t i = 0; i < f->nports; i++) {
        const ib_port *p = &f->ports[i];
        if (p->peer_idx != IBT_NONE && i > p->peer_idx)
            continue;
        const ib_node *n = &f->nodes[p->node];
        char a = n->kind == IB_SWITCH ? 'S' : n->kind == IB_ROUTER ? 'R' : 'H';
        fprintf(out, "  \"%c-%016" PRIx64 "\" -- \"%c-%016" PRIx64 "\" [taillabel=\"%u\",headlabel=\"%u\"];\n", a,
                n->guid, p->peer_kind, p->peer_guid, p->num, p->peer_num);
    }
    fputs("}\n", out);
}
