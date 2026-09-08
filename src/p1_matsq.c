/* p1_matsq.c -- Problem 1: A^2 for A of order N, entries drawn from {-1,0,1},
 * stored row-major.
 *
 *   --algo omm    ordinary (unblocked) matrix multiplication
 *   --algo bmm    blocked matrix multiplication, --block {4,8,16,32,64}
 *   --algo naive  textbook i-j-k triple loop; the oracle, small N only
 *
 * WHY THIS IS FAST
 * ----------------
 * 1. Loop order i-k-j, not i-j-k.  In i-k-j the innermost loop walks A[k][*]
 *    and C[i][*] with unit stride, so it streams cache lines and the compiler
 *    auto-vectorises it.  i-j-k walks B down a column, one cache line per
 *    element.  No transpose is used anywhere, so the assignment's "count any
 *    transformation in the runtime" clause costs us nothing.
 *
 * 2. The {-1,0,1} domain is exploited directly.  With a[k] hoisted out of the
 *    j loop we can branch on it *once per k* rather than per element:
 *      a[k] ==  0  -> skip the entire inner loop  (~1/3 of all work vanishes)
 *      a[k] ==  1  -> c[j] += b[j]                (vector add, no multiply)
 *      a[k] == -1  -> c[j] -= b[j]                (vector sub, no multiply)
 *    A generic multiply path is kept for correctness if the domain ever
 *    changes.  Net: ~2/3 of the multiply-adds become plain adds.
 *
 * 3. int32_t elements.  |(A^2)[i][j]| <= N <= 4096, so 32 bits is exact and
 *    twice as many lanes fit in a vector register as with double.  Being
 *    integer also makes the arithmetic associative, which is why every
 *    algorithm below produces a bit-identical result.
 *
 * VERIFICATION (three independent layers)
 *   a. --selftest: OMM and every block size compared element-wise against the
 *      naive oracle at N = 257 (deliberately prime, to catch blocking edge
 *      cases where the block size does not divide N).
 *   b. Every run spot-checks 128 random output entries by recomputing their
 *      dot product directly. O(128*N), free.
 *   c. The run script compares the printed checksum of the OpenMP binary at
 *      every thread count against the separately compiled sequential binary.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- kernels -- */

/* The three inner loops, one per value of a[k]. See NOINLINE in common.h for
 * why these are separate functions rather than written in place -- it is worth
 * a 5.5x difference inside an OpenMP region. */
NOINLINE static void row_add(int32_t *restrict c, const int32_t *restrict b, int n)
{
    for (int j = 0; j < n; j++) c[j] += b[j];
}

NOINLINE static void row_sub(int32_t *restrict c, const int32_t *restrict b, int n)
{
    for (int j = 0; j < n; j++) c[j] -= b[j];
}

/* Generic fallback: only reached if the input domain is not {-1,0,1}. */
NOINLINE static void row_axpy(int32_t *restrict c, const int32_t *restrict b,
                              int32_t s, int n)
{
    for (int j = 0; j < n; j++) c[j] += s * b[j];
}

static inline void row_apply(int32_t *restrict c, const int32_t *restrict b,
                             int32_t aik, int n)
{
    if (aik == 1)       row_add(c, b, n);
    else if (aik == -1) row_sub(c, b, n);
    else                row_axpy(c, b, aik, n);
}

/* Ordinary matrix multiplication: C = A*A. Parallel over rows of C, so each
 * thread owns its output rows outright -- no reduction, no false sharing
 * (rows are 8 KB at N=2048, far wider than a cache line). */
static void omm(const int32_t *restrict A, int32_t *restrict C, int n)
{
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) {
        int32_t       *restrict c = C + (size_t)i * n;
        const int32_t *restrict a = A + (size_t)i * n;
        memset(c, 0, (size_t)n * sizeof *c);
        for (int k = 0; k < n; k++) {
            const int32_t aik = a[k];
            if (aik == 0) continue;
            row_apply(c, A + (size_t)k * n, aik, n);
        }
    }
}

/* Blocked matrix multiplication: C = A*A over bs x bs tiles.
 * Parallelised with collapse(2) over the (bi,bj) tile grid, so each C tile is
 * written by exactly one thread and no synchronisation is needed at all.
 * C is zeroed inside the timed region -- blocking needs a pre-zeroed
 * accumulator and it would be dishonest to hide that cost. */
static void bmm(const int32_t *restrict A, int32_t *restrict C, int n, int bs)
{
    const size_t total = (size_t)n * n;
#pragma omp parallel for schedule(static)
    for (size_t t = 0; t < total; t += 4096)
        memset(C + t, 0, IMIN((size_t)4096, total - t) * sizeof *C);

    const int nb = (n + bs - 1) / bs;
#pragma omp parallel for collapse(2) schedule(static)
    for (int bi = 0; bi < nb; bi++) {
        for (int bj = 0; bj < nb; bj++) {
            const int i0 = bi * bs, i1 = IMIN(i0 + bs, n);
            const int j0 = bj * bs, j1 = IMIN(j0 + bs, n);
            for (int k0 = 0; k0 < n; k0 += bs) {
                const int k1 = IMIN(k0 + bs, n);
                for (int i = i0; i < i1; i++) {
                    int32_t       *restrict c = C + (size_t)i * n;
                    const int32_t *restrict a = A + (size_t)i * n;
                    for (int k = k0; k < k1; k++) {
                        const int32_t aik = a[k];
                        if (aik == 0) continue;
                        row_apply(c + j0, A + (size_t)k * n + j0, aik, j1 - j0);
                    }
                }
            }
        }
    }
}

/* Textbook i-j-k. Slow on purpose: it is the oracle, not a contender. */
static void naive(const int32_t *restrict A, int32_t *restrict C, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            int32_t s = 0;
            for (int k = 0; k < n; k++) s += A[(size_t)i * n + k] * A[(size_t)k * n + j];
            C[(size_t)i * n + j] = s;
        }
}

/* ----------------------------------------------------------- verification -- */

/* Recompute a sample of output entries from scratch. Cheap enough to leave on
 * for every timed run. */
static int spot_check(const int32_t *A, const int32_t *C, int n, uint64_t seed)
{
    rng_t r = rng_stream(seed ^ 0xC0FFEEULL, 0);
    for (int s = 0; s < 128; s++) {
        int     i = (int)rng_below(&r, (uint32_t)n);
        int     j = (int)rng_below(&r, (uint32_t)n);
        int32_t acc = 0;
        for (int k = 0; k < n; k++) acc += A[(size_t)i * n + k] * A[(size_t)k * n + j];
        if (acc != C[(size_t)i * n + j]) {
            fprintf(stderr, "spot_check: C[%d][%d] = %d, expected %d\n", i, j,
                    C[(size_t)i * n + j], acc);
            return 0;
        }
    }
    return 1;
}

static int selftest(void)
{
    const int  n     = 257;              /* prime: no block size divides it */
    const int  bss[] = {4, 8, 16, 32, 64};
    const size_t sz  = (size_t)n * n;
    int32_t   *A     = xalloc(sz * sizeof *A);
    int32_t   *ref   = xalloc(sz * sizeof *ref);
    int32_t   *got   = xalloc(sz * sizeof *got);
    int        ok    = 1;

    fill_pm1_i32(A, sz, 12345);
    naive(A, ref, n);

    omm(A, got, n);
    if (memcmp(ref, got, sz * sizeof *ref) != 0) { fprintf(stderr, "FAIL omm\n"); ok = 0; }

    for (size_t b = 0; b < sizeof bss / sizeof *bss; b++) {
        bmm(A, got, n, bss[b]);
        if (memcmp(ref, got, sz * sizeof *ref) != 0) {
            fprintf(stderr, "FAIL bmm bs=%d\n", bss[b]);
            ok = 0;
        }
    }

    fprintf(stderr, "p1 selftest (n=%d vs naive oracle): %s\n", n, ok ? "PASS" : "FAIL");
    xfree(A); xfree(ref); xfree(got);
    return ok;
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    static const char *const opts[] = { "n", "block", "algo", NULL };
    args_validate(argc, argv, opts);

    const int threads = setup_threads(argc, argv);

    if (arg_flag(argc, argv, "selftest")) return selftest() ? 0 : 1;

    const int         n    = (int)arg_long(argc, argv, "n", 2048);
    const int         bs   = (int)arg_long(argc, argv, "block", 32);
    const char       *algo = arg_str(argc, argv, "algo", "omm");
    const uint64_t    seed = (uint64_t)arg_long(argc, argv, "seed", 1);
    const size_t      sz   = (size_t)n * n;

    int32_t *A = xalloc(sz * sizeof *A);
    int32_t *C = xalloc(sz * sizeof *C);

    const double t0 = wall_seconds();
    fill_pm1_i32(A, sz, seed);
    const double t1 = wall_seconds();

    if      (strcmp(algo, "omm")   == 0) omm(A, C, n);
    else if (strcmp(algo, "bmm")   == 0) bmm(A, C, n, bs);
    else if (strcmp(algo, "naive") == 0) naive(A, C, n);
    else { fprintf(stderr, "unknown --algo %s\n", algo); return 2; }

    const double t2 = wall_seconds();

    char variant[32];
    if (strcmp(algo, "bmm") == 0) snprintf(variant, sizeof variant, "bmm-b%d", bs);
    else                          snprintf(variant, sizeof variant, "%s", algo);

    result_t r = { .problem = "p1",
                   .variant = variant,
                   .n       = n,
                   .threads = threads,
                   .init_s  = t1 - t0,
                   .time_s  = t2 - t1,
                   .pass    = spot_check(A, C, n, seed),
                   .sum     = checksum_par(C, sz * sizeof *C),
                   .extra   = NULL };
    print_result(&r);

    xfree(A); xfree(C);
    return r.pass ? 0 : 1;
}
