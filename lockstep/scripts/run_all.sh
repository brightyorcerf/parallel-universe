#!/bin/sh
# Full experiment. Usage: scripts/run_all.sh [durationMs=10000] [runs=5]
# Resumable: a (impl, N, workload) file that already exists is skipped.
set -e
cd "$(dirname "$0")/.."
DUR=${1:-10000}; RUNS=${2:-5}
THREADS=1,2,4,6,8,10,12,14,16
mkdir -p build results/raw
javac -d build src/*.java
for impl in LazyDList LockFreeDList LockStack TreiberStack EliminationStack LockQueue MSQueue; do
  for n in 1000 10000; do
    for wl in 0C-50I-50D 50C-25I-25D 100C-0I-0D; do
      out=results/raw/${impl}_${n}_${wl}.csv
      [ -f "$out" ] && continue
      echo "$(date +%T) $impl N=$n $wl"
      java -Xms2g -Xmx2g -cp build Bench "$impl" "$n" "$wl" "$THREADS" "$RUNS" "$DUR" > "$out.tmp"
      mv "$out.tmp" "$out"
    done
  done
done
python3 scripts/analyze.py
PATH="$HOME/.gnuplot-env/bin:$PATH" gnuplot scripts/plots.gp
