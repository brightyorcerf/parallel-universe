# Why LockStack dips at two threads

In the main sweep, `LockStack` (optimistic read of `top`, then a non-fair
`ReentrantLock`, validate, publish) falls from 30.7 MOPS at one thread to 8.4 at
two, then recovers to 19.5 at four. `TreiberStack` shows no such dip, so the
suspect is the lock rather than contention on `top`.

Hypothesis: with two threads, nearly every unlock has a parked waiter to wake.
With more, the non-fair lock lets a running thread barge in and take the lock
before the woken one is scheduled, so most handoffs skip the wake-up.

Three variants test it, all identical to `LockStack` except for the lock:

| variant | change | prediction if the hypothesis holds |
|---|---|---|
| `LockStackFair` | `new ReentrantLock(true)`: no barging | every handoff wakes someone, so it collapses |
| `LockStackSpin` | test-and-test-and-set spin lock: nobody parks | no dip at two threads |
| `LockStackCount` | counts failed validations | rules the optimistic-retry explanation in or out |

## Results

N = 1,000, 50% push / 50% pop, mean of 5 runs of 2 s, 4-core i5-7500, JDK 17,
macOS, under `caffeinate -dims`. Raw rows in `results/sweep.csv`.

| threads | LockStack | Fair | Spin | Count | Treiber |
|---|---|---|---|---|---|
| 1 | 29.2 | 29.4 | 33.8 | 26.1 | 57.3 |
| 2 | 8.5 | 0.2 | 13.4 | 9.6 | 14.3 |
| 3 | 19.9 | 0.1 | 8.2 | 16.1 | 10.9 |
| 4 | 20.5 | 0.1 | 6.2 | 16.0 | 9.8 |
| 6 | 19.9 | 0.0 | 4.6 | 15.8 | 9.5 |
| 8 | 19.3 | 0.0 | 3.5 | 16.0 | 9.8 |

- Forbidding barging (fair lock) collapses throughput to 0.2 MOPS at two threads
  and below 0.1 from three: wake-ups are the expensive part.
- Removing parking (spin lock) removes the dip: 13.4 MOPS at two threads. It
  never recovers either, since spinning only gets worse with more threads. So
  the recovery from three threads up comes from parked threads staying parked.
- Failed validations are 7.5% of lock acquisitions at two threads and 0.7% from
  three up. Ten times higher, but far too small to explain a 3.5x drop on their
  own; it fits strict alternation at two threads and runs of consecutive
  acquisitions by one barging thread at three or more.
- JFR (`jdk.ThreadPark`, threshold 0) counts about 0.15 parks per operation at
  two threads against 0.023 at four, roughly 6.6 times as many
  (`results/parks.txt`). JFR's own overhead lowers throughput in those runs.

Open: why the switch happens between two and three threads, not gradually.

## Notes

- `results/sweep.csv` was produced with the main harness's 4-thread JIT warm-up.
  This directory's `Bench` was then changed to warm up at the first measured
  thread count, so that a park recording only contains its own configuration.
  `run.sh` uses the changed `Bench` for both, so a rerun of the sweep warms up at
  one thread.
- Four cores means six and eight threads are oversubscribed; those rows mix lock
  behaviour with OS scheduling.

`./run.sh` reruns everything.
