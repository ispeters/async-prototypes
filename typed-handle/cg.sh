#!/bin/sh
# Instructions (Ir) and indirect branches (Bi) per run of each scenario, in
# each mode, under Callgrind: the difference between N and 3N runs, over 2N.
# For `repeat`, `repeat_checked`, `scheduled` and `hopped` the numbers are per
# iteration.
#
#   ./cg.sh [CXX [N]]      e.g. ./cg.sh g++ 1000
#   SCENARIOS="let both" ./cg.sh    only these
#
# STDEXEC is stdexec's include directory at the pinned commit (30acdb0);
# by default, the copy a CMake preset fetched under build/.
set -eu
CXX=${1:-g++}
N=${2:-1000}
DIR=$(cd "$(dirname "$0")" && pwd)
STDEXEC=${STDEXEC:-$(ls -d "$DIR"/../build/*/_deps/stdexec_30acdb0-src/include | head -1)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

totals() {  # Ir Bi from a Callgrind output file
  awk '/^events:/ { for (i = 2; i <= NF; i++) col[$i] = i - 1 }
       /^totals:/ { print $(col["Ir"] + 1), $(col["Bi"] + 1) }' "$1"
}

printf '%-8s %-16s %10s %6s\n' mode scenario Ir/run Bi/run
for mode in 0 1 2 3; do
  name=$(echo EAGER ERASED TYPED VARIANT | cut -d' ' -f$((mode + 1)))
  "$CXX" -std=c++23 -O2 -DNDEBUG -static -DTH_MODE=$mode -isystem "$STDEXEC" \
    "$DIR/bench.cpp" "$DIR/remote.cpp" -o "$OUT/bench$mode"
  for s in ${SCENARIOS:-chain4 chain4_throwing let both boundary repeat chain4_unstoppable chain4_checked both_live both_checked boundary_checked repeat_checked scheduled hopped remote remote_checked}; do
    for k in 1 3; do
      valgrind --tool=callgrind --branch-sim=yes \
        --callgrind-out-file="$OUT/cg.$k" "$OUT/bench$mode" "$s" $((k * N)) \
        >/dev/null 2>&1
    done
    set -- $(totals "$OUT/cg.1") $(totals "$OUT/cg.3")
    awk -v m="$name" -v s="$s" -v n="$N" -v a="$1" -v b="$2" -v c="$3" -v d="$4" \
      'BEGIN { printf "%-8s %-16s %10.1f %6.2f\n", m, s, (c - a) / (2 * n), (d - b) / (2 * n) }'
  done
done
