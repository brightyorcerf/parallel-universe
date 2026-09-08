/* common.h -- shared timing, RNG, allocation, CLI and reporting helpers.
 *
 * Two contracts everything else in this assignment relies on:
 *
 *  1. REPRODUCIBLE INPUT.  Every fill_* routine produces byte-identical output
 *     for a given (n, seed) no matter how many threads run it, because the RNG
 *     is seeded per fixed-size block index instead of per thread.  Without this
 *     a 1-thread run and a 16-thread run would be multiplying different
 *     matrices and "verify against sequential" would be meaningless.
 *
 *  2. FIRST TOUCH.  Those same parallel fills are the first writes to the
 *     pages, so on a NUMA machine each page lands on the node of the thread
 *     that will later read it.  Init is deliberately parallel for this reason.
 *
 * Timing is gettimeofday() as the assignment requires.
 */
#ifndef HPC_COMMON_H
#define HPC_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef _OPENMP
#  include <omp.h>
#else
/* Stubs so every program also compiles as a genuinely sequential binary
 * (make seq) -- that is the reference we verify the parallel runs against. */
static inline int  omp_get_max_threads(void) { return 1; }
static inline int  omp_get_num_threads(void) { return 1; }
static inline int  omp_get_thread_num(void)  { return 0; }
static inline void omp_set_num_threads(int t) { (void)t; }
static inline void omp_set_dynamic(int d)     { (void)d; }
#endif

#define IMIN(a, b) ((a) < (b) ? (a) : (b))
#define IMAX(a, b) ((a) > (b) ? (a) : (b))

/* Hot inner loops are hoisted into NOINLINE helpers with restrict-qualified
 * parameters. This is not cosmetic: when such a loop is written inline inside
 * an `omp parallel for` body, clang unrolls and interleaves it far less
 * aggressively than the same loop in a non-OpenMP build -- measured at 5.5x
 * slower for the problem-1 kernel at N=1024. Forcing the loop into its own
 * function gives the vectoriser a small, alias-free body to work on and
 * restores identical code generation in both builds. The call happens once per
 * k, around a loop of n iterations, so the call overhead is noise. */
#if defined(__GNUC__) || defined(__clang__)
#  define NOINLINE __attribute__((noinline))
#else
#  define NOINLINE
#endif

/* Block size used to partition every parallel fill.  Fixed so the RNG stream
 * layout -- and therefore the generated data -- is thread-count independent. */
#define FILL_BLOCK ((size_t)1 << 16)

/* ---------------------------------------------------------------- timing -- */

/* Wall clock seconds via gettimeofday(), as mandated by the assignment. */
double wall_seconds(void);

/* --------------------------------------------------------------- memory --- */

void *xalloc(size_t bytes);   /* 64-byte aligned, zero-filled on failure never; aborts on OOM */
void  xfree(void *p);

/* ------------------------------------------------------------------ rng --- */

/* splitmix64: one 64-bit word of state, good statistical quality, and cheap
 * enough that seeding a fresh stream per block costs nothing. */
static inline uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

typedef struct { uint64_t s; } rng_t;

static inline rng_t rng_stream(uint64_t seed, uint64_t block)
{
    rng_t r;
    r.s = splitmix64(seed ^ (block * 0x9E3779B97F4A7C15ULL));
    return r;
}

static inline uint64_t rng_next(rng_t *r)
{
    r->s += 0x9E3779B97F4A7C15ULL;
    uint64_t z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform on {-1,0,1}.  Multiply-shift instead of `% 3` -- no division in a
 * loop that runs 10^9 times. */
static inline int rng_pm1(rng_t *r)
{
    uint32_t hi = (uint32_t)(rng_next(r) >> 32);
    return (int)(((uint64_t)hi * 3u) >> 32) - 1;
}

/* Uniform on [0,k), k < 2^32. */
static inline uint32_t rng_below(rng_t *r, uint32_t k)
{
    uint32_t hi = (uint32_t)(rng_next(r) >> 32);
    return (uint32_t)(((uint64_t)hi * k) >> 32);
}

/* ---------------------------------------------------------------- fills --- */
/* All parallel, all thread-count independent, all first-touch. */

void fill_pm1_i8 (int8_t  *v, size_t n, uint64_t seed);
void fill_pm1_i32(int32_t *v, size_t n, uint64_t seed);
void fill_pm1_f64(double  *v, size_t n, uint64_t seed);
void fill_below_u16(uint16_t *v, size_t n, uint32_t k, uint64_t seed);

/* ------------------------------------------------------------- checksum --- */

/* FNV-1a over raw bytes.  Used to compare results *across processes* -- the
 * sequential binary and the OpenMP binary print the same number or the run is
 * rejected. */
uint64_t checksum(const void *p, size_t bytes);

/* Parallel variant for very large buffers: FNV-1a per block, combined in
 * block order, so it is still thread-count independent. */
uint64_t checksum_par(const void *p, size_t bytes);

/* -------------------------------------------------------------- cli args -- */
/* Accepts both "--name value" and "--name=value". */

long        arg_long  (int argc, char **argv, const char *name, long def);
double      arg_double(int argc, char **argv, const char *name, double def);
const char *arg_str   (int argc, char **argv, const char *name, const char *def);
int         arg_flag  (int argc, char **argv, const char *name);

/* Rejects any --option not in `known` (a NULL-terminated list) and exits.
 *
 * This exists because the silent-default failure mode is genuinely dangerous
 * here: a mistyped or badly quoted flag would otherwise fall back to the
 * default and produce a table of perfectly plausible timings for the wrong
 * algorithm. Better to refuse to run than to report a confident wrong number.
 * "--threads", "--seed" and "--selftest" are always accepted. */
void args_validate(int argc, char **argv, const char *const *known);

/* Applies --threads if given (default: OMP_NUM_THREADS / all cores) and pins
 * the count so the runtime cannot silently hand us fewer. Returns the count. */
int setup_threads(int argc, char **argv);

/* --------------------------------------------------------------- report --- */

typedef struct {
    const char *problem;   /* "p1" .. "p5"                                   */
    const char *variant;   /* "omm", "bmm-b32", "counting", ...              */
    long        n;         /* problem size                                   */
    int         threads;
    double      init_s;    /* setup time, reported but not part of `time_s`  */
    double      time_s;    /* the measured computation                       */
    int         pass;      /* 1 = verification passed                        */
    uint64_t    sum;       /* result checksum, compared across binaries      */
    const char *extra;     /* optional trailing "k=v k=v", may be NULL       */
} result_t;

/* Exactly one line on stdout, machine-parsable by run_experiments.sh.
 * Everything else a program has to say goes to stderr. */
void print_result(const result_t *r);

#endif /* HPC_COMMON_H */
