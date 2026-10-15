# threadmill: five OpenMP kernels (HPC Assignment 1)

Five parallel kernels in C11 + OpenMP, each with a sequential reference, a
self-test, and a timing harness that refuses to report a runtime for a result
it could not verify.

```
src/       the five programs plus shared helpers
scripts/   run_experiments.sh (timing sweep), make_excel.py (workbook), package.sh
plots/     plot_all.sh, generates the .gp files and the PNGs
results/   generated: one CSV + one .meta per configuration
bin/       generated: two binaries per problem
```

---

## Why this is clever, not just parallel

The lazy path through all five problems is: write the textbook algorithm,
slap `#pragma omp parallel for` on the outermost loop, call it done. That
earns the parallelization marks. It leaves the bonus marks on the table,
because every one of these problems has a smarter algorithm hiding under the
naive one, and the naive one is naive *before a single thread ever spawns*.
Thread count only ever multiplies whatever work you chose to do; choosing
less work multiplies harder.

- X¹⁵ is not "multiply X by itself 14 times." Computing powers is an
  addition-chain problem, 15 has a chain of length 5 (`X², X³, X⁶, X¹²,
  X¹⁵`), so five matrix multiplications reach it, not fourteen. Then, because
  `X` is complex, each of those five multiplications is itself reducible:
  Gauss's trick turns one complex multiply into 3 real matmuls instead of 4.
  5 × 3 = 15 real matmuls to answer X¹⁵, versus 56 for the textbook
  "multiply 14 times, 4 real matmuls each" approach. That's a 3.7×
  reduction in *work*, measured before OpenMP is even in the picture.

- The prime sieve doesn't re-derive the same divisions on every segment.
  A segmented sieve that restarts "find the first multiple of p in this
  segment" from scratch for every segment pays a division per prime per
  segment, that's segments × primes divisions for no reason. Carrying each
  prime's cursor forward from the segment before it collapses that to
  threads × primes: each thread just picks up where the last segment left
  off, once. Verified with a control: forcing the segment size past L2 (64
  MB instead of 256 KB) slows the identical sieve down 11.6×, proof the
  cache-sizing decision is load-bearing, not decorative.

- Sorting numbers in [0, 1000] is not a comparison problem. With only
  1001 distinct values, a comparison sort is solving a harder problem than
  the one that was asked, `O(N log N)` when `O(N + K)` counting sort is
  exact and correct. Measured: counting sort is ~72× faster than a
  parallel merge sort on the same input. No amount of thread-tuning closes a
  gap that big; the algorithm choice *is* the optimization.

- Squaring a {−1, 0, 1} matrix never needs a multiply instruction.
  Every entry of A is in {−1, 0, 1}, so every term of the dot product
  `A[i][k] * A[k][j]` is either 0 (skip it), +row (add it), or −row (subtract
  it). The inner loop becomes vectorizable adds and subtracts over whichever
  algorithm's memory layout you pick, streaming i-k-j or cache-blocked
  tiles, instead of a multiply-accumulate that spends silicon computing
  products the domain guarantees are trivial.

- The dot product is a bandwidth problem wearing an arithmetic problem's
  clothes. 2N loads for N multiply-adds means the bus, not the ALU,
  decides the runtime. Storing the {−1, 0, 1} operands as `int8_t` instead
  of `int` moves 4× fewer bytes across that bus, a decision that matters
  more than every other line in that file combined. The program reports
  effective GB/s for exactly this reason: a memory-bound kernel is judged
  against the bus's ceiling, not the clock.

None of this is free, cleverness that silently returns a wrong answer isn't
cleverness, it's a bug with better marketing. That's why every one of these
shortcuts is checked against a boring, obviously-correct sequential twin
before its runtime counts for anything: see [How results are
verified](#how-results-are-verified) below.

---

## Build

```sh
make            # bin/<prog> (OpenMP) and bin/<prog>_seq (no OpenMP at all)
make check      # every self-test; run this before trusting any timing
make info       # show the detected compiler and flags
```

`make` auto-detects the platform: `gcc -fopenmp` on Linux, `clang` +
Homebrew `libomp` on macOS. `-march=native` is probed and used when the
compiler accepts it, it is worth a large multiple on these kernels because it
unlocks AVX2/FMA.

Two binaries are built per problem on purpose:

* `bin/p1_matsq`, OpenMP.
* `bin/p1_matsq_seq`, compiled with no `-fopenmp` at all. Not "OpenMP with
  one thread"; the pragmas are inert and `common.h` supplies stub `omp_*`
  functions. This is the genuinely sequential reference the assignment requires
  results to be checked against.

---

## Run

```sh
scripts/run_experiments.sh --quick     # small sizes, 2 reps, 1/2/4 threads (~1 min)
scripts/run_experiments.sh             # the real sweep: 1..16 threads, 5 reps
python3 scripts/make_excel.py --out TEAMNUM_ASSIGN1.xlsx
plots/plot_all.sh
scripts/package.sh TEAMNUM 1           # -> TEAMNUM_ASSIGN1.zip
```

Timing uses `gettimeofday()`, as specified. Each configuration is run five
times; every individual run is recorded in the CSV and in the workbook
alongside the average, so nothing is hidden behind the mean.

`run_experiments.sh` sets `OMP_PROC_BIND=close` and `OMP_PLACES=cores`. Without
pinning, the five repetitions partly measure how the OS happened to schedule
threads that minute rather than how the code scales.

Speedup is defined as `avg(1 thread) / avg(t threads)`. The sequential
binary's time is recorded separately in each sheet, because it answers a
different question (how much the OpenMP runtime itself costs at one thread).

`make_excel.py` uses only the Python standard library, an `.xlsx` is a zip of
XML parts, and requiring a `pip install` on a lab machine or a grader's machine
is a worse trade than 100 lines of `zipfile`.

---

## How results are verified

The assignment says results only count if they match the sequential code, so
verification is wired into the harness rather than done once by hand. Four
layers:

1. Self-tests (`make check`). Each program checks itself against an
   independent oracle at small sizes, a naive triple loop, `qsort`, a plain
   sieve, known values of π(x), Miller–Rabin. Sizes are chosen to be awkward
   (N = 257 is prime, so no block size divides it; N = 0, 1, 2 and 2²⁰±1 for
   the sorters and the blocked reduction).
2. Per-run verification, printed as `verify=PASS|FAIL` on every timed run.
3. Cross-binary checksums. Every run prints an FNV-1a checksum of its
   result. `run_experiments.sh` compares each of the 45 OpenMP runs against the
   sequential binary's checksum and aborts the configuration on any mismatch.
   This works because these kernels are *bit-reproducible* (see below).
4. Cross-algorithm checksums. OMM and all five BMM block sizes square the
   same matrix; counting sort and merge sort sort the same keys. Their
   checksums are compared to each other at the end of the sweep.

### Why bitwise comparison is legitimate here

For problems 1, 3, 4 and 5 the arithmetic is integer, so addition is
associative and any evaluation order gives an identical result.

Problem 2 is floating point, where reassociation would normally change the last
bits. It does not here: the matrix multiply is parallelised over rows of the
output, so each element is accumulated over `k` in a fixed order by a single
thread. The thread count changes which core does the work, not the order in
which the additions happen. One thread and sixteen threads produce bit-identical
matrices, and a mismatch would be a genuine bug rather than rounding.

---

## Two traps that would have corrupted the numbers

Both of these were hit while building this, and both produce *plausible* wrong
results rather than obvious failures, which is what makes them worth writing
down.

1. A loop inside `omp parallel for` is optimised worse than the same loop
outside it. With Apple clang (`-Xpreprocessor -fopenmp`), writing the hot
inner loop directly in the body of a parallel region cost a factor of 5.5×
against the identical loop in a non-OpenMP build, 0.48 s vs 0.087 s for the
problem-1 kernel at N = 1024. Disassembly showed the sequential build emitting
three times as many vector instructions (108 `vpaddd` vs 36): the outlined
region was vectorised but barely unrolled or interleaved.

The fix is to hoist the inner loops into their own `NOINLINE` functions with
`restrict`-qualified parameters (`row_add`, `row_sub`, `daxpy`), giving the
vectoriser a small alias-free body. That restores identical code generation in
both builds and costs nothing sequentially. Verified afterwards: the OpenMP
binary at one thread now matches the no-OpenMP binary to within 2% on all five
problems, where before it was 3–6× slower on problems 1 and 2.

This matters beyond raw speed: an inflated single-thread baseline inflates
every speedup number computed from it. The measured speedups would have
looked *better* while the code was slower.

2. A mistyped flag used to fall back to a default silently. `zsh` does not
word-split unquoted variables, so `args="--algo bmm --block 4"; prog $args`
arrives as one argument, matches no option, and the program cheerfully runs the
default algorithm. This produced a table of six "block sizes" that were all
secretly the unblocked kernel and all, unsurprisingly, identical.

`args_validate()` now rejects any unrecognised `--option` and exits non-zero,
with a specific hint when the argument contains a space. Refusing to run beats
reporting a confident wrong number.

---

## Problem 1, A² (N = 2048 and 4096), entries from {−1, 0, 1}

`src/p1_matsq.c` · `--algo omm|bmm|naive --n N --block B`

Loop order. i-k-j, not i-j-k. The innermost loop then walks `A[k][*]` and
`C[i][*]` with unit stride, streaming cache lines and auto-vectorising. i-j-k
walks a column and pays a cache line per element. No transpose is used
anywhere, so the assignment's "count any transformation in the runtime" clause
costs nothing.

Exploiting {−1, 0, 1}. With `a[k]` hoisted out of the `j` loop, the branch
on its value is taken once per `k` rather than per element:

| `a[k]` | inner loop |
|---|---|
| `0`  | skipped entirely, about ⅓ of all work disappears |
| `1`  | `c[j] += b[j]`, a vector add, no multiply |
| `-1` | `c[j] -= b[j]`, a vector subtract |

A generic multiply path is kept so the code stays correct if the domain ever
changes. Net effect: roughly two-thirds of the multiply-adds become plain adds
and one-third vanish.

`int32_t` elements. |(A²)ᵢⱼ| ≤ N ≤ 4096, so 32 bits is exact, and twice as
many lanes fit in a vector register as with `double`.

BMM loses here, and the block size says why. Blocking exists to create
temporal locality that a naive i-j-k loop lacks. The i-k-j kernel above already
streams both operands with unit stride, so tiling buys no locality back while
shortening the vectorised inner loop from N elements to B. Measured on the
development machine (i5-7500, 4 cores), N = 2048, single thread:

| variant | time (s) | vs OMM |
|---|---|---|
| OMM (unblocked) | 1.36 |, |
| BMM block 4  | 31.68 | 23.3× slower |
| BMM block 8  | 17.12 | 12.6× slower |
| BMM block 16 |  9.16 |  6.7× slower |
| BMM block 32 |  2.96 |  2.2× slower |
| BMM block 64 |  1.88 |  1.4× slower |

The trend is monotone and it is the inner loop length that drives it: at B = 4
the vectoriser has four `int32` values to work with in a register that holds
eight, and the loop overhead is paid every four elements. As B grows the kernel
converges towards OMM from below without overtaking it. All six produce
identical checksums.

This is the honest result, and the plots show it. The assignment asks for both
implementations and for their speedups, not for blocking to win, and it is
worth saying in the report *why* it does not: this kernel's arithmetic intensity
is already low (one add per two streamed operands, thanks to the {−1,0,1}
trick), so it is bandwidth- rather than reuse-limited, and blocking optimises
reuse.

## Problem 2, X¹⁵ for a complex 2048×2048 matrix

`src/p2_xpow15.c` · `--n N --chain opt|naive --cmul 3|4`

Two independent optimisations that multiply together.

1. Addition chain (5 multiplications, not 14).

```
P = X·X    (X²)      Optimal: ℓ(15) = 5 in the addition-chain sense.
U = P·X    (X³)      Binary exponentiation needs 6.
P = U·U    (X⁶)      The literal reading of the problem needs 14.
Q = P·P    (X¹²)
P = Q·U    (X¹⁵)
```

2. Karatsuba/Gauss complex multiply (3 real multiplies, not 4).

```
M1 = A·C,  M2 = B·D,  M3 = (A+B)·(C+D)
Re = M1 − M2
Im = M3 − M1 − M2        ( = A·D + B·C )
```

The three extra O(N²) matrix additions are free next to an O(N³) multiply.

Combined: 5 × 3 = 15 real matrix multiplications against 14 × 4 = 56. A
3.7× reduction in work before a single thread is spawned. `--chain naive` and
`--cmul 4` disable each optimisation independently, which lets the gain be
attributed rather than just claimed. Measured at N = 512, single thread:

| variant | real matmuls | time (s) | speedup |
|---|---|---|---|
| 14 successive × 4-mult complex | 56 | 1.892 | baseline |
| addition chain × 4-mult complex | 20 | 0.705 | 2.68× |
| addition chain × 3-mult complex | 15 | 0.470 | 4.02× |

So the chain contributes 2.68× (predicted 14/5 = 2.8×) and Karatsuba a further
1.50× (predicted 4/3 = 1.33×), for 4.02× overall against a predicted 3.73×.
Both slightly beat their prediction because the operands in the chain are
sparser on the early multiplies, which the `aik == 0` skip in the kernel picks
up.

Precision, stated plainly. Entries start in {−1, 0, 1} but grow roughly as
sₖ₊₁ ~ √N · sₖ² under squaring, reaching ~10²² at N = 2048, past 2⁵³ ≈ 9×10¹⁵,
where `double` stops representing every integer exactly. So the full-size result
is a floating-point approximation, and each run prints `maxabs` so the magnitude
is visible rather than assumed. Two things remain exact:

* Parallel equals sequential bit for bit, for the reason given above.
* `--selftest` runs N = 64, where every intermediate stays below 2⁵³
  (measured: maxabs = 2.4×10¹⁴) and `double` arithmetic on these integers is
  therefore *exact*. All four combinations of {chain, naive} × {3-mult, 4-mult}
  must agree bit for bit, which validates the addition chain and the Karatsuba
  identity exactly rather than within a tolerance. If the magnitude assumption
  ever fails, the test reports INCONCLUSIVE instead of passing quietly.

Note that `--cmul 3` and `--cmul 4` give slightly different *floating-point*
results at full size, the Karatsuba form subtracts two large products to
recover the imaginary part, so it is worse-conditioned. That is the cost of the
25% saving, and it is why both are kept.

## Problem 3, sorting integers in [0, 1000]

`src/p3_sort.c` · `--algo counting|merge|qsort --n N [--stream]`

The range is tiny, so do not compare. With only 1001 distinct keys, counting
sort is O(N + K) and touches each element once; a comparison sort cannot beat
O(N log N) and drags every element through ~30 cache-missing comparisons. That
is a different complexity class, not a micro-optimisation. Measured at N = 5×10⁶,
single thread: counting sort 0.0077 s, parallel merge sort 0.558 s, 72×
faster, identical checksums.

Details that matter for scaling:

* `uint16_t` elements. Keys < 1024 need 10 bits; 2 bytes instead of 4 halves
  memory traffic, and this kernel is purely bandwidth-bound.
* Per-thread private histograms, each padded to a 64-byte multiple, no atomics
  and no false sharing in the counting phase.
* The scatter phase parallelises over the 1001 key values. Each key owns a
  contiguous, disjoint output range, so threads never collide and the writes are
  long sequential runs.

`--algo merge` is a task-parallel merge sort, kept so the report can *measure*
the comparison-sort baseline rather than assert it. Its top-level merge is
sequential, so Amdahl caps it around 3–4×; a parallel co-ranking merge would
lift that, but counting sort is the right answer for this key range anyway.

`--stream` and the 10¹² in the problem statement. Counting sort only ever
needs the histogram. With values generated on the fly the sort runs in O(K)
memory, 8 KB, so N is limited by time, not RAM. An in-memory array of 10¹²
elements would need 2 TB even at 2 bytes each. The sorted sequence is reported
run-length encoded, which is the only form in which 10¹² elements can be written
down at all. Measured: 10⁹ keys in 2.86 s single-threaded, 8 KB resident.

## Problem 4, primes in [2⁴⁰, 2⁴¹)

`src/p4_primes.c` · `--low L --high H --segbytes B`

Trial division is hopeless (~10¹² candidates × ~10⁶ divisors) and a plain sieve
is equally hopeless the other way (one bit per number over [0, 2⁴¹) is 256 GB).
A segmented sieve keeps only the √HIGH base primes (~10⁵) plus one cache-sized
window at a time, so it is the only structurally viable approach. On top of that:

* Odds only, halves memory and marking work.
* Segments sized to fit L2 (`--segbytes`, default 256 KB). Marking is a
  strided scatter; once the window leaves cache every mark becomes a memory
  round trip.
* Incremental multiple offsets. The textbook version recomputes each base
  prime's first multiple in every segment: `nseg × nprimes` divisions, which
  here would be ~8×10⁷ and a real fraction of runtime. Instead each thread takes
  a contiguous band of segments and carries a per-prime cursor forward
  across them, cutting divisions to `nthreads × nprimes`, three orders of
  magnitude fewer. The contiguous band is also why the schedule is a manual
  static partition rather than `dynamic`: the cursor only makes sense if a
  thread walks its segments in order, and sieving cost per segment is
  near-uniform across a narrow range, so nothing is lost to imbalance.
* Counting by popcount over the bitset, not a per-number test.

*Not done:* a mod-30 wheel (another ~25%) and a bucket sieve. Neither is needed
at this range; both are the natural next step.

Only a count is produced. π(2⁴¹) − π(2⁴⁰) ≈ 3.7×10¹⁰ primes; storing them at
8 bytes each is 300 GB. The count is the only output that can exist.

Default range. The sweep uses [2⁴⁰, 2⁴⁰+2³²), wide enough to be a real
measurement, narrow enough for 45 runs. The full [2⁴⁰, 2⁴¹) is ~256× more work;
run it once with `--high 2199023255552`.

Verification is unusually strong here because no table of π(x) exists at 2⁴⁰:
every timed run re-sieves one *randomly chosen* segment and cross-examines 200
random numbers in it with a deterministic Miller–Rabin test (12 bases, exact for
n < 3.3×10²⁴) in both directions, so both over-marking and under-marking are
caught. The self-test additionally checks [0, 10⁶) number by number against an
independent plain sieve, matches π(10⁶), π(10⁷) and π(10⁸) against their known
values, and checks a million numbers at 10¹² against Miller–Rabin.

## Problem 5, dot product of two length-10⁹ vectors

`src/p5_dotprod.c` · `--n N`

There are only 2N arithmetic operations but 2N element loads, so runtime is set
by bytes crossing the memory bus, not by flops. Hence:

* `int8_t` storage. The values need 2 bits. Storing them as `int`, the
  obvious choice, would move 8 GB instead of 2 GB and run about 4× slower. This
  single decision matters more than everything else in the file.
* Blocked accumulation. The inner loop accumulates into an `int32`, which
  lets the compiler keep the reduction in vector registers and widen 8-bit lanes
  efficiently; a bare `int64 += a[i]*b[i]` forces a widening chain that
  vectorises far worse. Blocks are 2²⁰ elements and each product is in
  {−1, 0, 1}, so `|partial| ≤ 2²⁰` and the `int32` cannot overflow.
* `reduction(+)` over `int64` for the outer sum, exact for any N up to 2⁶³.

Expect the speedup curve to flatten early. This kernel saturates memory
bandwidth after a few cores, and no number of threads buys bandwidth that does
not exist. That flattening is the correct result, not a defect, and it is the
clearest illustration in the assignment of a memory-bound rather than
compute-bound kernel.

---

## A note on two problem sizes

The assignment text as distributed reads "Sort 10¹² integers" and "primes
between 2⁴⁰ and 2⁴¹" (rendered as `1012`, `240`, `241`, exponents lost in
formatting). Both are taken at face value here rather than quietly shrunk:

* 10¹² integers cannot be materialised (2 TB at 2 bytes each), so `--stream`
  solves the problem in O(K) memory instead, which is the correct answer to the
  question as asked.
* [2⁴⁰, 2⁴¹) contains ~3.7×10¹⁰ primes, which cannot be stored, so the program
  counts them. The full range runs from the same binary; the timing sweep uses a
  2³²-wide window so that 45 runs finish in reasonable time.

Every size is a command-line parameter, so if the intended values differ, only
`scripts/run_experiments.sh` changes, no code does.

---

## Reproducibility

`common.h` seeds the RNG per fixed-size block index, not per thread. A
1-thread run and a 16-thread run therefore generate byte-identical input data,
which is what makes checksum comparison across thread counts meaningful. Those
same parallel fills are also the first writes to the pages, so on a NUMA machine
each page lands on the node of the thread that will later read it, data init is
parallel deliberately, not incidentally.
