/* Generates a two-level fat-tree in ibnetdiscover format for tests and benchmarks.
 * usage: fattree LEAVES SPINES HOSTS_PER_LEAF [degrade|ibsim|ibsim-messy]
 * ibsim writes the free-form ibsim net file dialect; ibsim-messy adds GUID base lines, state lines, comments
 * and one duplicated node ID so that parser edge cases are exercised. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWITCH_BASE 0x0002c90100000000ULL
#define HOST_BASE   0x0002c90200000000ULL

static uint64_t spine_guid(unsigned s) { return SWITCH_BASE + 0x1000 + s; }
static uint64_t leaf_guid(unsigned l) { return SWITCH_BASE + 0x2000 + l; }
static uint64_t host_guid(unsigned l, unsigned h, unsigned nh) { return HOST_BASE + (uint64_t)l * nh + h; }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s LEAVES SPINES HOSTS_PER_LEAF [degrade]\n", argv[0]);
        return 2;
    }
    unsigned nl = (unsigned)atoi(argv[1]), ns = (unsigned)atoi(argv[2]), nh = (unsigned)atoi(argv[3]);
    int degrade = argc > 4 && strcmp(argv[4], "degrade") == 0;
    int ibsim = argc > 4 && strncmp(argv[4], "ibsim", 5) == 0;
    int messy = argc > 4 && strcmp(argv[4], "ibsim-messy") == 0;

    if (ibsim) {
        puts("# ibsim dialect fat-tree");
        for (unsigned s = 0; s < ns; s++) {
            if (messy && s == 1)
                puts("switchguid=0x300000");
            printf("Switch\t%u \"spine%03u\"\n", nl, s);
            for (unsigned l = 0; l < nl; l++)
                printf("[%u]\t\"leaf%03u\"[%u]\n", l + 1, l, nh + 1 + s);
            puts("");
        }
        for (unsigned l = 0; l < nl; l++) {
            if (messy && l % 7 == 3)
                printf("# leaf %u follows\nvendid=0x2c9\ndevid=0xd2f0\n", l);
            if (messy && l % 11 == 5)
                printf("switchguid=0x%x\n", 0x500000 + l * 0x100);
            printf("Switch\t%u \"leaf%03u\"%s\n", nh + ns, l, messy && l % 5 == 0 ? "\t# \"leaf with desc\"" : "");
            for (unsigned h = 0; h < nh; h++)
                printf("[%u]\t\"node%04u\"%s\n", h + 1, l * nh + h, messy && h % 3 == 0 ? " w=4" : "");
            for (unsigned s = 0; s < ns; s++)
                printf("[%u]\t\"spine%03u\"[%u]\n", nh + 1 + s, s, l + 1);
            puts("");
            if (messy && l == nl / 2) { /* the same ID defined a second time */
                printf("Switch\t%u \"leaf%03u\"\n[1]\t\"node%04u\"\n\n", nh + ns, l, l * nh);
            }
        }
        for (unsigned l = 0; l < nl; l++) {
            for (unsigned h = 0; h < nh; h++) {
                if (messy && (l * nh + h) % 50 == 25)
                    printf("hcaguids=0x%x\n", 0x900000 + (l * nh + h) * 0x10);
                printf("%s\t1 \"node%04u\"\n[1]\t\"leaf%03u\"[%u]\n\n", messy && h % 4 == 1 ? "Hca" : "Ca", l * nh + h, l,
                       h + 1);
            }
        }
        return 0;
    }

    unsigned spine_lid_base = 1, leaf_lid_base = 1 + ns, host_lid_base = 1 + ns + nl;

    puts("#\n# Topology file: synthetic fat-tree\n#\n");

    for (unsigned s = 0; s < ns; s++) {
        uint64_t g = spine_guid(s);
        printf("vendid=0x2c9\ndevid=0xd2f0\nsysimgguid=0x%" PRIx64 "\nswitchguid=0x%" PRIx64 "(%" PRIx64 ")\n", g, g, g);
        printf("Switch\t%u \"S-%016" PRIx64 "\"\t\t# \"spine%03u\" base port 0 lid %u lmc 0\n", nl, g, s, spine_lid_base + s);
        for (unsigned l = 0; l < nl; l++)
            printf("[%u]\t\"S-%016" PRIx64 "\"[%u]\t\t# \"leaf%03u\" lid %u 4xHDR\n", l + 1, leaf_guid(l), nh + 1 + s, l,
                   leaf_lid_base + l);
        puts("");
    }
    for (unsigned l = 0; l < nl; l++) {
        uint64_t g = leaf_guid(l);
        printf("vendid=0x2c9\ndevid=0xd2f0\nsysimgguid=0x%" PRIx64 "\nswitchguid=0x%" PRIx64 "(%" PRIx64 ")\n", g, g, g);
        printf("Switch\t%u \"S-%016" PRIx64 "\"\t\t# \"leaf%03u\" base port 0 lid %u lmc 0\n", nh + ns, g, l, leaf_lid_base + l);
        for (unsigned h = 0; h < nh; h++) {
            uint64_t hg = host_guid(l, h, nh);
            printf("[%u]\t\"H-%016" PRIx64 "\"[1](%" PRIx64 ") \t\t# \"node%04u HCA-1\" lid %u 4xEDR\n", h + 1, hg, hg + 1,
                   l * nh + h, host_lid_base + l * nh + h);
        }
        for (unsigned s = 0; s < ns; s++) {
            const char *speed = (degrade && l == 0 && s == 0) ? "4xFDR" : "4xHDR";
            printf("[%u]\t\"S-%016" PRIx64 "\"[%u]\t\t# \"spine%03u\" lid %u %s\n", nh + 1 + s, spine_guid(s), l + 1, s,
                   spine_lid_base + s, speed);
        }
        puts("");
    }
    for (unsigned l = 0; l < nl; l++) {
        for (unsigned h = 0; h < nh; h++) {
            uint64_t hg = host_guid(l, h, nh);
            printf("vendid=0x2c9\ndevid=0x1017\nsysimgguid=0x%" PRIx64 "\ncaguid=0x%" PRIx64 "\n", hg, hg);
            printf("Ca\t1 \"H-%016" PRIx64 "\"\t\t# \"node%04u HCA-1\"\n", hg, l * nh + h);
            printf("[1](%" PRIx64 ") \t\"S-%016" PRIx64 "\"[%u]\t\t# lid %u lmc 0 \"leaf%03u\" lid %u 4xEDR\n", hg + 1,
                   leaf_guid(l), h + 1, host_lid_base + l * nh + h, l, leaf_lid_base + l);
            puts("");
        }
    }
    return 0;
}
