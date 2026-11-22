#!/usr/bin/env bash
# Full throughput sweep. Resumable: configs already in the CSV are skipped.
# Usage: scripts/run_all.sh [seconds=10] [runs=5]
set -euo pipefail
cd "$(dirname "$0")/.."
SECS=${1:-10} RUNS=${2:-5}
BIN=./bench; [[ -x bench.exe ]] && BIN=./bench.exe
OUT=results/throughput.csv
[[ -f $OUT ]] || echo "tag,ds,workload,threads,mean_mops,stdev_mops,total_ops,run1,run2,run3,run4,run5" > "$OUT"
for ds in bst avl treap skiplist; do
  for wl in "100 0" "90 9" "50 25" "30 35" "0 50"; do
    set -- $wl
    name="$1C-$2I-$((100 - $1 - $2))D"
    for t in 1 2 4 6 8 10 12 14 16; do
      grep -q "^RESULT,$ds,$name,$t," "$OUT" && continue
      echo "[$(date +%T)] $ds $name threads=$t" >&2
      $BIN "$ds" "$t" "$1" "$2" "$SECS" "$RUNS" 2>/dev/null >> "$OUT"
    done
  done
done
