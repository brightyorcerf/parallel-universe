"""raw/*.csv -> results/all_runs.csv, results/averages.csv, results/data/<impl>_<N>_<wl>.dat (for gnuplot)."""
import csv, glob, os, statistics
from collections import defaultdict

root = os.path.join(os.path.dirname(__file__), "..", "results")
rows = []
for f in sorted(glob.glob(os.path.join(root, "raw", "*.csv"))):
    rows += list(csv.reader(open(f)))

hdr = ["impl", "N", "workload", "threads", "run", "ops", "elapsed_ns", "mops"]
with open(os.path.join(root, "all_runs.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(hdr); w.writerows(rows)

groups = defaultdict(lambda: defaultdict(list))
for impl, n, wl, t, run, ops, ns, mops in rows:
    groups[(impl, n, wl)][int(t)].append(float(mops))

with open(os.path.join(root, "averages.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["impl", "N", "workload", "threads", "P1", "P2", "P3", "P4", "P5", "avg_mops", "stdev", "speedup"])
    for (impl, n, wl), by_t in sorted(groups.items()):
        base = statistics.mean(by_t[1])
        lines = []
        for t in sorted(by_t):
            p = by_t[t]; avg = statistics.mean(p); sd = statistics.stdev(p) if len(p) > 1 else 0.0
            w.writerow([impl, n, wl, t, *[f"{x:.4f}" for x in p], f"{avg:.4f}", f"{sd:.4f}", f"{avg / base:.4f}"])
            lines.append(f"{t} {avg:.6f} {sd:.6f} {avg / base:.6f}")
        with open(os.path.join(root, "data", f"{impl}_{n}_{wl}.dat"), "w") as d:
            d.write("# threads avg_mops stdev speedup\n" + "\n".join(lines) + "\n")
