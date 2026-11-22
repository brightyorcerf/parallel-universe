#!/usr/bin/env bash
# Linux only. Reproduces measurement trap 5: does perf undercount cache misses
# when two threads share one physical core (SMT siblings)?
# Runs two independent single-threaded AVL read-only benchmarks at the same time,
# pinned first to two separate cores, then to the two hyperthreads of one core,
# and prints cache misses per operation for each (timed phase only).
#
# Usage: scripts/smt_check.sh [seconds=4]   -> also writes results/smt_check.txt
set -euo pipefail
cd "$(dirname "$0")/.."
SECS=${1:-4}
command -v perf >/dev/null || { echo "perf not found" >&2; exit 1; }
[[ -x bench ]] || make bench

sib=$(cut -d, -f2 /sys/devices/system/cpu/cpu0/topology/thread_siblings_list | cut -d- -f2)
other=$(for c in /sys/devices/system/cpu/cpu[0-9]*; do n=${c##*cpu}; [[ $n != 0 && $n != "$sib" ]] && echo "$n"; done | sort -n | head -1)
[[ -n $sib && $sib != 0 ]] || { echo "cpu0 has no SMT sibling (SMT off?)" >&2; exit 1; }

# One pinned run under its own perf stat; prints "cpu misses_per_op".
run_pinned() {
  local d c a res ops misses
  d=$(mktemp -d); mkfifo "$d/ctl" "$d/ack"
  exec {c}<>"$d/ctl" {a}<>"$d/ack"
  res=$(PERF_CTL_FIFO="$d/ctl" PERF_ACK_FIFO="$d/ack" \
    perf stat -D -1 --control "fd:$c,$a" -e cache-misses -x, -o "$d/stat" \
    -- taskset -c "$1" ./bench avl 1 100 0 "$SECS" 1 2>/dev/null)
  ops=$(cut -d, -f7 <<< "$res")
  misses=$(awk -F, '$3 ~ /cache-misses/ {s += $1} END {print s + 0}' "$d/stat")
  awk -v cpu="$1" -v m="$misses" -v o="$ops" 'BEGIN { printf "cpu%s %.1f\n", cpu, m / o }'
  rm -rf "$d"
}

pair() {  # two concurrent runs on the given cpus
  local out; out=$(mktemp)
  run_pinned "$2" > "$out.a" & run_pinned "$3" > "$out.b" & wait
  echo "$1: $(cat "$out.a") | $(cat "$out.b")  (cache misses per op)"
  rm -f "$out" "$out.a" "$out.b"
}

{
  echo "SMT check, $(date -u +%F), $(lscpu | sed -n 's/^Model name: *//p'), cpu0 sibling = cpu$sib"
  echo "solo: $(run_pinned 0)  (cache misses per op)"
  pair "separate cores " 0 "$other"
  pair "sibling threads" 0 "$sib"
} | tee results/smt_check.txt
