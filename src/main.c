#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "hex.h"

#include "ibnetlens.h"

typedef struct {
    const char *output;
    const char *exclude;
    ib_format format;
    bool strict;
    bool quiet;
    bool json;
    bool stats;
    int progress; /* 1 on, -1 off, 0 only when stderr is a terminal */
    unsigned threads;
} options;

static void usage(FILE *out)
{
    fputs("usage: ibnetlens [parse] [-F format] [-o file] [--exclude TEXT] [-j N] [--strict] [-q] FILE|-\n"
          "       ibnetlens check [--json] [--strict] [-q] FILE|-\n"
          "       ibnetlens diff  [--json] [-q] OLD NEW\n"
          "       ibnetlens info  [-q] FILE|-\n"
          "\n"
          "common options:\n"
          "  -j N            use N threads (default: chosen from input size and CPU count)\n"
          "  --progress      show parse progress on stderr (default: only on a terminal)\n"
          "  --no-progress   never show progress; use this in scripts and cron jobs\n"
          "  --stats         print timings and peak memory to stderr when done\n"
          "  -q, --quiet     suppress parser warnings (also hides progress unless --progress)\n"
          "\n"
          "formats: json (default), csv, csv-nodes, dot, slurm, ibnd, legacy\n"
          "exit status: 0 ok, 1 findings or differences, 2 usage or input error\n",
          out);
}

static bool is_command(const char *s)
{
    return !strcmp(s, "parse") || !strcmp(s, "check") || !strcmp(s, "diff") || !strcmp(s, "info");
}

typedef struct {
    double parse_s;
    double resolve_s;
} load_times;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool want_progress(const options *o)
{
    if (o->progress)
        return o->progress > 0;
    return !o->quiet && isatty(STDERR_FILENO);
}

/* Peak resident memory in KB. VmHWM belongs to this process image only; ru_maxrss can start
 * from the high-water mark of the parent that forked us. */
static long peak_memory_kb(void)
{
    FILE *fp = fopen("/proc/self/status", "r");
    if (fp) {
        char line[128];
        long kb = 0;
        while (fgets(line, sizeof line, fp)) {
            if (sscanf(line, "VmHWM: %ld kB", &kb) == 1) {
                fclose(fp);
                return kb;
            }
        }
        fclose(fp);
    }
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0;
#ifdef __APPLE__
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
}

static void print_stats(const ib_fabric *f, const load_times *t, double output_s, double total_s)
{
    long peak_kb = peak_memory_kb();
    fprintf(stderr,
            "ibnetlens: stats parse_s=%.4f resolve_s=%.4f output_s=%.4f total_s=%.4f peak_kb=%ld threads=%u simd=%d "
            "nodes=%zu ports=%zu\n",
            t->parse_s, t->resolve_s, output_s, total_s, peak_kb, f->threads_used, ibt_hex_simd ? 1 : 0, f->nnodes,
            f->nports);
}

static int load(const char *path, ib_fabric *f, const options *o, load_times *times)
{
    ib_input in;
    if (ib_input_open(&in, path) != 0) {
        fprintf(stderr, "ibnetlens: %s: %s\n", path, strerror(errno));
        return -1;
    }
    ib_parse_opts po = {.name = strcmp(path, "-") == 0 ? "<stdin>" : path, .strict = o->strict, .quiet = o->quiet,
                       .progress = want_progress(o), .threads = o->threads};
    double t0 = now_s();
    int rc = ib_parse(f, in.data, in.len, &po);
    times->parse_s = now_s() - t0;
    ib_input_close(&in);
    if (rc != 0)
        return -1;
    t0 = now_s();
    ib_resolve(f, o->threads);
    times->resolve_s = now_s() - t0;
    return 0;
}

static FILE *open_output(const char *path)
{
    if (!path)
        return stdout;
    FILE *fp = fopen(path, "w");
    if (!fp)
        fprintf(stderr, "ibnetlens: %s: %s\n", path, strerror(errno));
    return fp;
}

static int close_output(FILE *fp, const char *path)
{
    if (fp == stdout)
        return fflush(stdout) == 0 ? 0 : -1;
    if (fclose(fp) != 0) {
        fprintf(stderr, "ibnetlens: %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int run_one(const char *cmd, const char *path, const options *o)
{
    ib_fabric f;
    ib_fabric_init(&f);
    load_times times = {0, 0};
    double started = now_s();
    int status = 2;

    if (load(path, &f, o, &times) != 0)
        goto done;
    FILE *out = open_output(o->output);
    if (!out)
        goto done;

    status = 0;
    double out_start = now_s();
    if (strcmp(cmd, "check") == 0)
        status = ib_check(&f, out, o->json) ? 1 : 0;
    else if (strcmp(cmd, "info") == 0)
        ib_write_info(&f, out);
    else
        ib_write(&f, o->format, out, &(ib_write_opts){.exclude = o->exclude});
    if (close_output(out, o->output) != 0)
        status = 2;
    if (o->stats)
        print_stats(&f, &times, now_s() - out_start, now_s() - started);
done:
    ib_fabric_free(&f);
    return status;
}

static int run_diff(const char *old_path, const char *new_path, const options *o)
{
    ib_fabric a, b;
    ib_fabric_init(&a);
    ib_fabric_init(&b);
    int status = 2;

    load_times times = {0, 0};
    if (load(old_path, &a, o, &times) == 0 && load(new_path, &b, o, &times) == 0) {
        FILE *out = open_output(o->output);
        if (out) {
            status = ib_diff(&a, &b, out, o->json) ? 1 : 0;
            if (close_output(out, o->output) != 0)
                status = 2;
        }
    }
    ib_fabric_free(&a);
    ib_fabric_free(&b);
    return status;
}

int main(int argc, char **argv)
{
    options o = {.format = IB_FMT_JSON};
    const char *cmd = "parse";
    const char *files[2] = {NULL, NULL};
    int nfiles = 0;
    bool have_cmd = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            puts("ibnetlens " IBNETLENS_VERSION);
            return 0;
        } else if (!strcmp(a, "--progress")) {
            o.progress = 1;
        } else if (!strcmp(a, "--no-progress")) {
            o.progress = -1;
        } else if (!strcmp(a, "--stats")) {
            o.stats = true;
        } else if (!strcmp(a, "--strict")) {
            o.strict = true;
        } else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
            o.quiet = true;
        } else if (!strcmp(a, "--json")) {
            o.json = true;
        } else if (!strcmp(a, "-j") || !strcmp(a, "--threads")) {
            char *rest = NULL;
            long v = ++i < argc ? strtol(argv[i], &rest, 10) : -1;
            if (v < 1 || v > 256 || *rest) {
                fprintf(stderr, "ibnetlens: %s needs a count between 1 and 256\n", a);
                return 2;
            }
            o.threads = (unsigned)v;
        } else if (!strcmp(a, "--exclude")) {
            if (++i >= argc) {
                fprintf(stderr, "ibnetlens: option %s needs a value\n", a);
                return 2;
            }
            o.exclude = argv[i];
        } else if (!strcmp(a, "-F") || !strcmp(a, "--format") || !strcmp(a, "-o") || !strcmp(a, "--output")) {
            bool is_fmt = a[1] == 'F' || a[2] == 'f';
            if (++i >= argc) {
                fprintf(stderr, "ibnetlens: option %s needs a value\n", a);
                return 2;
            }
            if (is_fmt) {
                if (ib_format_from(argv[i], &o.format) != 0) {
                    fprintf(stderr, "ibnetlens: unknown format '%s'\n", argv[i]);
                    return 2;
                }
            } else {
                o.output = argv[i];
            }
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "ibnetlens: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        } else if (!have_cmd && nfiles == 0 && is_command(a)) {
            cmd = a;
            have_cmd = true;
        } else if (nfiles < 2) {
            files[nfiles++] = a;
        } else {
            usage(stderr);
            return 2;
        }
    }

    bool is_diff = strcmp(cmd, "diff") == 0;
    if (nfiles != (is_diff ? 2 : 1)) {
        usage(stderr);
        return 2;
    }
    return is_diff ? run_diff(files[0], files[1], &o) : run_one(cmd, files[0], &o);
}
