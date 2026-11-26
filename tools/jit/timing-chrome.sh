#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The whole-command timings of tools/jit/timing.sh, in headless Chromium (one browser at a
# time, closed after each page). Each argument is a comma-separated group of tests run in
# one page: the page installs the test packages first (tools/jit/os/inst.sh, not timed;
# headless IndexedDB did not persist between pages here), then runs tbench.sh per test.
#   SIDES="off on off2" sh tools/jit/timing-chrome.sh build-NAME N group...
B=$1; N=$2; shift 2
L=/home/ensui/uc-os/.track/bench.lock
enc() { node -e 'process.stdout.write(encodeURIComponent(process.argv[1]))' "$1"; }
for G in "$@"; do
  IN="sh /t/inst.sh > /dev/null 2>&1\n"
  for T in $(echo $G | tr ',' ' '); do IN="${IN}export T=$T N=$N SIDES=\"$SIDES\"\nsh /t/tbench.sh\n"; done
  IN="${IN}exit\n"
  # uc-os-dev's verification builds go first: no lock until their log's last line says done
  V=${VERIFY_LOG:-/home/ensui/uc-os/.worktrees/c9q/build-vfy-log/log.txt}
  until [ -f "$V" ] && tail -1 "$V" | grep -q '^done'; do sleep 30; done
  until mkdir $L 2>/dev/null; do sleep 20; done                 # the lock first,
  echo "translator timing in Chromium ($G), pid $$, waiting for quiet" > $L/owner
  until awk '{exit !($1 < 3)}' /proc/loadavg; do sleep 10; done   # then quiet, holding it
  echo "translator timing in Chromium ($G), pid $$, until $(date -d '+10 min' '+%H:%M') at the latest" > $L/owner
  echo "== $G load $(cut -d' ' -f1-3 /proc/loadavg) $(date '+%H:%M')" >> $B/jit-os/timing-chrome.txt
  timeout 400 nice -n 19 node tools/jit/chrome.mjs --page=tools/jit/os/page.html --timeout=390000 \
    --query="build=$B&store=0&input=$(enc "$IN")" 2>&1 | grep -E "jtime|rror|HOST" >> $B/jit-os/timing-chrome.txt
  echo "== end load $(cut -d' ' -f1 /proc/loadavg)" >> $B/jit-os/timing-chrome.txt
  rm -f $L/owner; rmdir $L
  sleep 60   # a gap: let other sessions take the lock between windows
done
