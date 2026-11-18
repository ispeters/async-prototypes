#!/bin/sh
# Instructions and indirect branches per get, from callgrind: the difference
# between runs of 1000 and 3000 gets. usage: cg.sh <statically linked bench>
bin=$1
count() {  # prints "Ir Bi" totals
  valgrind --tool=callgrind --branch-sim=yes --callgrind-out-file=/tmp/cg.out "$bin" loop "$@" >/dev/null 2>/tmp/cg.err || { cat /tmp/cg.err >&2; exit 1; }
  awk '/^events:/ { for (i = 2; i <= NF; i++) col[$i] = i - 1 } /^totals:|^summary:/ { print $(col["Ir"] + 1), $(col["Bi"] + 1); exit }' /tmp/cg.out
}
printf '%-8s %-8s %-8s %14s %18s\n' column root path instructions 'indirect branches'
for root in each nested; do for path in hit miss suspend; do for col in s c m; do
  [ $col = s ] && [ $path = suspend ] && continue
  set -- $(count $col $root $path 1000); a=$1; ab=$2
  set -- $(count $col $root $path 3000); b=$1; bb=$2
  printf '%-8s %-8s %-8s %14s %18s\n' $col $root $path $(( (b - a) / 2000 )) $(( (bb - ab) / 2000 ))
done; done; done
