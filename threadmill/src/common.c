#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ---------------------------------------------------------------- timing -- */

double wall_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

/* --------------------------------------------------------------- memory --- */

void *xalloc(size_t bytes)
{
    size_t rounded = (bytes + 63u) & ~(size_t)63u;
    void  *p       = aligned_alloc(64, rounded ? rounded : 64);
    if (!p) {
        fprintf(stderr, "fatal: out of memory allocating %zu bytes\n", bytes);
        exit(2);
    }
    return p;
}

void xfree(void *p) { free(p); }

/* ---------------------------------------------------------------- fills --- */

/* One macro because the four fills differ only in element type and the
 * expression that turns a draw into a value. Writing it four times by hand
 * would just be four places to get the block seeding wrong. */
#define DEFINE_FILL(name, type, expr)                                          \
    void name(type *v, size_t n, uint64_t seed)                                \
    {                                                                          \
        const size_t nblk = (n + FILL_BLOCK - 1) / FILL_BLOCK;                 \
        _Pragma("omp parallel for schedule(static)")                           \
        for (size_t b = 0; b < nblk; b++) {                                    \
            rng_t        r  = rng_stream(seed, b);                             \
            const size_t lo = b * FILL_BLOCK;                                  \
            const size_t hi = IMIN(lo + FILL_BLOCK, n);                        \
            for (size_t i = lo; i < hi; i++) v[i] = (type)(expr);              \
        }                                                                      \
    }

DEFINE_FILL(fill_pm1_i8,  int8_t,  rng_pm1(&r))
DEFINE_FILL(fill_pm1_i32, int32_t, rng_pm1(&r))
DEFINE_FILL(fill_pm1_f64, double,  rng_pm1(&r))

void fill_below_u16(uint16_t *v, size_t n, uint32_t k, uint64_t seed)
{
    const size_t nblk = (n + FILL_BLOCK - 1) / FILL_BLOCK;
#pragma omp parallel for schedule(static)
    for (size_t b = 0; b < nblk; b++) {
        rng_t        r  = rng_stream(seed, b);
        const size_t lo = b * FILL_BLOCK;
        const size_t hi = IMIN(lo + FILL_BLOCK, n);
        for (size_t i = lo; i < hi; i++) v[i] = (uint16_t)rng_below(&r, k);
    }
}

/* ------------------------------------------------------------- checksum --- */

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME  1099511628211ULL

static inline uint64_t fnv1a(const unsigned char *p, size_t bytes, uint64_t h)
{
    for (size_t i = 0; i < bytes; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

uint64_t checksum(const void *p, size_t bytes)
{
    return fnv1a((const unsigned char *)p, bytes, FNV_OFFSET);
}

uint64_t checksum_par(const void *p, size_t bytes)
{
    const unsigned char *b    = (const unsigned char *)p;
    const size_t         CHK  = 1u << 20;
    const size_t         nblk = (bytes + CHK - 1) / CHK;
    uint64_t            *part = (uint64_t *)xalloc(nblk * sizeof *part);

#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < nblk; i++) {
        size_t lo = i * CHK, hi = IMIN(lo + CHK, bytes);
        part[i] = fnv1a(b + lo, hi - lo, FNV_OFFSET);
    }

    /* Combined in block order, so the value does not depend on thread count. */
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < nblk; i++)
        h = fnv1a((const unsigned char *)&part[i], sizeof part[i], h);

    xfree(part);
    return h;
}

/* -------------------------------------------------------------- cli args -- */

/* Returns the value string for --name, or NULL. Handles "--name=v" and
 * "--name v". */
static const char *arg_find(int argc, char **argv, const char *name)
{
    const size_t len = strlen(name);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) != 0) continue;
        const char *a = argv[i] + 2;
        if (strncmp(a, name, len) != 0) continue;
        if (a[len] == '=') return a + len + 1;
        if (a[len] == '\0') return (i + 1 < argc) ? argv[i + 1] : "";
    }
    return NULL;
}

long arg_long(int argc, char **argv, const char *name, long def)
{
    const char *v = arg_find(argc, argv, name);
    if (!v || !*v) return def;
    return strtol(v, NULL, 0);
}

double arg_double(int argc, char **argv, const char *name, double def)
{
    const char *v = arg_find(argc, argv, name);
    if (!v || !*v) return def;
    return strtod(v, NULL);
}

const char *arg_str(int argc, char **argv, const char *name, const char *def)
{
    const char *v = arg_find(argc, argv, name);
    return (v && *v) ? v : def;
}

int arg_flag(int argc, char **argv, const char *name)
{
    const size_t len = strlen(name);
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "--", 2) == 0 && strncmp(argv[i] + 2, name, len) == 0 &&
            argv[i][2 + len] == '\0')
            return 1;
    return 0;
}

void args_validate(int argc, char **argv, const char *const *known)
{
    static const char *const always[] = { "threads", "seed", "selftest", NULL };

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) != 0) continue;   /* a value, not an option */

        const char  *a   = argv[i] + 2;
        const size_t len = strcspn(a, "=");             /* name ends at '=' or NUL */
        int          ok  = 0;

        for (const char *const *k = always; *k && !ok; k++)
            ok = (strlen(*k) == len && strncmp(a, *k, len) == 0);
        for (const char *const *k = known; *k && !ok; k++)
            ok = (strlen(*k) == len && strncmp(a, *k, len) == 0);

        if (!ok) {
            fprintf(stderr, "error: unrecognised option \"%s\"\n", argv[i]);
            if (strchr(a, ' '))
                fprintf(stderr, "hint: this argument contains a space, so the whole "
                                "string arrived as one argument.\n"
                                "      Quoting like \"--algo bmm --block 4\" does not "
                                "split under zsh; pass the flags separately.\n");
            fprintf(stderr, "known options: --threads --seed --selftest");
            for (const char *const *k = known; *k; k++) fprintf(stderr, " --%s", *k);
            fputc('\n', stderr);
            exit(2);
        }
    }
}

int setup_threads(int argc, char **argv)
{
    long t = arg_long(argc, argv, "threads", 0);
    /* Dynamic adjustment off: otherwise the runtime may quietly give us fewer
     * threads than requested and the speedup column becomes a fiction. */
    omp_set_dynamic(0);
    if (t > 0) omp_set_num_threads((int)t);
    int actual = 0;
#pragma omp parallel
    {
#pragma omp master
        actual = omp_get_num_threads();
    }
    return actual;
}

/* --------------------------------------------------------------- report --- */

void print_result(const result_t *r)
{
    printf("RESULT problem=%s variant=%s n=%ld threads=%d init=%.6f time=%.6f "
           "verify=%s sum=0x%016llx%s%s\n",
           r->problem, r->variant, r->n, r->threads, r->init_s, r->time_s,
           r->pass ? "PASS" : "FAIL", (unsigned long long)r->sum,
           r->extra ? " " : "", r->extra ? r->extra : "");
    fflush(stdout);
}
