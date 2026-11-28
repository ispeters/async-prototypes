#!/bin/sh
# The coin scenarios (bench.cpp), in each mode: wall-clock nanoseconds per run (the best
# of REPS runs of N), and Callgrind's simulated mispredictions per run of
# conditional (Bcm) and indirect (Bim) branches. Callgrind's predictor is a
# simple model, not the machine's; the times are the machine's.
#
#   ./predict.sh [CXX [N [REPS]]]      e.g. ./predict.sh g++ 20000000 5
#
# STDEXEC as for cg.sh.
set -eu
CXX=${1:-g++}
N=${2:-20000000}
REPS=${3:-5}
DIR=$(cd "$(dirname "$0")" && pwd)
STDEXEC=${STDEXEC:-$(ls -d "$DIR"/../build/*/_deps/stdexec_30acdb0-src/include | head -1)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CG=10000

counts() {  # Bcm Bim from a Callgrind output file
  awk '/^events:/ { for (i = 2; i <= NF; i++) col[$i] = i - 1 }
       /^totals:/ { print $(col["Bcm"] + 1), $(col["Bim"] + 1) }' "$1"
}

printf '%-8s %-15s %8s %7s %7s\n' mode scenario ns/run Bcm/run Bim/run
for mode in 0 1 2 3; do
  name=$(echo EAGER ERASED TYPED VARIANT | cut -d' ' -f$((mode + 1)))
  "$CXX" -std=c++23 -O2 -DNDEBUG -static -DTH_MODE=$mode -isystem "$STDEXEC" \
    "$DIR/bench.cpp" "$DIR/remote.cpp" -o "$OUT/bench$mode"
  for s in coin_heads coin_alternate coin_random opaque_heads opaque_alternate opaque_random; do
    best=
    for r in $(seq "$REPS"); do
      t0=$(date +%s%N)
      "$OUT/bench$mode" "$s" "$N"
      t1=$(date +%s%N)
      t=$((t1 - t0))
      if [ -z "$best" ] || [ "$t" -lt "$best" ]; then best=$t; fi
    done
    for k in 1 3; do
      valgrind --tool=callgrind --branch-sim=yes \
        --callgrind-out-file="$OUT/cg.$k" "$OUT/bench$mode" "$s" $((k * CG)) \
        >/dev/null 2>&1
    done
    set -- $(counts "$OUT/cg.1") $(counts "$OUT/cg.3")
    awk -v m="$name" -v s="$s" -v n="$N" -v t="$best" -v c="$CG" \
      -v a="$1" -v b="$2" -v x="$3" -v y="$4" \
      'BEGIN { printf "%-8s %-15s %8.2f %7.2f %7.2f\n", m, s, t / n,
               (x - a) / (2 * c), (y - b) / (2 * c) }'
  done
done
