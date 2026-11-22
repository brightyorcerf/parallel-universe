#!/usr/bin/env bash
# Linux only. Cache misses per million operations with `perf stat`.
# Counters are enabled only during the timed phase of each run (via perf's
# --control fds), so prefill and teardown are not counted. The bench's RESULT
# line is also saved, so one Linux sweep yields throughput as well.
#
# Usage: scripts/perf_cache.sh [seconds=10] [runs=5]
# Needs: perf (linux-tools-$(uname -r)), kernel.perf_event_paranoid <= 1
set -euo pipefail
cd "$(dirname "$0")/.."
SECS=${1:-10} RUNS=${2:-5}
command -v perf >/dev/null || { echo "perf not found: sudo apt install linux-tools-\$(uname -r)" >&2; exit 1; }
[[ -x bench ]] || make bench
OUT=results/perf_cache.csv THR=results/throughput_linux.csv
[[ -f $OUT ]] || echo "ds,workload,threads,total_ops,cache_misses,cache_refs,misses_per_mop,mean_mops" > "$OUT"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkfifo "$tmp/ctl" "$tmp/ack"
exec {ctl}<>"$tmp/ctl" {ack}<>"$tmp/ack"  # hold both ends open so no open() blocks

for ds in bst avl treap skiplist; do
  for wl in "100 0" "90 9" "50 25" "30 35" "0 50"; do
    set -- $wl
    name="$1C-$2I-$((100 - $1 - $2))D"
    for t in 1 2 4 6 8 10 12 14 16; do
      grep -q "^$ds,$name,$t," "$OUT" && continue
      echo "[$(date +%T)] $ds $name threads=$t" >&2
      res=$(PERF_CTL_FIFO="$tmp/ctl" PERF_ACK_FIFO="$tmp/ack" \
        perf stat -D -1 --control "fd:$ctl,$ack" -e cache-misses,cache-references -x, -o "$tmp/stat" \
        -- ./bench "$ds" "$t" "$1" "$2" "$SECS" "$RUNS" 2>/dev/null)
      echo "$res" >> "$THR"
      # hybrid CPUs report cpu_core/cache-misses/ and cpu_atom/... separately: sum them
      misses=$(awk -F, '$3 ~ /cache-misses/ {s += $1} END {print s + 0}' "$tmp/stat")
      refs=$(awk -F, '$3 ~ /cache-references/ {s += $1} END {print s + 0}' "$tmp/stat")
      ops=$(cut -d, -f7 <<< "$res") mops=$(cut -d, -f5 <<< "$res")
      awk -v d="$ds" -v w="$name" -v t="$t" -v o="$ops" -v m="$misses" -v r="$refs" -v mo="$mops" \
        'BEGIN { printf "%s,%s,%d,%.0f,%.0f,%.0f,%.1f,%s\n", d, w, t, o, m, r, m / (o / 1e6), mo }' >> "$OUT"
    done
  done
done
