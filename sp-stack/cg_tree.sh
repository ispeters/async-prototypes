#!/bin/sh
# Instructions per call of the tree walk, from callgrind: the difference
# between 2 and 6 walks of 131071 calls. usage: cg_tree.sh <statically linked tree binary>
bin=$1
ir() {
  valgrind --tool=callgrind --callgrind-out-file=/tmp/cgt.out "$bin" loop "$@" >/dev/null 2>/tmp/cgt.err || { cat /tmp/cgt.err >&2; exit 1; }
  awk '/^totals:|^summary:/ { print $2; exit }' /tmp/cgt.out
}
for col in s c m; do
  a=$(ir $col 2); b=$(ir $col 6)
  awk -v c=$col -v a=$a -v b=$b 'BEGIN { printf "%s %.1f\n", c, (b - a) / (4 * 131071) }'
done | tr '\n' ' '; echo
