"""Plots + markdown tables from results/throughput.csv and (if present) results/perf_cache.csv.

Usage: py scripts/plot.py   (or python3 on Linux)
"""
import csv
import pathlib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = pathlib.Path(__file__).resolve().parent.parent
RES = ROOT / "results"
DS = ["bst", "avl", "treap", "skiplist"]
LABEL = {"bst": "BST", "avl": "AVL", "treap": "Treap", "skiplist": "Skiplist"}
COLOR = {"bst": "#2a78d6", "avl": "#eb6834", "treap": "#1baf7a", "skiplist": "#eda100"}
MARKER = {"bst": "o", "avl": "s", "treap": "^", "skiplist": "D"}
WORKLOADS = ["100C-0I-0D", "90C-9I-1D", "50C-25I-25D", "30C-35I-35D", "0C-50I-50D"]
THREADS = [1, 2, 4, 6, 8, 10, 12, 14, 16]


def load(path, key_cols, val_col):
    data = {}
    if not path.exists():
        return data
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if row and row[0] == "RESULT":
                row = row[1:]
            if len(row) <= max(key_cols + [val_col]) or row[0] == "ds" or row[0] == "tag":
                continue
            ds, wl, t = (row[i] for i in key_cols)
            data[(ds, wl, int(t))] = float(row[val_col])
    return data


def plot(data, ylabel, title, fname):
    fig, axes = plt.subplots(1, 5, figsize=(22, 4.6), sharey=False)
    for ax, wl in zip(axes, WORKLOADS):
        for ds in DS:
            pts = [(t, data[(ds, wl, t)]) for t in THREADS if (ds, wl, t) in data]
            if not pts:
                continue
            xs, ys = zip(*pts)
            ax.plot(xs, ys, color=COLOR[ds], marker=MARKER[ds], markersize=6, linewidth=2, label=LABEL[ds])
        ax.set_title(wl, fontsize=12)
        ax.set_xlabel("threads")
        ax.set_xticks(THREADS)
        ax.set_ylim(bottom=0)
        ax.grid(color="#e4e4e2", linewidth=0.8)
        ax.set_axisbelow(True)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        ax.axvline(8, color="#9a9a96", linewidth=1, linestyle=":")  # hardware threads on the test machine
    axes[0].set_ylabel(ylabel)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=4, frameon=False, fontsize=11, bbox_to_anchor=(0.5, 1.02))
    fig.suptitle(title, y=1.09, fontsize=14)
    fig.tight_layout()
    fig.savefig(RES / fname, dpi=130, bbox_inches="tight")
    plt.close(fig)


def table(data, fmt):
    out = []
    for wl in WORKLOADS:
        out.append(f"\n#### {wl}\n\n| threads | " + " | ".join(LABEL[d] for d in DS) + " |")
        out.append("|---:|" + "---:|" * len(DS))
        for t in THREADS:
            cells = [fmt.format(data[(d, wl, t)]) if (d, wl, t) in data else "-" for d in DS]
            out.append(f"| {t} | " + " | ".join(cells) + " |")
    return "\n".join(out)


if __name__ == "__main__":
    md = []
    thr = load(RES / "throughput.csv", [0, 1, 2], 3)
    if thr:
        plot(thr, "throughput (Mops/s, mean of 5 runs)", "Throughput vs threads", "throughput.png")
        md.append("## Throughput (Mops/s, mean of 5 x 10 s runs)\n" + table(thr, "{:.2f}"))
    perf = load(RES / "perf_cache.csv", [0, 1, 2], 6)
    if perf:
        plot(perf, "cache misses per million ops", "Cache misses per million operations (perf)", "cache_misses.png")
        md.append("## Cache misses per million operations\n" + table(perf, "{:,.0f}"))
    (RES / "tables.md").write_text("\n\n".join(md) + "\n")
    print(f"wrote {len(thr)} throughput points, {len(perf)} perf points")
