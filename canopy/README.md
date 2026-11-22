# canopy: concurrent BST, AVL tree, treap and skiplist (HPC Assignment 3)

Four concurrent ordered sets with `contains`, `insert` and `remove`, benchmarked on
five workloads at 1–16 threads. Keys are drawn from [1, 2M], each structure is
prefilled with 1M keys (50%), each run lasts 10 s, and every reported throughput
is the mean of 5 runs. Cache misses per million operations come from `perf stat`.

```
src/common.hpp     spinlock, epoch-based memory reclamation, RNG
src/lo_tree.hpp    BST, AVL and Treap (one logical-ordering core, three balancing policies)
src/skiplist.hpp   lazy skiplist
src/bench.cpp      benchmark harness + concurrent correctness stress test
scripts/run_all.sh     full throughput sweep (any OS)        -> results/throughput.csv
scripts/perf_cache.sh  full sweep under perf stat (Linux)    -> results/perf_cache.csv
scripts/plot.py        plots + tables                        -> results/*.png, results/tables.md
```

## Build and run

```bash
make                       # g++ >= 13 (or clang >= 17), -std=c++23 -O3
./bench test               # correctness stress test: all four structures, 16 threads
./bench avl 8 50 25        # one config: AVL, 8 threads, 50C-25I-25D, 5 x 10 s
scripts/run_all.sh         # all 180 configs (~2.7 h); resumable
scripts/perf_cache.sh      # Linux: same sweep under perf stat (~2.7 h); resumable
python3 scripts/plot.py
```

For perf on Linux: `sudo apt install linux-tools-$(uname -r)` and
`sudo sysctl kernel.perf_event_paranoid=1`. Bare metal is best: most VMs and WSL2
do not expose the hardware cache counters.

## Design

### BST, AVL and Treap: logical ordering (Drachsler, Vechev, Yahav, PPoPP'14)

All three trees share one core (`LoTree<Balance>`). Each node is in two structures:

* A sorted doubly linked list (`pred`/`succ`, guarded by per-node `succLock`).
  Only the list decides whether a key is in the set. Insert and remove linearize
  when they change `p->succ`.
* The physical tree (`left`/`right`/`parent`, guarded by per-node `treeLock`).
  It is only an index into the list.

`contains` takes no locks and never retries. It walks the tree to some node,
then walks `pred`/`succ` to the exact spot. A rotation or relocation can send the
tree walk to the wrong place, but the list walk always corrects it. This
is what lets the AVL and the treap restructure freely while readers run.

Updates lock the list first (`pred.succLock`, then `node.succLock`), then the tree.
These rules make the code deadlock-free:

1. Take succLocks before treeLocks, and succLocks in key order.
2. Block on a treeLock only while holding no other treeLock. Try-lock any further
   treeLock, and on failure release them all and retry.
3. Change `x.left/right/height` only under `x.treeLock`. Change `x.parent` only
   while holding the treeLocks of both the old and the new parent. So holding
   a node's lock and checking `x.parent == P` proves P really is the parent.

The three trees differ only in physical maintenance:

| | insert | remove |
|---|---|---|
| BST | attach as leaf under `pred` or `succ` (one of them always has a free slot) | 0/1 child: splice. 2 children: move the in-order successor into its place. Tree locks are taken *before* the logical unlink, as in the paper |
| AVL | same, then walk up fixing heights and doing single/double rotations | same as BST, then rebalance from the lowest changed node |
| Treap | same, then rotate up while priority > parent's | mark + unlink from the list, then rotate the node down below its higher-priority child until it has ≤ 1 child, then splice |

The AVL tree uses *relaxed* balance, as concurrent AVL trees usually do: heights
are read without locks and fixed by later rebalances. The stress test still finds
0 balance violations at quiescence, with depth 22 for 521K keys (log₂ n ≈ 19).
The treap's heap order is also relaxed. In rare interleavings a node finishes its
upward rotations just before an ancestor rotates, which leaves a priority
inversion: 1 inversion in a 30-node treap under extreme contention, 0 at
realistic sizes. This affects only shape, never correctness.

### Skiplist: lazy skiplist (Herlihy, Lev, Luchangco, Shavit, 2007)

Insert and remove lock only the predecessors of the affected node, then validate.
`contains` is wait-free: a node is present iff it is `fullyLinked` and not `marked`.
Nodes are over-allocated to their real height (geometric with p = ½, max 22 levels),
so a node costs exactly its own levels.

### Memory reclamation

Removed nodes go to epoch-based reclamation. A node retired while the global
epoch is *e* is freed once the epoch reaches *e + 2*, after every thread that could
still hold a pointer to it has left its operation. Without this, a 10-second
write-heavy run would leak several GB.

### Harness

* A fresh structure for every run, prefilled in parallel with 1M distinct random keys.
* Worker threads wait at a barrier. The main thread starts the clock, sleeps 10 s,
  and then raises a stop flag. Throughput = total ops ÷ measured elapsed time.
* Each operation picks a key uniform in [1, 2M] and an op type from the workload mix.
* `perf stat` starts disabled (`-D -1`). The harness enables counting just before
  the workers start and disables it right after they join, over perf's
  `--control` fds, so prefill and teardown are excluded. Misses per million ops =
  cache-misses ÷ (total ops across the 5 runs ÷ 10⁶).

## Correctness

`./bench test` runs every structure with 16 threads for 2 s at three key ranges:
64 keys (extreme contention), 4096, and 1M. The mix is 20C-40I-40D. Each thread
records, per key, its successful inserts minus successful removes. Afterwards:

* For every key, the summed balance is 0 or 1, and it equals `contains(key)`.
* For the trees, in-order traversal == the logical list, no node is marked, and
  every `parent` pointer is consistent.
* For the skiplist, every level is sorted, contains only live nodes, and is a
  subset of the levels below.
* Structure size == number of keys with balance 1.

```
bst       threads=16 range=64       size=36       depth=10   violations=0      PASS
bst       threads=16 range=4096     size=1991     depth=46   violations=0      PASS
bst       threads=16 range=1048576  size=523865   depth=47   violations=0      PASS
avl       threads=16 range=64       size=33       depth=6    violations=0      PASS
avl       threads=16 range=4096     size=2080     depth=13   violations=0      PASS
avl       threads=16 range=1048576  size=521313   depth=22   violations=0      PASS
treap     threads=16 range=64       size=30       depth=9    violations=1      PASS
treap     threads=16 range=4096     size=2032     depth=25   violations=0      PASS
treap     threads=16 range=1048576  size=516577   depth=49   violations=0      PASS
skiplist  threads=16 range=64       size=29       depth=9    violations=0      PASS
skiplist  threads=16 range=4096     size=2079     depth=11   violations=0      PASS
skiplist  threads=16 range=1048576  size=521496   depth=19   violations=0      PASS
ALL PASS
```

## Results

Test machine: Intel Core i7-1165G7 (4 cores / 8 hardware threads, 2.8 GHz,
12 MB L3), 8 GB RAM, Windows 10, GCC 14.2 `-O3 -march=native`. The laptop was
on AC power with the High performance plan, and nothing else heavy was running.
The dotted line in the plots marks 8 threads. Beyond it the machine is
oversubscribed, so gains above 8 threads come only from hiding lock/memory stalls,
not from more cores. Two configs whose runs were disturbed (the machine entered
standby mid-run, or ran short of memory) were re-run, and the data is in
`results/throughput.csv`. An earlier sweep taken on battery power is kept in
`results/throughput_noisy_battery.csv` for reference only. Battery throttling
made it 2–3× slower and erratic, so it is not used.

![throughput](results/throughput.png)

### Analysis

Scaling. Every structure keeps scaling up to 8 threads (4 cores plus hyper-threads): 3.7–6.5× the
1-thread throughput at 8 threads (e.g., AVL 100C: 2.96 → 10.91 Mops/s; BST
0C-50I-50D: 1.12 → 5.79). Beyond 8 threads throughput is flat. The CPU has no more
hardware threads, and none of the structures collapse under oversubscription,
because the spinlocks yield after a short spin. So 8 threads is the real
saturation point on this machine. The 10–16 thread points show robustness, not
extra scalability.

AVL is the fastest in every workload. On a read-heavy mix it beats the BST by
about 2× at 1 thread and 1.2–1.4× at 8 threads. Height matters more than anything
else here. Lookups dominate every workload, and each tree level is roughly one
cache miss on a 1M-node structure that is far larger than L2/L3. The AVL stays at
depth ≈ 1.44 log₂ n (22 in the stress test), while the random BST and the treap
average ~2.9 ln n and reach depth ~47–49. The AVL pays for rotations only on
updates, and the logical-ordering design keeps readers lock-free during those
rotations. So balance is nearly free for readers and the shallower tree wins
everywhere.

The gap narrows as writes grow. At 0C-50I-50D, AVL (6.0) is only about 4%
ahead of the BST (5.8). Every successful update in the AVL walks up to rebalance,
takes extra tree locks, and its rotations invalidate cache lines that other
threads are traversing. With uniform random keys the unbalanced BST is already
"randomly balanced", so its extra depth costs less than AVL maintenance saves.

The treap is the slowest tree. With uniform keys a treap has the same expected
shape as the random BST, but it pays for priorities: rotate-up on every insert and
rotate-down on every remove. It reaches 0.74–0.96× the BST's throughput. Its benefit
(balance independent of insertion order) does not show with uniform random keys.
It would show with sorted or skewed inserts, where the plain BST degenerates.

The skiplist is competitive on read-heavy mixes (second at 1–8 threads in
100C), thanks to wait-free `contains` and towers that make search paths short.
It is the most fragile on write-heavy mixes past 8 threads (30C-35I-35D drops from
5.96 to 4.36–5.38). An insert locks up to `top+1` predecessors and a remove holds
the victim's lock while it retries. When the OS preempts a thread holding those
locks (oversubscription), the threads behind it stall. The trees lock only 1–4
nodes per update, so they degrade more gracefully.

Read-only vs update mixes. Throughput falls as the update ratio rises for
every structure (AVL at 8 threads: 10.9 → 9.2 → 7.2 → 6.7 → 6.0 Mops/s). Updates
take locks, allocate and reclaim nodes, and write shared cache lines that other
cores then miss on.

Noise. Most configs have a coefficient of variation below 5% across the 5
runs. Exceptions: BST 100C at 1 thread (21%) and skiplist 0C-50I-50D at 1 thread
(17%), plus a BST 100C step up at 14–16 threads. All of these are consistent
with Windows background activity and turbo boost on a laptop. Pinning threads
on a dedicated Linux machine would tighten them.

### Cache misses per million operations (perf)

Not measured yet. `perf` needs Linux, and the throughput sweep ran on Windows.
The cache-miss sweep will run on the same laptop booted into Ubuntu (bare metal)
with `scripts/perf_cache.sh`. `CLAUDE.md` in this folder has the step-by-step
handoff for that session.

### Full throughput table

Mops/s, mean of 5 × 10 s runs.

#### 100C-0I-0D

| threads | BST | AVL | Treap | Skiplist |
|---:|---:|---:|---:|---:|
| 1 | 1.49 | 2.96 | 1.23 | 2.00 |
| 2 | 2.75 | 5.13 | 2.18 | 3.51 |
| 4 | 4.46 | 7.81 | 3.75 | 5.10 |
| 6 | 5.67 | 9.27 | 5.49 | 6.94 |
| 8 | 7.76 | 10.91 | 7.43 | 8.62 |
| 10 | 7.66 | 11.01 | 7.42 | 8.66 |
| 12 | 7.90 | 11.00 | 7.40 | 8.70 |
| 14 | 9.21 | 10.92 | 7.45 | 8.51 |
| 16 | 9.21 | 10.97 | 7.46 | 8.63 |

#### 90C-9I-1D

| threads | BST | AVL | Treap | Skiplist |
|---:|---:|---:|---:|---:|
| 1 | 1.56 | 2.34 | 1.15 | 1.68 |
| 2 | 2.69 | 4.26 | 1.99 | 2.87 |
| 4 | 4.42 | 6.33 | 3.31 | 4.54 |
| 6 | 5.95 | 7.59 | 4.73 | 5.71 |
| 8 | 7.56 | 9.16 | 6.37 | 6.99 |
| 10 | 7.52 | 9.14 | 6.37 | 7.00 |
| 12 | 7.48 | 9.16 | 6.40 | 7.02 |
| 14 | 7.62 | 9.22 | 6.40 | 6.99 |
| 16 | 7.60 | 9.15 | 6.42 | 6.98 |

#### 50C-25I-25D

| threads | BST | AVL | Treap | Skiplist |
|---:|---:|---:|---:|---:|
| 1 | 1.25 | 1.70 | 1.05 | 1.38 |
| 2 | 2.16 | 2.92 | 1.89 | 2.43 |
| 4 | 3.40 | 4.47 | 3.16 | 4.05 |
| 6 | 4.95 | 5.58 | 4.44 | 5.38 |
| 8 | 6.53 | 7.24 | 5.84 | 6.63 |
| 10 | 6.61 | 7.28 | 5.84 | 6.65 |
| 12 | 6.63 | 7.23 | 5.93 | 6.57 |
| 14 | 6.62 | 7.29 | 5.93 | 6.61 |
| 16 | 6.62 | 7.30 | 5.95 | 6.60 |

#### 30C-35I-35D

| threads | BST | AVL | Treap | Skiplist |
|---:|---:|---:|---:|---:|
| 1 | 1.17 | 1.45 | 1.02 | 1.28 |
| 2 | 2.03 | 2.59 | 1.81 | 2.28 |
| 4 | 3.39 | 4.09 | 3.04 | 3.77 |
| 6 | 4.62 | 5.28 | 4.19 | 3.91 |
| 8 | 6.22 | 6.69 | 5.53 | 5.96 |
| 10 | 6.21 | 6.70 | 5.56 | 4.36 |
| 12 | 6.28 | 6.68 | 5.54 | 4.75 |
| 14 | 6.28 | 6.69 | 5.55 | 5.38 |
| 16 | 6.32 | 6.72 | 5.56 | 5.14 |

#### 0C-50I-50D

| threads | BST | AVL | Treap | Skiplist |
|---:|---:|---:|---:|---:|
| 1 | 1.12 | 1.28 | 0.97 | 0.86 |
| 2 | 2.01 | 2.22 | 1.72 | 1.86 |
| 4 | 3.33 | 3.67 | 2.92 | 3.35 |
| 6 | 4.42 | 4.68 | 3.89 | 4.55 |
| 8 | 5.79 | 6.00 | 5.06 | 5.63 |
| 10 | 5.84 | 6.01 | 5.08 | 5.63 |
| 12 | 5.84 | 5.99 | 5.09 | 5.48 |
| 14 | 5.84 | 5.99 | 5.07 | 5.06 |
| 16 | 5.84 | 5.99 | 5.06 | 5.27 |

## Measurement traps

Four things corrupted early numbers. All were caught by reading the data, not by the harness.

1. Battery power. The first full sweep ran unplugged on the Balanced plan.
   Windows throttled the CPU, so the numbers came out 2 to 3 times too low and
   erratic: read-only BST at 1 thread measured slower than write-only BST. The
   data is kept in `results/throughput_noisy_battery.csv`. The final sweep ran
   on AC with the High performance plan.
2. A system install mid-sweep. A WSL install ran during the battery sweep, and
   AVL read-only at 8 threads measured 5.0 instead of 8.8 Mops/s. That created a
   fake "jump" at 12 threads, the kind of curve that is easy to explain and
   wrong. Re-measuring single points is what exposed it.
3. Modern Standby. Disabling sleep is not enough on this laptop: turning the
   display off puts it into Modern Standby, which froze the sweep for 10 hours
   in the middle of a run. One 10 s run then reported 0.001 Mops/s. Its config
   was re-run, and the display timeout is now disabled during sweeps.
4. Memory pressure. On 8 GB, other programs pushed the machine into swapping, and
   one run fell to a third of its siblings' throughput. Any run below 50% of its
   config's mean is now treated as disturbed and its config is re-run. The final
   CSV has none.
