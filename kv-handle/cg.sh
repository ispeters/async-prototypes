#!/bin/sh
# cg.sh <binary> : instructions and indirect branches per call, for every configuration
for c in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
  for path in hit miss; do
    for n in 1000 3000; do
      valgrind --tool=callgrind --branch-sim=yes --callgrind-out-file=/tmp/cg.$$ "$1" loop $c $path $n >/dev/null 2>/tmp/cg.log.$$
      eval "ir$n=$(grep 'Collected' /tmp/cg.log.$$ | awk '{print $4}')"
      eval "bi$n=$(grep 'Collected' /tmp/cg.log.$$ | awk '{print $7}')"
    done
    echo "$c $path $(( (ir3000 - ir1000) / 2000 )) $(( (bi3000 - bi1000) / 2000 ))"
  done
done
rm -f /tmp/cg.$$ /tmp/cg.log.$$
