#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/hex.h"
#include "../../src/ibnetlens.h"

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                                   \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_guidmap(void)
{
    ibt_guidmap m;
    ibt_guidmap_init(&m);
    CHECK(ibt_guidmap_get(&m, 1) == IBT_NONE);
    for (uint32_t i = 0; i < 100000; i++)
        CHECK(ibt_guidmap_put(&m, 0x0002c90300000000ULL + i * 0x20, i) == IBT_NONE);
    for (uint32_t i = 0; i < 100000; i++)
        CHECK(ibt_guidmap_get(&m, 0x0002c90300000000ULL + i * 0x20) == i);
    CHECK(ibt_guidmap_put(&m, 0x0002c90300000000ULL, 7) == 0);
    CHECK(ibt_guidmap_get(&m, 0x0002c90300000000ULL) == 0);
    CHECK(ibt_guidmap_put(&m, 0, 5) == IBT_NONE);
    CHECK(ibt_guidmap_get(&m, 0) == 5);
    ibt_guidmap_free(&m);
}

static void hostlist_is(const char **names, size_t n, const char *want)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    ib_hostlist_print(fp, names, n);
    fclose(fp);
    if (strcmp(buf, want) != 0) {
        fprintf(stderr, "hostlist: got '%s', want '%s'\n", buf, want);
        failures++;
    }
    free(buf);
}

static void test_hostlist(void)
{
    const char *a[] = {"n3", "n1", "n2", "n7"};
    hostlist_is(a, 4, "n[1-3,7]");
    const char *b[] = {"node001", "node100", "node002"};
    hostlist_is(b, 3, "node[001-002,100]");
    const char *c[] = {"n9", "n10", "n11"};
    hostlist_is(c, 3, "n[9-11]");
    const char *d[] = {"login", "n1", "login", "gpu-a", "gpu-b"};
    hostlist_is(d, 5, "gpu-a,gpu-b,login,n1");
    const char *e[] = {"r1c2", "r1c1", "r2c1"};
    hostlist_is(e, 3, "r1c[1-2],r2c1");
}

static int parse_str(ib_fabric *f, const char *text, bool strict)
{
    ib_parse_opts po = {.name = "test", .strict = strict, .quiet = true};
    ib_fabric_init(f);
    int rc = ib_parse(f, text, strlen(text), &po);
    ib_resolve(f, 1);
    return rc;
}

static void test_parse(void)
{
    ib_fabric f;
    const char *crlf =
        "vendid=0x2c9\r\ndevid=0xd2f0\r\nsysimgguid=0x10\r\nswitchguid=0x10(10)\r\n"
        "Switch\t2 \"S-0000000000000010\"\t\t# \"a b\" enhanced port 0 lid 5 lmc 1\r\n"
        "[1]\t\"S-0000000000000020\"[1]\t\t# \"c\" lid 6 4xHDR\r\n\r\n"
        "vendid=0x2c9\r\ndevid=0xd2f0\r\nsysimgguid=0x20\r\nswitchguid=0x20(20)\r\n"
        "Switch\t2 \"S-0000000000000020\"\t\t# \"c\" base port 0 lid 6 lmc 0\r\n"
        "[1]\t\"S-0000000000000010\"[1]\t\t# \"a b\" lid 5 4xHDR";
    CHECK(parse_str(&f, crlf, true) == 0);
    CHECK(f.nnodes == 2 && f.nports == 2);
    CHECK(f.nodes[0].lid == 5 && f.nodes[0].lmc == 1 && f.nodes[0].base_type == IB_BASE_ENHANCED);
    CHECK(strcmp(ib_str(&f, f.nodes[0].desc), "a b") == 0);
    CHECK(f.ports[0].peer_idx == 1 && f.ports[1].peer_idx == 0);
    CHECK(f.ports[0].speed == IB_HDR && f.ports[0].width == 4 && f.ports[0].peer_lid == 6);
    CHECK(f.warnings == 0);
    ib_fabric_free(&f);

    CHECK(parse_str(&f, "vendid=0xzz\n", true) == -1);
    ib_fabric_free(&f);

    CHECK(parse_str(&f, "vendid=0xzz\ngarbage line\n", false) == 0);
    CHECK(f.warnings == 2 && f.nnodes == 0);
    ib_fabric_free(&f);

    CHECK(parse_str(&f, "", true) == 0 && f.nnodes == 0);
    ib_fabric_free(&f);

    /* values that overflow 64 bits must be rejected, not wrapped */
    CHECK(parse_str(&f, "sysimgguid=0x11111111111111111\n", true) == -1);
    ib_fabric_free(&f);

    /* link line before any node is ignored with a warning */
    CHECK(parse_str(&f, "[1]\t\"S-0000000000000020\"[1]\t\t# \"c\" lid 6 4xHDR\n", false) == 0);
    CHECK(f.nports == 0 && f.warnings == 1);
    ib_fabric_free(&f);
}

static void test_ibsim_dialect(void)
{
    ib_fabric f;
    const char *net =
        "Switch 4 \"sw\"\n[1] \"h\"[2]\n[2] \"h\"\n\n"
        "Hca 2 \"h\"\n[1] \"sw\"[2]\n[2] \"sw\"[1] w=8\n\n"
        "Hca 1 \"h\"\n";
    CHECK(parse_str(&f, net, false) == 0);
    CHECK(f.nnodes == 3);
    CHECK(f.warnings == 1); /* node "h" defined twice */
    CHECK(f.nodes[0].guid == 0x200000 && f.nodes[1].guid == 0x100000);
    CHECK(f.ports[0].peer_node == 1 && f.ports[0].peer_num == 2);
    CHECK(f.ports[1].peer_num == 1);
    CHECK(f.ports[3].width == 8);
    CHECK(f.ports[0].peer_idx == 3 && f.ports[3].peer_idx == 0);
    ib_fabric_free(&f);
}

/* The SIMD and scalar hex paths must agree on every input, valid or not. */
static void test_hex_paths(void)
{
    static const char alphabet[] = "0123456789abcdefABCDEF xg:@`/G\"-";
    unsigned seed = 12345;
    char buf[48];

    ibt_hex_init();
    if (!ibt_hex_simd) {
        puts("hex: SIMD path unavailable, scalar only");
        return;
    }
    for (int it = 0; it < 300000; it++) {
        for (size_t i = 0; i < sizeof buf; i++) {
            seed = seed * 1103515245u + 12345u;
            /* mostly hex so that long runs and exact 16-digit runs occur */
            unsigned r = (seed >> 16) % 100;
            buf[i] = r < 85 ? alphabet[(seed >> 8) % 22] : alphabet[(seed >> 8) % (sizeof alphabet - 1)];
        }
        seed = seed * 1103515245u + 12345u;
        size_t off = (seed >> 16) % 16;
        uint64_t a = 0, b = 0;
        ibt_hex_simd = true;
        size_t na = ibt_hex_prefix(buf + off, &a);
        ibt_hex_simd = false;
        size_t nb = ibt_hex_prefix(buf + off, &b);
        if (na != nb || (na >= 1 && na <= 16 && a != b)) {
            fprintf(stderr, "hex mismatch at '%.20s': simd %zu/%llx scalar %zu/%llx\n", buf + off, na,
                    (unsigned long long)a, nb, (unsigned long long)b);
            failures++;
            break;
        }
    }
    ibt_hex_init();
}

/* ibdiagnet writes a label in parentheses after every port number; labels need not be numbers. */
static void test_port_labels(void)
{
    ib_fabric f;
    const char *text =
        "Switch\t4 \"S-0000000000000010\"\t\t# \"sw\" base port 0 lid 1 lmc 0\n"
        "[1](sw1p2pl4) \"S-0000000000000020\"[3](FNM1pl4)  # \"sw2\" lid 2 4xHDR\n"
        "[2](2) \"H-0000000000000030\"[1](1)(2a)  # \"h HCA-1\" lid 3 4xEDR\n"
        "\n"
        "Ca\t1 \"H-0000000000000030\"\t\t# \"h HCA-1\"\n"
        "[1](1)(2a) \"S-0000000000000010\"[2](2)  # lid 3 lmc 0 \"sw\" lid 1 4xEDR\n";
    CHECK(parse_str(&f, text, true) == 0);
    CHECK(f.nnodes == 2 && f.nports == 3);
    CHECK(f.ports[0].peer_num == 3 && f.ports[0].peer_portguid == 0);
    CHECK(f.ports[1].peer_portguid == 0x2a && f.ports[1].peer_num == 1);
    CHECK(f.ports[2].portguid == 0x2a && f.ports[2].peer_num == 2 && f.ports[2].lid == 3);
    CHECK(f.warnings == 0);
    ib_fabric_free(&f);

    /* the classic layouts keep their meaning: one group is the port GUID where there is one */
    text = "Ca\t1 \"H-0000000000000030\"\n[1](2a) \"S-0000000000000010\"[2]\n";
    CHECK(parse_str(&f, text, true) == 0 && f.ports[0].portguid == 0x2a && f.ports[0].peer_portguid == 0);
    ib_fabric_free(&f);

    CHECK(parse_str(&f, "Ca\t1 \"H-0000000000000030\"\n[1](a)(b)(c) \"S-0000000000000010\"[2]\n", true) == -1);
    ib_fabric_free(&f);
}

/* Found by the CI fuzzer: the SIMD hex path read past a small heap buffer because the thread-local input limit still
 * pointed into an earlier, larger input. Needs AddressSanitizer to fail, so it runs in `make asan`. */
static void test_stale_input_limit(void)
{
    static const char big[] = "vendid=0x2c9\ndevid=0x1\nsysimgguid=0x2c90300000200\nswitchguid=0x2c90300000200(2c90300000200)\n"
                              "Switch\t4 \"S-0002c90300000200\"\t# \"sw\"\n[1]\t\"S-0002c90300000201\"[1]\t# \"sw2\" lid 2 4xHDR\n";
    static const unsigned char crash[] = {
        0x23, 0x20, 0x74, 0x76, 0x6f, 0x64, 0x3d, 0x30, 0x78, 0x30, 0x30, 0,    0,    0,    0,    0,    0,    0,
        0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
        0,    0,    0,    0,    0,    0x30, 0x32, 0x63, 0x39, 0x30, 0x33, 0x30, 0x30, 0x30, 0x30, 0x30, 0x32, 0x30,
        0x30, 0x0a, 0x73, 0x77, 0x69, 0x74, 0x63, 0x68, 0x67, 0x75, 0x69, 0x64, 0x30, 0x78, 0x3d, 0x30, 0x30, 0x30,
        0x32, 0x63, 0x39, 0x30, 0x33, 0x30, 0x30, 0x30, 0x30, 0x30, 0x32, 0x30, 0x30, 0x28, 0x30, 0x30, 0x30, 0x32,
        0x63, 0x39, 0x30, 0x33, 0x30, 0x30, 0x30, 0x30, 0x30, 0x32, 0x30, 0x30, 0x29, 0x0a, 0x53, 0x77, 0x69, 0x74,
        0x63, 0x68, 0x09, 0x34, 0x20, 0x22, 0x53, 0x2d, 0x70, 0x69, 0x6e, 0x65, 0x32, 0x22, 0x5b, 0x35, 0x5d, 0x0a};
    ib_parse_opts po = {.name = "test", .quiet = true, .threads = 3};
    ib_fabric f;

    size_t n = strlen(big);
    char *first = malloc(n);
    memcpy(first, big, n);
    ib_fabric_init(&f);
    CHECK(ib_parse(&f, first, n, &po) == 0 && f.nnodes == 1);
    ib_fabric_free(&f);
    free(first);

    char *second = malloc(sizeof crash); /* exact size: the ASan redzone starts right after the last header */
    memcpy(second, crash, sizeof crash);
    ib_fabric_init(&f);
    CHECK(ib_parse(&f, second, sizeof crash, &po) == 0);
    ib_fabric_free(&f);
    free(second);
}

int main(void)
{
    test_guidmap();
    test_hex_paths();
    test_hostlist();
    test_parse();
    test_ibsim_dialect();
    test_port_labels();
    test_stale_input_limit();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("unit tests passed");
    return 0;
}
