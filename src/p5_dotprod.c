/* p5_dotprod.c -- Problem 5: dot product of two length-N vectors whose entries
 * are drawn from {-1,0,1}. Default N = 10^9.
 *
 * THIS KERNEL IS MEMORY-BOUND, SO THE OPTIMISATION IS ABOUT BYTES, NOT FLOPS
 * -------------------------------------------------------------------------
 * There are only 2N arithmetic operations but 2N element loads, so runtime is
 * set by how many bytes cross the memory bus. Hence:
 *
 *   - int8_t storage. The values need 2 bits; storing them as int (the obvious
 *     choice) would move 8 GB instead of 2 GB for N = 10^9 and run 4x slower.
 *     This single decision matters more than everything else in the file.
 *   - Blocked accumulation. The inner loop accumulates into an int32, which
 *     lets the compiler keep the reduction in vector registers and widen 8-bit
 *     lanes efficiently; a bare `int64 += a[i]*b[i]` forces a widening chain
 *     that vectorises far worse. Blocks are 2^20 elements, and each product is
 *     in {-1,0,1}, so |partial| <= 2^20 -- an int32 cannot overflow.
 *   - reduction(+) over int64 for the outer sum, exact for any N up to 2^63.
 *
 * Because the arithmetic is integer, addition is associative and the result is
 * *exactly* identical at 1 thread and at 16 -- no tolerance, no epsilon. The
 * program checks this itself by also computing a plain sequential sum and
 * requiring bitwise equality.
 *
 * Expect the speedup curve to flatten early. The kernel saturates memory
 * bandwidth after a few cores, and no amount of threads buys bandwidth that
 * does not exist. That flattening is the correct result, not a bug.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>

#define BLOCK ((size_t)1 << 20)

static int64_t dot_parallel(const int8_t *restrict a, const int8_t *restrict b, size_t n)
{
    const size_t nblk = (n + BLOCK - 1) / BLOCK;
    int64_t      sum  = 0;

#pragma omp parallel for schedule(static) reduction(+ : sum)
    for (size_t blk = 0; blk < nblk; blk++) {
        const size_t lo = blk * BLOCK, hi = IMIN(lo + BLOCK, n);
        int32_t      acc = 0;   /* |acc| <= BLOCK = 2^20, no overflow */
        for (size_t i = lo; i < hi; i++) acc += (int32_t)a[i] * (int32_t)b[i];
        sum += acc;
    }
    return sum;
}

/* The sequential reference. Deliberately the plainest loop that could work --
 * its job is to be obviously correct, not fast. */
static int64_t dot_reference(const int8_t *a, const int8_t *b, size_t n)
{
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (int64_t)a[i] * (int64_t)b[i];
    return sum;
}

static int selftest(void)
{
    const size_t ns[] = { 0, 1, 7, 1048575, 1048576, 1048577, 5000000 };
    int          ok   = 1;

    for (size_t c = 0; c < sizeof ns / sizeof *ns; c++) {
        const size_t n = ns[c];
        int8_t *a = xalloc(n + 1), *b = xalloc(n + 1);
        fill_pm1_i8(a, n, 11 + c);
        fill_pm1_i8(b, n, 77 + c);
        int64_t got = dot_parallel(a, b, n), want = dot_reference(a, b, n);
        if (got != want) {
            fprintf(stderr, "FAIL n=%zu got=%lld want=%lld\n", n, (long long)got,
                    (long long)want);
            ok = 0;
        }
        xfree(a); xfree(b);
    }
    /* Block-boundary sizes above are the point: 2^20-1, 2^20 and 2^20+1 catch
     * off-by-one errors in the blocked accumulation. */
    fprintf(stderr, "p5 selftest (parallel vs sequential at block boundaries): %s\n",
            ok ? "PASS" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    static const char *const opts[] = { "n", NULL };
    args_validate(argc, argv, opts);

    const int threads = setup_threads(argc, argv);

    if (arg_flag(argc, argv, "selftest")) return selftest() ? 0 : 1;

    const size_t   n    = (size_t)arg_double(argc, argv, "n", 1e9);
    const uint64_t seed = (uint64_t)arg_long(argc, argv, "seed", 1);

    int8_t *a = xalloc(n), *b = xalloc(n);

    const double t0 = wall_seconds();
    fill_pm1_i8(a, n, seed);
    fill_pm1_i8(b, n, seed ^ 0xA5A5ULL);
    const double t1 = wall_seconds();

    const int64_t dot = dot_parallel(a, b, n);
    const double  t2  = wall_seconds();

    /* Integer arithmetic is associative, so this must match exactly. */
    const int64_t ref  = dot_reference(a, b, n);
    const int     pass = (dot == ref);
    if (!pass)
        fprintf(stderr, "verify: parallel=%lld sequential=%lld\n", (long long)dot,
                (long long)ref);

    char extra[64];
    snprintf(extra, sizeof extra, "dot=%lld", (long long)dot);

    result_t r = { .problem = "p5",
                   .variant = "reduction",
                   .n       = (long)n,
                   .threads = threads,
                   .init_s  = t1 - t0,
                   .time_s  = t2 - t1,
                   .pass    = pass,
                   .sum     = (uint64_t)dot,
                   .extra   = extra };
    print_result(&r);

    xfree(a); xfree(b);
    return pass ? 0 : 1;
}
