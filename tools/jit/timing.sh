#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host side of the translator's whole-command timings inside UC.OS (Node). For each test:
# wait for load < 3 and the quiet lock, take the lock, boot on a copy of the installed store,
# run tools/jit/os/tbench.sh (N alternating runs per mode), release the lock.
#   sh tools/jit/timing.sh build-NAME N test...
B=$1; N=$2; shift 2
L=/home/ensui/uc-os/.track/bench.lock
for T in "$@"; do
  # uc-os-dev's verification builds go first: no lock until their log's last line says done
  V=${VERIFY_LOG:-/home/ensui/uc-os/.worktrees/c9q/build-vfy-log/log.txt}
  until [ -f "$V" ] && tail -1 "$V" | grep -q '^done'; do sleep 30; done
  until mkdir $L 2>/dev/null; do sleep 20; done                 # the lock first,
  echo "translator timing ($T), pid $$, waiting for quiet" > $L/owner
  until awk '{exit !($1 < 3)}' /proc/loadavg; do sleep 10; done   # then quiet, holding it
  echo "translator timing ($T), pid $$, until $(date -d '+10 min' '+%H:%M') at the latest" > $L/owner
  command cp $B/store-base.bin $B/jit-os/store-t.bin
  echo "== $T load $(cut -d' ' -f1-3 /proc/loadavg) $(date '+%H:%M')" >> $B/jit-os/timing.txt
  timeout 300 nice -n 19 node tools/jit/ucos-run.mjs --build=$B --image=$B/jit-os/initrd.tar --store=$B/jit-os/store-t.bin --init=/bin/sh \
    --input="export T=$T N=$N SIDES=\"$SIDES\"\nsh /t/tbench.sh\nexit\n" 2>&1 | grep -E "jtime|^== |rror" >> $B/jit-os/timing.txt
  echo "== end load $(cut -d' ' -f1 /proc/loadavg)" >> $B/jit-os/timing.txt
  rm -f $L/owner; rmdir $L
  sleep 60   # a gap: let other sessions take the lock between windows
done
