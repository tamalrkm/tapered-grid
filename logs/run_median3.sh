#!/bin/bash
# Three repetitions of the in-RAM suite; medians go into the paper's Tables 2-3.
cd "$(dirname "$0")"
out=results_median3.txt
: > $out
echo "# $(date -Is)  load=$(cut -d' ' -f1-3 /proc/loadavg)  THP=$(cat /sys/kernel/mm/transparent_hugepage/enabled)" >> $out
for rep in 1 2 3; do
  echo "## rep $rep  head-to-head B=16 d=13 M=3M" >> $out
  for m in rankonly tapered tapered-ram tabled hash oahash; do ./bench $m 16 13 3000000 42 >> $out 2>&1; done
  echo "## rep $rep  sweep d=10 M=2M" >> $out
  for B in 10 14 18 22; do for m in tapered-ram hash oahash; do ./bench $m $B 10 2000000 42 >> $out 2>&1; done; done
  rm -f memo.bin
done
echo "DONE $(date -Is)" >> $out
