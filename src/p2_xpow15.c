/* p2_xpow15.c -- Problem 2: X^15 where X = A + iB is a complex matrix of order
 * N, and A and B both hold values from {-1,0,1}.
 *
 * TWO ALGORITHMIC WINS, MULTIPLIED TOGETHER
 * -----------------------------------------
 * 1. Addition chain instead of 14 successive multiplies.
 *
 *      P = X*X      (X^2)      5 complex multiplications total, and 5 is
 *      U = P*X      (X^3)      provably optimal -- l(15) = 5 in the addition
 *      P = U*U      (X^6)      chain sense.  Plain binary exponentiation needs
 *      Q = P*P      (X^12)     6, the literal reading of the problem needs 14.
 *      P = Q*U      (X^15)
 *
 * 2. Three real multiplies per complex multiply instead of four (Gauss /
 *    Karatsuba):
 *
 *      M1 = A*C,  M2 = B*D,  M3 = (A+B)*(C+D)
 *      Re = M1 - M2
 *      Im = M3 - M1 - M2          [ = A*D + B*C ]
 *
 *    The three extra O(N^2) matrix additions are free next to an O(N^3)
 *    multiply.
 *
 * Together: 5 x 3 = 15 real matrix multiplications, against 14 x 4 = 56 for
 * the straightforward reading.  A 3.7x reduction in work before a single
 * thread is spawned.  --chain naive and --cmul 4 turn each optimisation off
 * independently so the report can attribute the speedup.
 *
 * PRECISION -- stated honestly
 * ----------------------------
 * Entries start in {-1,0,1} but grow roughly as s_{k+1} ~ sqrt(N) * s_k^2 under
 * squaring.  At N=2048 the entries of X^15 reach ~1e22, past the 2^53 ~ 9e15
 * where double still represents every integer exactly.  So at full size the
 * result is a floating-point approximation, and the program prints maxabs so
 * the magnitude is visible.  Two things are still exact:
 *
 *   - Parallel == sequential, bit for bit.  Each output row is accumulated by
 *     one thread in a fixed k order, so the rounding is identical at 1 thread
 *     and at 16.  The run script compares checksums across binaries.
 *   - --selftest runs N=64, where every intermediate stays below 2^53 and
 *     double arithmetic on integers is therefore exact.  All four combinations
 *     of {chain, naive} x {3-mult, 4-mult} must agree bit for bit, which
 *     validates the addition chain and the Karatsuba identity exactly.
 */
#include "common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double *re, *im; } cmat;

/* ------------------------------------------------------------ real kernel -- */

/* c += s * b. Hoisted into its own function on purpose -- see NOINLINE in
 * common.h; written inline inside the parallel region it loses most of its
 * unrolling and runs several times slower. This is the FMA loop the whole
 * problem reduces to, so it is the one loop worth being deliberate about. */
NOINLINE static void daxpy(double *restrict c, const double *restrict b, double s, int n)
{
    for (int j = 0; j < n; j++) c[j] += s * b[j];
}

/* C = A*B, i-k-j, parallel over rows of C.  Unit stride in the inner loop, one
 * thread per output row, so the k-accumulation order -- and hence the exact
 * floating-point result -- does not depend on the thread count. */
static void rmm(double *restrict C, const double *restrict A,
                const double *restrict B, int n)
{
#pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) {
        double       *restrict c = C + (size_t)i * n;
        const double *restrict a = A + (size_t)i * n;
        memset(c, 0, (size_t)n * sizeof *c);
        for (int k = 0; k < n; k++) {
            const double aik = a[k];
            if (aik == 0.0) continue;   /* pays for itself on the first, sparse, multiply */
            daxpy(c, B + (size_t)k * n, aik, n);
        }
    }
}

/* --------------------------------------------------------- elementwise ---- */

static void v_add(double *restrict d, const double *restrict x,
                  const double *restrict y, size_t sz)
{
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < sz; i++) d[i] = x[i] + y[i];
}

static void v_sub(double *restrict d, const double *restrict x,
                  const double *restrict y, size_t sz)
{
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < sz; i++) d[i] = x[i] - y[i];
}

/* d = x - y - z, the Karatsuba imaginary part in one pass over memory. */
static void v_sub2(double *restrict d, const double *restrict x,
                   const double *restrict y, const double *restrict z, size_t sz)
{
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < sz; i++) d[i] = x[i] - y[i] - z[i];
}

/* ------------------------------------------------------- complex multiply -- */

/* Five scratch real matrices, allocated once and reused by every multiply. */
typedef struct { double *s[5]; int use3; } ws_t;

/* dst = x * y.  dst must alias neither operand. */
static void cmul(cmat dst, cmat x, cmat y, int n, ws_t *w)
{
    const size_t sz = (size_t)n * n;
    double *m1 = w->s[0], *m2 = w->s[1], *m3 = w->s[2], *t1 = w->s[3], *t2 = w->s[4];

    if (w->use3) {
        v_add(t1, x.re, x.im, sz);
        v_add(t2, y.re, y.im, sz);
        rmm(m1, x.re, y.re, n);
        rmm(m2, x.im, y.im, n);
        rmm(m3, t1, t2, n);
        v_sub(dst.re, m1, m2, sz);
        v_sub2(dst.im, m3, m1, m2, sz);
    } else {
        rmm(m1, x.re, y.re, n);
        rmm(m2, x.im, y.im, n);
        rmm(m3, x.re, y.im, n);
        rmm(t1, x.im, y.re, n);
        v_sub(dst.re, m1, m2, sz);
        v_add(dst.im, m3, t1, sz);
    }
}

/* ---------------------------------------------------------------- powers -- */

/* X^15 via the optimal 5-multiplication addition chain.  Result lands in P. */
static void pow15_chain(cmat P, cmat Q, cmat U, cmat X, int n, ws_t *w)
{
    cmul(P, X, X, n, w);   /* X^2  */
    cmul(U, P, X, n, w);   /* X^3  */
    cmul(P, U, U, n, w);   /* X^6  */
    cmul(Q, P, P, n, w);   /* X^12 */
    cmul(P, Q, U, n, w);   /* X^15 */
}

/* X^15 by 14 successive multiplications -- the literal reading of the problem,
 * kept as the baseline the chain is measured against. Result lands in P. */
static void pow15_naive(cmat Pout, cmat Q, cmat X, int n, ws_t *w)
{
    const size_t bytes = (size_t)n * n * sizeof(double);
    cmat cur = Pout, nxt = Q;

    memcpy(cur.re, X.re, bytes);
    memcpy(cur.im, X.im, bytes);
    for (int e = 2; e <= 15; e++) {
        cmul(nxt, cur, X, n, w);
        cmat t = cur; cur = nxt; nxt = t;
    }
    /* 14 swaps is an even number, so `cur` is Pout again and this is a no-op.
     * Kept explicit so the function stays correct if the exponent changes. */
    if (cur.re != Pout.re) {
        memcpy(Pout.re, cur.re, bytes);
        memcpy(Pout.im, cur.im, bytes);
    }
}

/* ------------------------------------------------------------------ misc -- */

static double maxabs(const double *v, size_t sz)
{
    double m = 0.0;
#pragma omp parallel for schedule(static) reduction(max : m)
    for (size_t i = 0; i < sz; i++) {
        double a = fabs(v[i]);
        if (a > m) m = a;
    }
    return m;
}

static cmat cmat_alloc(int n)
{
    const size_t sz = (size_t)n * n;
    cmat c = { xalloc(sz * sizeof(double)), xalloc(sz * sizeof(double)) };
    return c;
}

static void cmat_free(cmat c) { xfree(c.re); xfree(c.im); }

/* ----------------------------------------------------------- verification -- */

/* At N=64 every intermediate stays well below 2^53, so double arithmetic on
 * these integers is exact and all four algorithm combinations must produce
 * bit-identical matrices.  If the magnitude assumption ever fails the test
 * reports INCONCLUSIVE instead of quietly passing. */
static int selftest(void)
{
    const int    n     = 64;
    const size_t sz    = (size_t)n * n;
    const size_t bytes = sz * sizeof(double);

    cmat X = cmat_alloc(n), P = cmat_alloc(n), Q = cmat_alloc(n), U = cmat_alloc(n);
    ws_t w = { { 0 }, 1 };
    for (int i = 0; i < 5; i++) w.s[i] = xalloc(bytes);

    fill_pm1_f64(X.re, sz, 7);
    fill_pm1_f64(X.im, sz, 8);

    double *ref_re = xalloc(bytes), *ref_im = xalloc(bytes);
    int     ok = 1, first = 1;
    double  worst = 0.0;

    const char *names[4] = { "chain+3mult", "chain+4mult", "naive+3mult", "naive+4mult" };
    for (int variant = 0; variant < 4; variant++) {
        w.use3 = (variant % 2 == 0);
        if (variant < 2) pow15_chain(P, Q, U, X, n, &w);
        else             pow15_naive(P, Q, X, n, &w);

        double mr = maxabs(P.re, sz), mi = maxabs(P.im, sz);
        if (mr > worst) worst = mr;
        if (mi > worst) worst = mi;

        if (first) {
            memcpy(ref_re, P.re, bytes);
            memcpy(ref_im, P.im, bytes);
            first = 0;
        } else if (memcmp(ref_re, P.re, bytes) || memcmp(ref_im, P.im, bytes)) {
            fprintf(stderr, "FAIL: %s differs from %s\n", names[variant], names[0]);
            ok = 0;
        }
    }

    const double EXACT_LIMIT = 9007199254740992.0;   /* 2^53 */
    if (worst >= EXACT_LIMIT) {
        fprintf(stderr, "p2 selftest INCONCLUSIVE: maxabs %.3e >= 2^53\n", worst);
        ok = 0;
    } else {
        fprintf(stderr,
                "p2 selftest (n=%d, all 4 algorithm variants bit-identical, "
                "maxabs %.3e < 2^53 so double is exact here): %s\n",
                n, worst, ok ? "PASS" : "FAIL");
    }

    for (int i = 0; i < 5; i++) xfree(w.s[i]);
    xfree(ref_re); xfree(ref_im);
    cmat_free(X); cmat_free(P); cmat_free(Q); cmat_free(U);
    return ok;
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    static const char *const opts[] = { "n", "cmul", "chain", NULL };
    args_validate(argc, argv, opts);

    const int threads = setup_threads(argc, argv);

    if (arg_flag(argc, argv, "selftest")) return selftest() ? 0 : 1;

    const int      n     = (int)arg_long(argc, argv, "n", 2048);
    const uint64_t seed  = (uint64_t)arg_long(argc, argv, "seed", 1);
    const int      use3  = (int)arg_long(argc, argv, "cmul", 3) == 3;
    const int      chain = strcmp(arg_str(argc, argv, "chain", "opt"), "opt") == 0;
    const size_t   sz    = (size_t)n * n;

    cmat X = cmat_alloc(n), P = cmat_alloc(n), Q = cmat_alloc(n);
    cmat U = chain ? cmat_alloc(n) : (cmat){ NULL, NULL };
    ws_t w = { { 0 }, use3 };
    for (int i = 0; i < 5; i++) w.s[i] = xalloc(sz * sizeof(double));

    const double t0 = wall_seconds();
    fill_pm1_f64(X.re, sz, seed);
    fill_pm1_f64(X.im, sz, seed ^ 0x5EEDULL);
    const double t1 = wall_seconds();

    if (chain) pow15_chain(P, Q, U, X, n, &w);
    else       pow15_naive(P, Q, X, n, &w);

    const double t2 = wall_seconds();

    /* Checksum both halves as one stream so re and im are both covered. */
    uint64_t s = checksum_par(P.re, sz * sizeof(double));
    s ^= checksum_par(P.im, sz * sizeof(double)) * 1099511628211ULL;

    const double mr = maxabs(P.re, sz), mi = maxabs(P.im, sz);

    char variant[48], extra[96];
    snprintf(variant, sizeof variant, "%s-cmul%d", chain ? "chain" : "naive", use3 ? 3 : 4);
    snprintf(extra, sizeof extra, "realmuls=%d maxabs=%.6e",
             (chain ? 5 : 14) * (use3 ? 3 : 4), mr > mi ? mr : mi);

    result_t r = { .problem = "p2",
                   .variant = variant,
                   .n       = n,
                   .threads = threads,
                   .init_s  = t1 - t0,
                   .time_s  = t2 - t1,
                   .pass    = 1,   /* cross-binary checksum comparison is the real gate */
                   .sum     = s,
                   .extra   = extra };
    print_result(&r);

    for (int i = 0; i < 5; i++) xfree(w.s[i]);
    cmat_free(X); cmat_free(P); cmat_free(Q);
    if (chain) cmat_free(U);
    return 0;
}
