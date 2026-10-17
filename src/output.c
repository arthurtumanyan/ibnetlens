#include <string.h>

#include "ibnetlens.h"

static const char *const format_names[IB_FMT_COUNT] = {
    "json", "csv", "csv-nodes", "dot", "slurm", "ibnd", "legacy",
};

int ib_format_from(const char *name, ib_format *out)
{
    for (int i = 0; i < IB_FMT_COUNT; i++) {
        if (strcmp(name, format_names[i]) == 0) {
            *out = (ib_format)i;
            return 0;
        }
    }
    return -1;
}

void ib_write(const ib_fabric *f, ib_format fmt, FILE *out, const ib_write_opts *wo)
{
    switch (fmt) {
    case IB_FMT_JSON: ib_write_json(f, out); break;
    case IB_FMT_CSV: ib_write_csv_links(f, out); break;
    case IB_FMT_CSV_NODES: ib_write_csv_nodes(f, out); break;
    case IB_FMT_DOT: ib_write_dot(f, out); break;
    case IB_FMT_SLURM: ib_write_slurm(f, out, wo ? wo->exclude : NULL); break;
    case IB_FMT_IBND: ib_write_ibnd(f, out); break;
    case IB_FMT_LEGACY: ib_write_legacy(f, out); break;
    default: break;
    }
}

void ib_write_info(const ib_fabric *f, FILE *out)
{
    size_t kinds[3] = {0, 0, 0};
    size_t speeds[IB_SPEED_COUNT] = {0};
    size_t unresolved = 0;

    for (size_t i = 0; i < f->nnodes; i++)
        kinds[f->nodes[i].kind]++;
    for (size_t i = 0; i < f->nports; i++) {
        speeds[f->ports[i].speed]++;
        if (f->ports[i].peer_node == IBT_NONE)
            unresolved++;
    }
    fprintf(out, "lines:        %zu\n", f->lines);
    fprintf(out, "switches:     %zu\n", kinds[IB_SWITCH]);
    fprintf(out, "adapters:     %zu\n", kinds[IB_CA]);
    fprintf(out, "routers:      %zu\n", kinds[IB_ROUTER]);
    fprintf(out, "port entries: %zu (%zu with unknown peer)\n", f->nports, unresolved);
    for (int s = 0; s < IB_SPEED_COUNT; s++) {
        if (speeds[s])
            fprintf(out, "  %-5s       %zu\n", ib_speed_name((ib_speed)s), speeds[s]);
    }
    fprintf(out, "warnings:     %zu\n", f->warnings);
}
