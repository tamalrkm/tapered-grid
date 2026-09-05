#!/bin/bash
# Chained: in-RAM suite (all modes, fixed pack) -> 22 GB out-of-core build -> locality walks.
cd "$(dirname "$0")"
out=results_oahash.txt
: > $out
echo "# $(date -Is)  $(lscpu | grep 'Model name' | sed 's/.*: *//')  THP=$(cat /sys/kernel/mm/transparent_hugepage/enabled)" >> $out
echo "## head-to-head B=16 d=13 M=3M seed=42" >> $out
for m in rankonly tapered tapered-ram tapered-ram-nohuge hash oahash oahash-nohuge; do ./bench $m 16 13 3000000 42 >> $out 2>&1; done
echo "## fixed d=10 sweep, M=2M seed=42" >> $out
for B in 10 14 18 22; do for m in rankonly tapered tapered-ram hash oahash; do ./bench $m $B 10 2000000 42 >> $out 2>&1; done; done
rm -f memo.bin
echo "DONE-SUITE $(date -Is)" >> $out
ooc=results_walk.txt
: > $ooc
echo "# $(date -Is)" >> $ooc
./ooc_bench build 22 15 >> $ooc 2>&1
./ooc_bench walk 22 15 20000 42 lex >> $ooc 2>&1
./ooc_bench walk 22 15 20000 42 hashed >> $ooc 2>&1
./ooc_bench walk 22 15 20000 7 lex >> $ooc 2>&1
rm -f ooc_memo.bin
echo "DONE-WALK $(date -Is)" >> $ooc
