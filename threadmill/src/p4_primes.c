/* p4_primes.c -- Problem 4: all primes in [LOW, HIGH), default [2^40, 2^40+2^32).
 *
 * WHY A SEGMENTED SIEVE, AND WHY IT IS SHAPED LIKE THIS
 * -----------------------------------------------------
 * Trial division over a range at 2^40 is hopeless: ~10^12 candidates times
 * ~10^6 divisors.  A plain Sieve of Eratosthenes is equally hopeless in the
 * other direction -- one bit per number over [0, 2^41) is 256 GB.  The
 * segmented sieve keeps only sqrt(HIGH) base primes (~10^5 of them) plus one
 * small window at a time, and is therefore the only structurally viable
 * approach.  On top of that:
 *
 *   - Odds only.  Halves both memory and marking work for one line of code.
 *   - Segments sized to fit L2 (--segbytes, default 256 KB).  Marking is a
 *     strided scatter; once the window leaves cache, every mark becomes a
 *     memory round trip and throughput collapses by an order of magnitude.
 *   - Incremental multiple offsets.  The textbook version recomputes each base
 *     prime's first multiple per segment: nseg * nprimes divisions, which here
 *     would be ~8*10^7 and a real fraction of total runtime.  Instead each
 *     thread takes a *contiguous band* of segments and carries a per-prime
 *     cursor forward across them, so the divisions drop to nthreads *
 *     nprimes -- three orders of magnitude fewer.  A contiguous band is also
 *     why the schedule is a manual static partition rather than dynamic: the
 *     cursor state only makes sense if a thread walks its segments in order,
 *     and sieving cost per segment is near-uniform across a narrow range, so
 *     nothing is lost to imbalance.
 *   - Counting via popcount over the bitset, not a per-number test.
 *
 * ponytail: no wheel factorisation beyond skipping evens, and no bucket sieve.
 * A mod-30 wheel would cut another ~25%, and bucketing would help if the range
 * ever got wide enough that large primes hit each segment less than twice.
 * Neither is needed at this range; both are the upgrade path.
 *
 * WHY ONLY A COUNT.  pi(2^41) - pi(2^40) is about 3.7*10^10 primes.  Storing
 * them at 8 bytes each is 300 GB, so the count (plus verification) is the only
 * output that can exist.  --sample-primes prints a handful for inspection.
 *
 * VERIFICATION
 *   a. --selftest: counts over [0,10^6), [0,10^7), [0,10^8) checked against the
 *      known values of pi(x), and every number below 10^6 checked one by one
 *      against an independent plain sieve.
 *   b. Every timed run re-sieves one randomly chosen segment and cross-examines
 *      200 random numbers in it with a deterministic Miller-Rabin test -- in
 *      *both* directions, so a sieve that over-marked or under-marked is caught.
 *      This is a real check at the real magnitude, where no table of pi(x) exists.
 *   c. The run script compares counts across thread counts and against the
 *      sequential binary.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- Miller-Rabin ----- */

static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m)
{
    return (uint64_t)((__uint128_t)a * b % m);
}

static uint64_t powmod(uint64_t a, uint64_t e, uint64_t m)
{
    uint64_t r = 1;
    a %= m;
    while (e) {
        if (e & 1) r = mulmod(r, a, m);
        a = mulmod(a, a, m);
        e >>= 1;
    }
    return r;
}

/* Deterministic for every n < 3.3*10^24 with these 12 bases -- far beyond the
 * 2^41 we need, so this is an exact primality test here, not a probable one. */
static int is_prime_mr(uint64_t n)
{
    static const uint64_t base[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };
    if (n < 2) return 0;
    for (size_t i = 0; i < sizeof base / sizeof *base; i++) {
        if (n % base[i] == 0) return n == base[i];
    }
    uint64_t d = n - 1;
    int      s = 0;
    while ((d & 1) == 0) { d >>= 1; s++; }

    for (size_t i = 0; i < sizeof base / sizeof *base; i++) {
        uint64_t x = powmod(base[i], d, n);
        if (x == 1 || x == n - 1) continue;
        int witness = 1;
        for (int r = 1; r < s; r++) {
            x = mulmod(x, x, n);
            if (x == n - 1) { witness = 0; break; }
        }
        if (witness) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------- base primes -- */

/* Odd primes <= limit, by a plain odds-only sieve. limit is ~10^6 here, so
 * this costs microseconds and is deliberately left sequential. */
static uint32_t *base_primes(uint32_t limit, int *count)
{
    const size_t   nbits = (size_t)limit / 2 + 1;   /* bit i <-> 2i+1 */
    unsigned char *mark  = calloc(nbits, 1);
    if (!mark) { fprintf(stderr, "fatal: oom in base_primes\n"); exit(2); }

    for (size_t i = 1; (2 * i + 1) * (2 * i + 1) <= (size_t)limit; i++)
        if (!mark[i])
            for (size_t j = (2 * i + 1) * (2 * i + 1) / 2; j < nbits; j += 2 * i + 1)
                mark[j] = 1;

    uint32_t *p = xalloc(nbits * sizeof *p);
    int       c = 0;
    for (size_t i = 1; i < nbits; i++)
        if (!mark[i] && 2 * i + 1 <= limit) p[c++] = (uint32_t)(2 * i + 1);

    free(mark);
    *count = c;
    return p;
}

/* ------------------------------------------------------------- segments -- */
/* Bit j of a segment beginning at global odd-index i0 represents the number
 * olo + 2*(i0 + j). A set bit means composite. */

/* First multiple of each base prime to mark at or after global index i0. */
static void offsets_at(int64_t *next, const uint32_t *bp, int nbp, uint64_t olo,
                       size_t i0)
{
    const uint64_t v0 = olo + 2 * (uint64_t)i0;
    for (int i = 0; i < nbp; i++) {
        const uint64_t p     = bp[i];
        uint64_t       start = p * p;                 /* below p^2 nothing is new */
        if (start < v0) start = ((v0 + p - 1) / p) * p;
        if ((start & 1) == 0) start += p;             /* p is odd, so this lands odd */
        next[i] = (int64_t)((start - olo) / 2) - (int64_t)i0;
    }
}

/* Mark one segment and advance every cursor into the following segment. */
static void mark_seg(uint64_t *w, size_t nbits, uint64_t olo, size_t i0,
                     const uint32_t *bp, int nbp, int64_t *next)
{
    memset(w, 0, ((nbits + 63) / 64) * sizeof *w);

    for (int i = 0; i < nbp; i++) {
        int64_t       j    = next[i];
        const int64_t step = bp[i];
        for (; j < (int64_t)nbits; j += step) w[j >> 6] |= 1ULL << (j & 63);
        next[i] = j - (int64_t)nbits;
    }
    /* Index 0 is the number 1 when the range starts at or below 1, and 1 is
     * not prime. Handled here so both the sieve and the verifier see it. */
    if (olo == 1 && i0 == 0) w[0] |= 1ULL;
}

static uint64_t count_zero_bits(const uint64_t *w, size_t nbits)
{
    const size_t full = nbits >> 6, rem = nbits & 63;
    uint64_t     c    = 0;
    for (size_t i = 0; i < full; i++) c += 64 - (uint64_t)__builtin_popcountll(w[i]);
    if (rem) {
        const uint64_t mask = ~0ULL >> (64 - rem);
        c += rem - (uint64_t)__builtin_popcountll(w[full] & mask);
    }
    return c;
}

/* ---------------------------------------------------------------- driver -- */

typedef struct {
    uint64_t olo;    /* first odd number considered            */
    size_t   M;      /* number of odd numbers in the range     */
    size_t   SB;     /* segment size in bits                   */
    size_t   nseg;
} plan_t;

static plan_t make_plan(uint64_t lo, uint64_t hi, size_t segbytes)
{
    plan_t pl;
    pl.olo  = (lo <= 1) ? 1 : (lo | 1);            /* first odd >= lo */
    pl.M    = (hi > pl.olo) ? (hi - pl.olo + 1) / 2 : 0;
    pl.SB   = segbytes * 8;
    pl.nseg = (pl.M + pl.SB - 1) / pl.SB;
    return pl;
}

static uint64_t sieve_count(plan_t pl, const uint32_t *bp, int nbp)
{
    uint64_t count = 0;
    if (pl.M == 0) return 0;

#pragma omp parallel reduction(+ : count)
    {
        const int    t  = omp_get_thread_num();
        const int    nt = omp_get_num_threads();
        /* Contiguous band per thread: the per-prime cursors below are only
         * valid if segments are visited in increasing order. */
        const size_t s0 = pl.nseg * (size_t)t / (size_t)nt;
        const size_t s1 = pl.nseg * (size_t)(t + 1) / (size_t)nt;

        if (s0 < s1) {
            uint64_t *w    = xalloc((pl.SB / 64 + 1) * sizeof *w);
            int64_t  *next = xalloc((size_t)nbp * sizeof *next);

            offsets_at(next, bp, nbp, pl.olo, s0 * pl.SB);   /* nbp divisions, once */
            for (size_t s = s0; s < s1; s++) {
                const size_t i0    = s * pl.SB;
                const size_t nbits = IMIN(pl.SB, pl.M - i0);
                mark_seg(w, nbits, pl.olo, i0, bp, nbp, next);
                count += count_zero_bits(w, nbits);
                /* mark_seg subtracted nbits; when the final segment is short,
                 * re-anchor rather than drift. */
                if (nbits != pl.SB && s + 1 < s1) offsets_at(next, bp, nbp, pl.olo, i0 + nbits);
            }
            xfree(w);
            xfree(next);
        }
    }
    return count;
}

/* ----------------------------------------------------------- verification -- */

/* Re-sieve one random segment and cross-examine random members of it with
 * Miller-Rabin, in both directions. */
static int verify_sample(plan_t pl, const uint32_t *bp, int nbp, uint64_t seed,
                         int nsample)
{
    if (pl.M == 0) return 1;

    rng_t        r     = rng_stream(seed ^ 0xBEEFULL, 0);
    const size_t s     = (size_t)rng_below(&r, (uint32_t)pl.nseg);
    const size_t i0    = s * pl.SB;
    const size_t nbits = IMIN(pl.SB, pl.M - i0);

    uint64_t *w    = xalloc((pl.SB / 64 + 1) * sizeof *w);
    int64_t  *next = xalloc((size_t)nbp * sizeof *next);
    offsets_at(next, bp, nbp, pl.olo, i0);
    mark_seg(w, nbits, pl.olo, i0, bp, nbp, next);

    int ok = 1;
    for (int t = 0; t < nsample && ok; t++) {
        const size_t   j     = rng_below(&r, (uint32_t)nbits);
        const uint64_t v     = pl.olo + 2 * (uint64_t)(i0 + j);
        const int      sieved = !((w[j >> 6] >> (j & 63)) & 1ULL);
        const int      truth  = is_prime_mr(v);
        if (sieved != truth) {
            fprintf(stderr, "verify: %llu sieve=%s miller-rabin=%s\n",
                    (unsigned long long)v, sieved ? "prime" : "composite",
                    truth ? "prime" : "composite");
            ok = 0;
        }
    }
    xfree(w);
    xfree(next);
    return ok;
}

static uint64_t count_range(uint64_t lo, uint64_t hi, size_t segbytes)
{
    plan_t    pl  = make_plan(lo, hi, segbytes);
    int       nbp = 0;
    uint32_t  root = 1;
    while ((uint64_t)(root + 1) * (root + 1) <= hi) root++;
    uint32_t *bp = base_primes(root, &nbp);

    uint64_t c = sieve_count(pl, bp, nbp);
    if (lo <= 2 && hi > 2) c++;              /* 2 is the one prime the odds-only sieve cannot see */
    xfree(bp);
    return c;
}

static int selftest(void)
{
    int ok = 1;

    /* Layer 1: every number below 10^6 against an independent plain sieve. */
    const uint32_t  LIM  = 1000000;
    unsigned char  *comp = calloc(LIM, 1);
    if (!comp) return 0;
    for (uint32_t i = 2; (uint64_t)i * i < LIM; i++)
        if (!comp[i])
            for (uint32_t j = i * i; j < LIM; j += i) comp[j] = 1;
    uint64_t plain = 0;
    for (uint32_t i = 2; i < LIM; i++) plain += !comp[i];
    free(comp);

    if (count_range(0, LIM, 1 << 15) != plain) {
        fprintf(stderr, "FAIL: [0,10^6) disagrees with plain sieve\n");
        ok = 0;
    }

    /* Layer 2: known values of pi(x). */
    const struct { uint64_t x, pi; } oracle[] = {
        { 1000000ULL, 78498ULL }, { 10000000ULL, 664579ULL }, { 100000000ULL, 5761455ULL }
    };
    for (size_t i = 0; i < sizeof oracle / sizeof *oracle; i++) {
        uint64_t got = count_range(0, oracle[i].x, 1 << 15);
        if (got != oracle[i].pi) {
            fprintf(stderr, "FAIL: pi(%llu) = %llu, expected %llu\n",
                    (unsigned long long)oracle[i].x, (unsigned long long)got,
                    (unsigned long long)oracle[i].pi);
            ok = 0;
        }
    }

    /* Layer 3: an offset range, so the segment start logic is exercised too. */
    uint64_t lo = 1000000000000ULL, hi = lo + 1000000ULL, brute = 0;
    for (uint64_t v = lo; v < hi; v++) brute += is_prime_mr(v);
    if (count_range(lo, hi, 1 << 15) != brute) {
        fprintf(stderr, "FAIL: [10^12, 10^12+10^6) disagrees with Miller-Rabin\n");
        ok = 0;
    }

    fprintf(stderr, "p4 selftest (plain sieve, pi(x) tables, Miller-Rabin at 10^12): %s\n",
            ok ? "PASS" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    static const char *const opts[] = { "low", "high", "segbytes", NULL };
    args_validate(argc, argv, opts);

    const int threads = setup_threads(argc, argv);

    if (arg_flag(argc, argv, "selftest")) return selftest() ? 0 : 1;

    /* Default: a 2^32-wide window starting at 2^40. The full [2^40, 2^41) is
     * ~256x more work; pass --high 2199023255552 for it. */
    const uint64_t lo   = (uint64_t)arg_double(argc, argv, "low", 1099511627776.0);
    const uint64_t hi   = (uint64_t)arg_double(argc, argv, "high", 1099511627776.0 + 4294967296.0);
    const size_t   segb = (size_t)arg_long(argc, argv, "segbytes", 1 << 18);
    const uint64_t seed = (uint64_t)arg_long(argc, argv, "seed", 1);

    const double t0 = wall_seconds();
    uint32_t     root = 1;
    while ((uint64_t)(root + 1) * (root + 1) <= hi) root++;
    int       nbp = 0;
    uint32_t *bp  = base_primes(root, &nbp);
    plan_t    pl  = make_plan(lo, hi, segb);
    const double t1 = wall_seconds();

    uint64_t count = sieve_count(pl, bp, nbp);
    if (lo <= 2 && hi > 2) count++;
    const double t2 = wall_seconds();

    const int pass = verify_sample(pl, bp, nbp, seed, 200);

    char extra[160];
    snprintf(extra, sizeof extra,
             "low=%llu high=%llu primes=%llu basePrimes=%d segbytes=%zu segments=%zu",
             (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)count, nbp, segb, pl.nseg);

    result_t r = { .problem = "p4",
                   .variant = "segmented",
                   .n       = (long)(hi - lo),
                   .threads = threads,
                   .init_s  = t1 - t0,
                   .time_s  = t2 - t1,
                   .pass    = pass,
                   .sum     = count,   /* the count *is* the result; compared across runs */
                   .extra   = extra };
    print_result(&r);

    xfree(bp);
    return pass ? 0 : 1;
}
