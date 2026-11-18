#!/bin/sh
# cg.sh <binary> : instructions and indirect branches per call (the difference
# between 1000 and 3000 calls), for every variant (-1 = synchronous) and workload
for w in 0 1 2 3 4 5 6 7; do
  for v in -1 0 1 2 3 4 5 6 7 8 9; do
    if [ "$v" = -1 ]; then case $w in 0|1|2|6) ;; *) continue ;; esac; fi
    for n in 1000 3000; do
      valgrind --tool=callgrind --branch-sim=yes --callgrind-out-file=/tmp/cg.$$ "$1" loop $v $w $n >/dev/null 2>/tmp/cg.log.$$
      eval "ir$n=$(grep 'Collected' /tmp/cg.log.$$ | awk '{print $4}')"
      eval "bi$n=$(grep 'Collected' /tmp/cg.log.$$ | awk '{print $7}')"
    done
    echo "$w $v $(( (ir3000 - ir1000) / 2000 )) $(( (bi3000 - bi1000) / 2000 ))"
  done
done
rm -f /tmp/cg.$$ /tmp/cg.log.$$
