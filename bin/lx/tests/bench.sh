#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Interpreter speed, natively: guest kernels from guest/bench.c, run alternately on engine
# variants, best of N runs each. Prints one line per (variant, kernel): best microseconds.
#   sh bench.sh OUTDIR [rounds]
# OUTDIR must hold: bench-native, bench-rv (guest), and engine builds lx-<variant>.
# Variants built by this script: default (tail-call dispatch), switch (same binary,
# --dispatch=switch), nofuse, memcheck0, memcheck2, softfp.
set -eu
O=$1; N=${2:-3}
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$(dirname "$HERE")
CF="-O2 -Wall -Wno-unused-parameter -Wno-format-truncation -ffp-contract=off -fno-math-errno"
[ -x "$O/lx-memcheck0" ] || nice -n 19 cc $CF -DLX_MEMCHECK=0 -o "$O/lx-memcheck0" "$SRC"/*.c
[ -x "$O/lx-memcheck2" ] || nice -n 19 cc $CF -DLX_MEMCHECK=2 -o "$O/lx-memcheck2" "$SRC"/*.c
[ -x "$O/lx-default" ] || nice -n 19 cc $CF -o "$O/lx-default" "$SRC"/*.c
for k in sieve sort sha256 float fib; do
  i=0
  while [ $i -lt "$N" ]; do
    printf 'native %s ' $k; nice -n 19 "$O/bench-native" $k 1 | awk '{print $3}'
    for v in "default" "default --dispatch=switch" "default --no-fuse" "memcheck0" "memcheck2" "default --soft-fp"; do
      set -- $v
      b=$1; shift
      printf '%s %s ' "$(echo "$v" | tr ' ' '_')" $k
      timeout 55 nice -n 19 "$O/lx-$b" "$@" "$O/bench-rv" $k 1 | awk '{print $3}'
    done
    i=$((i+1))
  done
done | awk '{ if (!($1" "$2 in best) || $3 < best[$1" "$2]) best[$1" "$2] = $3 } END { for (k in best) print k, best[k] }' | sort
