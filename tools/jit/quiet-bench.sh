#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The translator's final measurements, taken only on a quiet machine: before every piece it
# waits until the 1-minute load average is below $MAXLOAD (default 3), runs at nice 19, and
# keeps each invocation short. Results go to $B/jit/results/quiet-*.txt with the load noted.
#   sh tools/jit/quiet-bench.sh build-NAME [programs...]
B=${1:-build}; shift
P=${*:-"sieve sort sha256 fib matmul strloop indirect switch"}
MAXLOAD=${MAXLOAD:-3}
PARTS=${PARTS:-"node chrome ucos"}   # which parts to run
SETS=${SETS:-"interp,predecode block,dispatch,struct dispatch+calls,struct+calls,aot"}
TAG=${TAG:-}
R=$B/jit/results
mkdir -p "$R"
quiet() { until awk -v m="$MAXLOAD" '{exit !($1 < m)}' /proc/loadavg; do sleep 20; done; }
for p in $P; do
  # Node, standalone engine: interpreters, then the translated configurations, then chained variants
  case "$PARTS" in *node*) ;; *) SETS="";; esac
  for set in $SETS; do
    quiet
    echo "== $p [$set] load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$R/quiet-node$TAG.txt"
    rounds=2; [ "$set" = "interp,predecode" ] && rounds=1
    timeout 150 nice -n 19 node tools/jit/bench.mjs --build="$B" --programs="$p" --configs="$set" --rounds=$rounds --reps=5 >> "$R/quiet-node$TAG.txt" 2>&1
  done
  # Chromium: the main configurations
  case "$PARTS" in *chrome*)
  quiet
  echo "== $p load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$R/quiet-chrome$TAG.txt"
  timeout 180 nice -n 19 node tools/jit/chrome.mjs --page=tools/jit/bench.html --timeout=170000 \
    --query="build=$B&programs=$p&configs=interp,predecode,struct+calls,aot&rounds=1&reps=5" >> "$R/quiet-chrome$TAG.txt" 2>&1
  ;; esac
  # Inside UC.OS under Node: rvjit translated, rvjit interpreting, and the core engine's interpreter (lx)
  case "$PARTS" in *ucos*) ;; *) continue;; esac
  for run in "/bin/rvjit:repeat=3 /usr/share/jit/$p.elf" "/bin/rvjit:mode=interp /usr/share/jit/$p.elf" "/bin/rvjit:mode=pre repeat=2 /usr/share/jit/$p.elf" "/bin/lx:/usr/share/jit/$p.elf" "/bin/lx:/usr/share/jit/$p.elf"; do
    quiet
    init=${run%%:*}; a=${run#*:}
    echo "== $p $init $a load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$R/quiet-ucos$TAG.txt"
    timeout 120 nice -n 19 node tools/jit/ucos-run.mjs --build="$B" --init="$init" --args="$a" --stats 2>&1 | grep -E "@@RUN|@@STATE|@@HOST|checksum" >> "$R/quiet-ucos$TAG.txt"
  done
done
echo "done $(date '+%H:%M')" >> "$R/quiet-done$TAG.txt"
