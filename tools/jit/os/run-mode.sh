#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host side: boot the test image on a fresh copy of the installed store and run a script
# inside UC.OS with LX_JIT=MODE; the console output goes to build-NAME/jit-os/out-TAG.txt.
#   sh tools/jit/os/run-mode.sh build-NAME MODE SCRIPT TAG [JIT_OPTS] [MORE_ENV]
B=$1; MODE=$2; SCRIPT=$3; TAG=$4; OPTS=${5:-}; MORE=${6:-}
until [ ! -d /home/ensui/uc-os/.track/bench.lock ] && awk '{exit !($1<5)}' /proc/loadavg; do sleep 15; done
command cp $B/store-base.bin $B/jit-os/store-$TAG.bin
IN="export LX_JIT=$MODE LX_JIT_OPTS='$OPTS' $MORE\nsh /t/$SCRIPT\nfor f in /tmp/out/*; do echo \"=== \$f\"; cat \$f; done\nexit\n"
s=$(date +%s%N)
timeout 170 nice -n 19 node tools/jit/ucos-run.mjs --build=$B --image=$B/jit-os/initrd.tar --store=$B/jit-os/store-$TAG.bin --init=/bin/sh --input="$IN" --stats > $B/jit-os/out-$TAG.txt 2>&1
echo "exit $? wall $(( ($(date +%s%N) - s) / 1000000 )) ms load $(cut -d' ' -f1 /proc/loadavg)" >> $B/jit-os/out-$TAG.txt
tail -1 $B/jit-os/out-$TAG.txt
