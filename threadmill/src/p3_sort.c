/* p3_sort.c -- Problem 3: sort N integers drawn uniformly from [0, K) with
 * K = 1001 (the range "0 to 10^3", inclusive).
 *
 * THE POINT: THE RANGE IS TINY, SO DO NOT COMPARE
 * -----------------------------------------------
 * A comparison sort cannot beat O(N log N) and drags every element through
 * log2(N) ~ 30 cache-missing comparisons.  With only 1001 distinct keys,
 * counting sort is O(N + K), touches each element exactly once, and needs no
 * comparisons at all.  It is not a micro-optimisation, it is a different
 * complexity class, and on 10^8 elements it beats qsort by well over an order
 * of magnitude before any thread is spawned.  --algo qsort and --algo merge
 * are provided so the report can show that gap rather than assert it.
 *
 * Implementation notes that matter for scaling:
 *   - uint16_t elements.  Keys < 1024 need 10 bits; using 2 bytes instead of 4
 *     halves the memory traffic, and this kernel is purely bandwidth-bound.
 *   - Per-thread private histograms, each padded to a 64-byte multiple, so the
 *     counting phase has zero atomics and zero false sharing.
 *   - The scatter phase parallelises over the 1001 key values.  Each key owns a
 *     contiguous, disjoint output range, so threads never collide and the
 *     writes are long sequential runs the hardware prefetcher loves.
 *
 * --stream answers the literal 10^12 in the problem statement.  Counting sort
 * only ever needs the histogram, so with values generated on the fly the sort
 * runs in O(K) memory -- 8 KB -- instead of the 2 TB an in-memory array would
 * need.  The sorted sequence is reported run-length encoded, which is the only
 * form in which 10^12 elements can be written down at all.
 *
 * VERIFICATION.  Two checks that together *prove* correctness rather than
 * sampling it: the output is non-decreasing, and the output's histogram equals
 * the input's.  Sorted + same multiset = correct permutation, by definition.
 * --selftest additionally compares against qsort element-for-element.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MERGE_CUTOFF 8192   /* below this, task overhead exceeds the gain */

/* ------------------------------------------------------------- histogram -- */

/* Round the per-thread histogram stride up to a 64-byte multiple so no two
 * threads ever share a cache line. */
static size_t hist_stride(int k) { return ((size_t)k + 7u) & ~(size_t)7u; }

static void histogram(const uint16_t *v, size_t n, int k, uint64_t *out)
{
    const int    nt     = omp_get_max_threads();
    const size_t stride = hist_stride(k);
    uint64_t    *priv   = xalloc((size_t)nt * stride * sizeof *priv);

#pragma omp parallel
    {
        const int t = omp_get_thread_num();
        uint64_t *h = priv + (size_t)t * stride;
        memset(h, 0, stride * sizeof *h);
#pragma omp for schedule(static)
        for (size_t i = 0; i < n; i++) h[v[i]]++;
    }

    /* k is ~1000; reducing it is nothing next to the pass over n. */
    for (int j = 0; j < k; j++) {
        uint64_t s = 0;
        for (int t = 0; t < nt; t++) s += priv[(size_t)t * stride + j];
        out[j] = s;
    }
    xfree(priv);
}

/* --------------------------------------------------------- counting sort -- */

static void counting_sort(const uint16_t *restrict in, uint16_t *restrict out,
                          size_t n, int k, uint64_t *cnt)
{
    histogram(in, n, k, cnt);

    uint64_t *off = xalloc((size_t)k * sizeof *off);
    uint64_t  run = 0;
    for (int j = 0; j < k; j++) { off[j] = run; run += cnt[j]; }

    /* Each key value owns a disjoint, contiguous output range. */
#pragma omp parallel for schedule(static)
    for (int j = 0; j < k; j++) {
        uint16_t *restrict p = out + off[j];
        const uint64_t     c = cnt[j];
        for (uint64_t i = 0; i < c; i++) p[i] = (uint16_t)j;
    }
    xfree(off);
}

/* --------------------------------------------------------- merge sort ----- */

static int cmp_u16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

static void merge_runs(const uint16_t *a, size_t na, const uint16_t *b, size_t nb,
                       uint16_t *dst)
{
    size_t i = 0, j = 0, o = 0;
    while (i < na && j < nb) dst[o++] = (a[i] <= b[j]) ? a[i++] : b[j++];
    while (i < na) dst[o++] = a[i++];
    while (j < nb) dst[o++] = b[j++];
}

/* Task-parallel merge sort.  ponytail: the merge itself is sequential, so the
 * top-level O(n) merge caps speedup at roughly 3-4x by Amdahl.  A parallel
 * merge (co-ranking) would lift that, but counting sort is the right answer for
 * this key range anyway -- this path exists to quantify the comparison-sort
 * baseline, not to win. */
static void msort(uint16_t *a, uint16_t *tmp, size_t n, int depth)
{
    if (n <= MERGE_CUTOFF) { qsort(a, n, sizeof *a, cmp_u16); return; }
    const size_t m = n / 2;
    if (depth > 0) {
        /* firstprivate, not shared: the task only needs a copy of the pointer,
         * and copying sidesteps any question about this frame's lifetime. */
#pragma omp task firstprivate(a, tmp, m, depth)
        msort(a, tmp, m, depth - 1);
#pragma omp task firstprivate(a, tmp, m, n, depth)
        msort(a + m, tmp + m, n - m, depth - 1);
#pragma omp taskwait
    } else {
        msort(a, tmp, m, 0);
        msort(a + m, tmp + m, n - m, 0);
    }
    merge_runs(a, m, a + m, n - m, tmp);
    memcpy(a, tmp, n * sizeof *a);
}

static void merge_sort(uint16_t *a, uint16_t *tmp, size_t n)
{
    int depth = 1;
    while ((1 << depth) < omp_get_max_threads() * 4) depth++;   /* ~4 tasks/thread */
#pragma omp parallel
#pragma omp single
    msort(a, tmp, n, depth);
}

/* ----------------------------------------------------------- verification -- */

static int is_sorted(const uint16_t *v, size_t n)
{
    int bad = 0;
#pragma omp parallel for schedule(static) reduction(| : bad)
    for (size_t i = 1; i < n; i++) bad |= (v[i - 1] > v[i]);
    return !bad;
}

/* Sorted + identical multiset == a correct sort.  No sampling involved. */
static int verify(const uint16_t *out, size_t n, int k, const uint64_t *in_hist)
{
    if (!is_sorted(out, n)) { fprintf(stderr, "verify: output not sorted\n"); return 0; }
    uint64_t *h = xalloc((size_t)k * sizeof *h);
    histogram(out, n, k, h);
    int ok = memcmp(h, in_hist, (size_t)k * sizeof *h) == 0;
    if (!ok) fprintf(stderr, "verify: multiset changed\n");
    xfree(h);
    return ok;
}

static int selftest(void)
{
    const size_t ns[] = { 0, 1, 2, 1000, 100003, 500000 };
    const int    k    = 1001;
    int          ok   = 1;

    for (size_t c = 0; c < sizeof ns / sizeof *ns; c++) {
        const size_t n = ns[c];
        uint16_t *in  = xalloc((n + 1) * sizeof *in);
        uint16_t *out = xalloc((n + 1) * sizeof *out);
        uint16_t *ref = xalloc((n + 1) * sizeof *ref);
        uint16_t *tmp = xalloc((n + 1) * sizeof *tmp);
        uint64_t *cnt = xalloc((size_t)k * sizeof *cnt);

        fill_below_u16(in, n, (uint32_t)k, 99 + c);
        memcpy(ref, in, n * sizeof *in);
        qsort(ref, n, sizeof *ref, cmp_u16);

        counting_sort(in, out, n, k, cnt);
        if (n && memcmp(ref, out, n * sizeof *ref)) {
            fprintf(stderr, "FAIL counting n=%zu\n", n); ok = 0;
        }

        memcpy(out, in, n * sizeof *in);
        merge_sort(out, tmp, n);
        if (n && memcmp(ref, out, n * sizeof *ref)) {
            fprintf(stderr, "FAIL merge n=%zu\n", n); ok = 0;
        }

        xfree(in); xfree(out); xfree(ref); xfree(tmp); xfree(cnt);
    }
    fprintf(stderr, "p3 selftest (counting + merge vs qsort, incl. n=0,1,2): %s\n",
            ok ? "PASS" : "FAIL");
    return ok;
}

/* --------------------------------------------------------------- stream --- */

/* O(K) memory. Values are generated and histogrammed on the fly, never stored,
 * so N is limited by time rather than by RAM -- which is what makes the 10^12
 * in the problem statement answerable at all. */
static void stream_sort(size_t n, int k, uint64_t seed, uint64_t *cnt)
{
    const int    nt     = omp_get_max_threads();
    const size_t stride = hist_stride(k);
    const size_t nblk   = (n + FILL_BLOCK - 1) / FILL_BLOCK;
    uint64_t    *priv   = xalloc((size_t)nt * stride * sizeof *priv);

#pragma omp parallel
    {
        uint64_t *h = priv + (size_t)omp_get_thread_num() * stride;
        memset(h, 0, stride * sizeof *h);
#pragma omp for schedule(static)
        for (size_t b = 0; b < nblk; b++) {
            rng_t        r  = rng_stream(seed, b);
            const size_t lo = b * FILL_BLOCK, hi = IMIN(lo + FILL_BLOCK, n);
            for (size_t i = lo; i < hi; i++) h[rng_below(&r, (uint32_t)k)]++;
        }
    }
    for (int j = 0; j < k; j++) {
        uint64_t s = 0;
        for (int t = 0; t < nt; t++) s += priv[(size_t)t * stride + j];
        cnt[j] = s;
    }
    xfree(priv);
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    static const char *const opts[] = { "n", "k", "algo", "stream", NULL };
    args_validate(argc, argv, opts);

    const int threads = setup_threads(argc, argv);

    if (arg_flag(argc, argv, "selftest")) return selftest() ? 0 : 1;

    const size_t   n    = (size_t)arg_double(argc, argv, "n", 1e8);
    const int      k    = (int)arg_long(argc, argv, "k", 1001);
    const uint64_t seed = (uint64_t)arg_long(argc, argv, "seed", 1);
    const char    *algo = arg_str(argc, argv, "algo", "counting");
    uint64_t      *cnt  = xalloc((size_t)k * sizeof *cnt);

    if (arg_flag(argc, argv, "stream")) {
        const double t0 = wall_seconds();
        stream_sort(n, k, seed, cnt);
        const double t1 = wall_seconds();
        uint64_t total = 0;
        for (int j = 0; j < k; j++) total += cnt[j];
        char extra[64];
        snprintf(extra, sizeof extra, "rle_runs=%d total=%llu", k,
                 (unsigned long long)total);
        result_t r = { "p3", "counting-stream", (long)n, threads, 0.0, t1 - t0,
                       total == n, checksum(cnt, (size_t)k * sizeof *cnt), extra };
        print_result(&r);
        xfree(cnt);
        return r.pass ? 0 : 1;
    }

    uint16_t *in  = xalloc(n * sizeof *in);
    uint16_t *out = xalloc(n * sizeof *out);

    const double t0 = wall_seconds();
    fill_below_u16(in, n, (uint32_t)k, seed);
    const double t1 = wall_seconds();

    uint64_t *in_hist = xalloc((size_t)k * sizeof *in_hist);
    histogram(in, n, k, in_hist);            /* reference multiset, untimed */

    const double t2 = wall_seconds();
    if (strcmp(algo, "counting") == 0) {
        counting_sort(in, out, n, k, cnt);
    } else if (strcmp(algo, "merge") == 0) {
        memcpy(out, in, n * sizeof *in);
        uint16_t *tmp = xalloc(n * sizeof *tmp);
        merge_sort(out, tmp, n);
        xfree(tmp);
    } else if (strcmp(algo, "qsort") == 0) {
        memcpy(out, in, n * sizeof *in);
        qsort(out, n, sizeof *out, cmp_u16);
    } else {
        fprintf(stderr, "unknown --algo %s\n", algo);
        return 2;
    }
    const double t3 = wall_seconds();

    result_t r = { .problem = "p3",
                   .variant = algo,
                   .n       = (long)n,
                   .threads = threads,
                   .init_s  = t1 - t0,
                   .time_s  = t3 - t2,
                   .pass    = verify(out, n, k, in_hist),
                   .sum     = checksum_par(out, n * sizeof *out),
                   .extra   = NULL };
    print_result(&r);

    xfree(in); xfree(out); xfree(cnt); xfree(in_hist);
    return r.pass ? 0 : 1;
}
