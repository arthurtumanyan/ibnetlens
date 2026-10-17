#ifndef IBNETLENS_H
#define IBNETLENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "guidmap.h"
#include "util.h"

#define IBNETLENS_VERSION "1.0.1"

typedef enum { IB_CA = 0, IB_SWITCH = 1, IB_ROUTER = 2 } ib_kind;

typedef enum {
    IB_SPEED_UNKNOWN = 0,
    IB_SDR, IB_DDR, IB_QDR, IB_FDR10, IB_FDR, IB_EDR, IB_HDR, IB_NDR, IB_XDR,
    IB_SPEED_COUNT
} ib_speed;

typedef enum { IB_BASE_NONE = 0, IB_BASE_PORT = 1, IB_BASE_ENHANCED = 2 } ib_base_type;

typedef struct {
    uint64_t guid;
    uint64_t sysimg;
    uint64_t portguid;   /* switch port 0 GUID, 0 when absent */
    uint32_t desc;       /* offset into the string pool */
    uint32_t name;       /* ibsim-style node ID, 0 for ibnetdiscover IDs */
    uint32_t first_port; /* connected ports are contiguous in fabric.ports */
    uint32_t nports;
    uint32_t declared_ports;
    uint32_t lid;        /* switches and routers; CA LIDs live on ports */
    uint32_t base_port;
    uint16_t vendid;
    uint16_t devid;
    uint8_t kind;
    uint8_t lmc;
    uint8_t base_type;
} ib_node;

typedef struct {
    uint64_t portguid;      /* local port GUID, present on CA lines */
    uint64_t peer_guid;     /* node GUID of the remote end */
    uint64_t peer_portguid; /* remote port GUID, present on switch lines */
    uint32_t peer_name;     /* ibsim-style remote node ID, 0 when GUID-addressed */
    uint32_t node;
    uint32_t peer_node;     /* IBT_NONE when the remote node is not in the file */
    uint32_t peer_idx;      /* index of the reverse port, IBT_NONE when missing */
    uint32_t lid;           /* local LID, CA lines only */
    uint32_t peer_lid;
    uint16_t num;
    uint16_t peer_num;
    uint8_t lmc;
    uint8_t width;
    uint8_t speed;
    char peer_kind;         /* 'S', 'H' or 'R' as written in the remote ID */
} ib_port;

typedef struct {
    uint32_t first; /* node index already in the map */
    uint32_t dup;   /* node index that reused the GUID */
} ib_dup;

typedef struct {
    ib_node *nodes;
    size_t nnodes, nodes_cap;
    ib_port *ports;
    size_t nports, ports_cap;
    char *pool;
    size_t pool_len, pool_cap;
    ib_dup *dups;
    size_t ndups, dups_cap;
    ibt_guidmap map;
    uint32_t *names;     /* open-addressing table of node indices keyed by name */
    size_t names_cap;
    size_t names_n;
    size_t lines;
    size_t warnings;
    unsigned threads_used;
} ib_fabric;

/* model.c */
void ib_fabric_init(ib_fabric *f);
void ib_fabric_free(ib_fabric *f);
/* Pre-sizes the arrays for an input of `bytes`; capacity only, nothing is counted as used. */
void ib_fabric_reserve(ib_fabric *f, size_t bytes);
uint32_t ib_node_add(ib_fabric *f);
uint32_t ib_port_add(ib_fabric *f);
uint32_t ib_intern(ib_fabric *f, ibt_slice s);
const char *ib_str(const ib_fabric *f, uint32_t off);
const char *ib_kind_name(ib_kind k);
const char *ib_speed_name(ib_speed s);
ib_speed ib_speed_from(const char *s, size_t n);
uint64_t ib_rate_mbps(ib_speed s, unsigned width);
int ib_compare_guid(uint64_t a, uint64_t b);
/* Returns the node already registered under the name, or IBT_NONE after registering `idx`. */
uint32_t ib_name_add(ib_fabric *f, uint32_t idx);
uint32_t ib_name_find(const ib_fabric *f, const char *name);

/* parse.c */
typedef struct {
    const char *name; /* used in diagnostics */
    bool strict;
    bool quiet;
    bool progress;    /* report progress on stderr */
    unsigned threads; /* 0 picks a count from the input size and CPU count */
} ib_parse_opts;

/* Returns 0 on success, -1 when strict mode stopped on a malformed line. */
int ib_parse(ib_fabric *f, const char *buf, size_t len, const ib_parse_opts *opts);

/* resolve.c */
void ib_resolve(ib_fabric *f, unsigned threads);

/* input.c */
typedef struct {
    const char *data;
    size_t len;
    void *map;
    char *heap;
} ib_input;

int ib_input_open(ib_input *in, const char *path);
void ib_input_close(ib_input *in);

/* check.c */
size_t ib_check(const ib_fabric *f, FILE *out, bool json);

/* diff.c */
size_t ib_diff(const ib_fabric *a, const ib_fabric *b, FILE *out, bool json);

/* output */
typedef enum {
    IB_FMT_JSON, IB_FMT_CSV, IB_FMT_CSV_NODES, IB_FMT_DOT,
    IB_FMT_SLURM, IB_FMT_IBND, IB_FMT_LEGACY, IB_FMT_COUNT
} ib_format;

int ib_format_from(const char *name, ib_format *out);
typedef struct {
    const char *exclude; /* slurm: skip adapters whose description contains this text */
} ib_write_opts;

void ib_write(const ib_fabric *f, ib_format fmt, FILE *out, const ib_write_opts *wo);
void ib_write_info(const ib_fabric *f, FILE *out);

void ib_write_json(const ib_fabric *f, FILE *out);
void ib_write_csv_links(const ib_fabric *f, FILE *out);
void ib_write_csv_nodes(const ib_fabric *f, FILE *out);
void ib_write_dot(const ib_fabric *f, FILE *out);
void ib_write_slurm(const ib_fabric *f, FILE *out, const char *exclude);
void ib_write_ibnd(const ib_fabric *f, FILE *out);
void ib_write_legacy(const ib_fabric *f, FILE *out);

/* hostlist.c: compresses names into Slurm hostlist notation. Sorts `names` in place. */
void ib_hostlist_print(FILE *out, const char **names, size_t n);

/* jsonutil */
void ib_json_str(FILE *out, const char *s);

#endif
