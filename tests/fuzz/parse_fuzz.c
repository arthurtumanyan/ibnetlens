#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "../../src/ibnetlens.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static FILE *sink;
    if (!sink)
        sink = fopen("/dev/null", "w");

    ib_fabric f;
    ib_parse_opts po = {.name = "fuzz", .strict = false, .quiet = true, .threads = 3};
    ib_fabric_init(&f);
    ib_parse(&f, (const char *)data, size, &po);
    ib_resolve(&f, 1);
    ib_check(&f, sink, false);
    for (int fmt = 0; fmt < IB_FMT_COUNT; fmt++)
        ib_write(&f, (ib_format)fmt, sink, NULL);
    ib_diff(&f, &f, sink, true);
    ib_fabric_free(&f);
    return 0;
}
