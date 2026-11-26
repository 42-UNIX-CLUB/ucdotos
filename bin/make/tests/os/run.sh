#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build an image with make and the tools its tests use, boot it under Node and
# run /usr/share/tests/make/run.sh inside UC.OS. Fails if any case fails.
#   sh bin/make/tests/os/run.sh [build-dir] [case ...]
B=${1:-build-make}
[ $# -gt 0 ] && shift
cd "$(dirname "$0")/../../../.." || exit 2
make -j2 B="$B" PROGS="sh make ls cat echo true false touch rm mkdir cp mv sort wc seq sleep head tail grep env pwd uniq tee rmdir" >/dev/null || exit 2
out=$(node tools/run.mjs --build="$B" --init=/bin/sh --input="sh /usr/share/tests/make/run.sh $*\nexit\n" 2>&1)
printf '%s\n' "$out"
case "$out" in
  *" 0 failed"*) exit 0 ;;
  *) exit 1 ;;
esac
