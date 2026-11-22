# canopy: handoff for the Linux session

Everything in Assignment 3 is done except the cache-miss measurement, which
needs `perf` on bare-metal Linux. The throughput sweep was run on Windows (same
laptop, booted into Windows) and is final: do not re-measure or overwrite
`results/throughput.csv`. Read README.md first; it explains the design, the
harness and how perf is wired in.

## Task: measure cache misses per million operations with perf

1. Toolchain and perf (needs the user's sudo password):
   ```sh
   sudo apt update
   sudo apt install -y g++ make python3-matplotlib linux-tools-common linux-tools-generic linux-tools-$(uname -r)
   sudo sysctl kernel.perf_event_paranoid=1
   ```
   `make` needs a compiler that accepts `-std=c++23` (GCC 11+; GCC 13+ preferred).
   If `g++ --version` is older, install `g++-13` and use `make CXX=g++-13`.

2. Build and check correctness on this machine:
   ```sh
   make && ./bench test        # must end with ALL PASS
   ```

3. Check that the hardware counters exist before the long sweep:
   ```sh
   perf stat -e cache-misses,cache-references ./bench avl 4 50 25 1 1
   ```
   Both counters must show numbers. If they show `<not supported>`, stop and
   tell the user: this is not bare metal or the PMU is disabled, so the sweep
   would record zeros.

4. Smoke-test the script on one short config, then delete its output:
   ```sh
   scripts/perf_cache.sh 1 1    # run in the background; stop it once results/perf_cache.csv has one data row
   cat results/perf_cache.csv   # misses_per_mop column must be > 0
   rm results/perf_cache.csv results/throughput_linux.csv
   ```

5. Full sweep. Ask the user which one; the default is the first.
   - `scripts/perf_cache.sh 10 1`: 1 run of 10 s per config, about 40 min.
   - `scripts/perf_cache.sh 10 5`: 5 runs per config, as the assignment says, about 2.7 h.

   Run it in the background. It is resumable: rerunning skips configs that are
   already in `results/perf_cache.csv`. Keep the laptop on AC power and awake
   (`systemd-inhibit --what=idle:sleep scripts/perf_cache.sh 10 1`). Don't run
   anything heavy alongside it.

6. Plots and tables:
   ```sh
   python3 scripts/plot.py      # adds results/cache_misses.png and a table to results/tables.md
   ```
   Look at `results/cache_misses.png` before writing anything.

7. Update README.md:
   - Replace the "Not measured yet" paragraph under "Cache misses per million
     operations (perf)" with the figure (`![cache misses](results/cache_misses.png)`),
     a short analysis and the cache-miss table from `results/tables.md`.
   - State the Linux machine (CPU, kernel, distro, compiler) and that perf counted
     only the timed phase of each run. The harness toggles counting through
     perf's `--control` fds.
   - Whether 1 or 5 runs per config were used.
   - Tie the analysis to the throughput results. Expectations to confirm or refute
     with the data: misses/op tracks tree depth (AVL lowest, BST and treap
     higher); the skiplist touches more nodes per search; update-heavy mixes add
     coherence misses as threads grow. Report what the numbers show, even when it
     contradicts these expectations.
   - Keep the repo's README style: no bold, no em dashes.
   - `results/throughput_linux.csv` is a by-product (throughput on Linux). Keep
     it; mention it in one line. Do not replace the Windows throughput numbers with it.

8. Commit and push from the repo root: Conventional Commits, imperative lowercase
   subject of at most 72 chars, body as `- ` bullets, no Claude attribution or
   co-author lines. For example:
   `chore(canopy): add perf cache-miss results` and
   `docs(canopy): add cache-miss analysis`.
   Then delete this CLAUDE.md in a final commit
   (`chore(canopy): remove linux handoff notes`), since its job is done.

## Gotchas

- 16 threads run on 8 hardware threads; that is intended (the assignment asks for 1 to 16).
- `perf stat -D -1 --control fd:...` needs perf 5.10 or newer. If perf rejects
  `--control`, drop `-D -1 --control ...` from `scripts/perf_cache.sh` and note
  in the README that prefill is then included in the counts.
- On hybrid Intel CPUs, perf reports `cpu_core/cache-misses/` and
  `cpu_atom/cache-misses/` separately. The script sums them.
