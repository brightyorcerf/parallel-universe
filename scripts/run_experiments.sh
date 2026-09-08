#!/usr/bin/env bash
# Runs every timed experiment and writes one CSV per configuration.
#
# For each configuration this script:
#   1. runs the *sequential* binary (compiled with no OpenMP at all) once, to
#      get a reference time and, more importantly, a reference result checksum;
#   2. runs the OpenMP binary REPS times at each thread count;
#   3. rejects the whole configuration if any run reports verify=FAIL, or if any
#      run's result checksum differs from the sequential one. A wrong answer is
#      not a slow answer, and a timing table built on one is worthless.
#
# Usage:
#   scripts/run_experiments.sh              full sweep (hours; lab machine)
#   scripts/run_experiments.sh --quick      small sizes, 2 reps, 1/2/4 threads
#
# Override anything: THREADS="1 2 4" REPS=3 scripts/run_experiments.sh
set -euo pipefail

cd "$(dirname "$0")/.."
BIN=bin
OUT=${OUT:-results}

QUICK=0
[[ "${1:-}" == "--quick" ]] && QUICK=1

if (( QUICK )); then
    THREADS=${THREADS:-"1 2 4"}
    REPS=${REPS:-2}
    P1_SMALL=512;  P1_LARGE=768
    P2_N=256
    P3_N=5000000
    P3_MERGE_N=5000000
    P4_LOW=1099511627776
    P4_HIGH=$((1099511627776 + 268435456))     # 2^40 .. 2^40+2^28
    P5_N=20000000
else
    THREADS=${THREADS:-"1 2 4 6 8 10 12 14 16"}
    REPS=${REPS:-5}
    P1_SMALL=2048; P1_LARGE=4096
    P2_N=2048
    P3_N=1000000000                            # 10^9 keys at 2 bytes: 2 GB in + 2 GB out
    # Merge sort needs a third 2 GB scratch buffer on top of that, so it gets a
    # smaller N. The two sorts are separate curves; the self-test is what
    # compares them against each other at identical N.
    P3_MERGE_N=200000000
    P4_LOW=1099511627776
    P4_HIGH=2199023255552                       # 2^40 .. 2^41 (full assigned range)
    P5_N=1000000000
fi

mkdir -p "$OUT"

# Bind threads to cores. Without this the OS is free to migrate threads between
# runs and the 5 repetitions measure scheduler luck as much as the code.
export OMP_PROC_BIND=${OMP_PROC_BIND:-close}
export OMP_PLACES=${OMP_PLACES:-cores}

field() { sed -n "s/.*[[:space:]]$1=\([^[:space:]]*\).*/\1/p" <<<"$2"; }

# measure <csv-name> <program> [args...]
measure() {
    local name=$1 prog=$2; shift 2
    local csv="$OUT/$name.csv" meta="$OUT/$name.meta"
    local line seq_time seq_sum t sum verify

    echo "=== $name : $prog $*" >&2

    line=$("$BIN/${prog}_seq" "$@")
    seq_time=$(field time "$line")
    seq_sum=$(field sum "$line")
    if [[ "$(field verify "$line")" != PASS ]]; then
        echo "!! $name: sequential run failed verification, skipping" >&2
        return 1
    fi

    { echo "program=$prog"
      echo "args=$*"
      echo "seq_time=$seq_time"
      echo "seq_sum=$seq_sum"
      echo "reps=$REPS"
      echo "host=$(uname -n) $(uname -s) $(uname -m)"
      echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$meta"

    { printf 'threads'
      for ((r = 1; r <= REPS; r++)); do printf ',run%d' "$r"; done
      printf ',avg,speedup\n'
    } > "$csv"

    local base="" avg
    for th in $THREADS; do
        local times=() total=0
        for ((r = 1; r <= REPS; r++)); do
            line=$("$BIN/$prog" --threads "$th" "$@")
            t=$(field time "$line"); sum=$(field sum "$line"); verify=$(field verify "$line")
            if [[ "$verify" != PASS ]]; then
                echo "!! $name t=$th rep=$r: verify=FAIL" >&2; return 1
            fi
            if [[ "$sum" != "$seq_sum" ]]; then
                echo "!! $name t=$th rep=$r: checksum $sum != sequential $seq_sum" >&2; return 1
            fi
            times+=("$t")
            total=$(awk -v a="$total" -v b="$t" 'BEGIN{printf "%.6f", a+b}')
        done
        avg=$(awk -v s="$total" -v n="$REPS" 'BEGIN{printf "%.6f", s/n}')
        [[ -z "$base" ]] && base=$avg
        local sp
        sp=$(awk -v b="$base" -v a="$avg" 'BEGIN{printf "%.4f", (a>0)? b/a : 0}')
        printf '%s,%s,%s,%s\n' "$th" "$(IFS=,; echo "${times[*]}")" "$avg" "$sp" >> "$csv"
        printf '  threads=%-3s avg=%-12s speedup=%s\n' "$th" "$avg" "$sp" >&2
    done
}

echo "--- correctness gates ---" >&2
make -s check >&2

echo "--- problem 1: A^2 ---" >&2
for n in "$P1_SMALL" "$P1_LARGE"; do
    measure "p1_omm_n${n}" p1_matsq --algo omm --n "$n"
    for b in 4 8 16 32 64; do
        measure "p1_bmm_n${n}_b${b}" p1_matsq --algo bmm --block "$b" --n "$n"
    done
done

echo "--- problem 2: X^15 ---" >&2
measure "p2_chain_n${P2_N}"       p2_xpow15 --n "$P2_N" --chain opt   --cmul 3
measure "p2_naive14_n${P2_N}"     p2_xpow15 --n "$P2_N" --chain naive --cmul 4

echo "--- problem 3: sorting ---" >&2
measure "p3_counting_n${P3_N}"       p3_sort --algo counting --n "$P3_N"
measure "p3_merge_n${P3_MERGE_N}"    p3_sort --algo merge    --n "$P3_MERGE_N"

echo "--- problem 4: primes ---" >&2
# This is the longest configuration in the sweep by a wide margin. Under
# `set -e` a single failed rep here would abort the script and problem 5 would
# never run, so its failure is recorded and stepped over instead. Every other
# configuration still aborts the sweep, which is what we want -- this one is
# the exception because of what it costs to redo.
measure "p4_primes" p4_primes --low "$P4_LOW" --high "$P4_HIGH" \
    || echo "!! p4 failed verification -- continuing so later problems still run" >&2

echo "--- problem 5: dot product ---" >&2
measure "p5_dot_n${P5_N}" p5_dotprod --n "$P5_N"

# Different algorithms for the same problem must agree with each other, not just
# with their own sequential twin. OMM and every BMM block size square the same
# matrix, and integer arithmetic makes those results bit-identical, so one
# differing checksum is a real bug. (The two sorts run at different N here, so
# their cross-comparison lives in `make check` instead.)
cross_check() {
    local label=$1; shift
    local first="" name sum
    for name in "$@"; do
        [[ -f "$OUT/$name.meta" ]] || continue
        sum=$(sed -n 's/^seq_sum=//p' "$OUT/$name.meta")
        if [[ -z "$first" ]]; then first=$sum
        elif [[ "$sum" != "$first" ]]; then
            echo "!! $label: $name checksum $sum != $first" >&2
            return 1
        fi
    done
    echo "cross-check $label: all algorithms agree ($first)" >&2
}

echo "--- cross-algorithm agreement ---" >&2
for n in "$P1_SMALL" "$P1_LARGE"; do
    cross_check "p1 n=$n" "p1_omm_n${n}" \
        "p1_bmm_n${n}_b4" "p1_bmm_n${n}_b8" "p1_bmm_n${n}_b16" \
        "p1_bmm_n${n}_b32" "p1_bmm_n${n}_b64"
done

echo >&2
echo "CSV files written to $OUT/" >&2
echo "Next: python3 scripts/make_excel.py && plots/plot_all.sh" >&2
