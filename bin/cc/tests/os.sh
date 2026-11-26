#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build programs of the tree with our cc and ld (natively, as a cross compiler), put
# them into a UC.OS image in place of the clang builds, and run the system suites.
# usage: sh bin/cc/tests/os.sh BUILD-DIR "prog1 prog2 ..." [suites]
#   suites: any of ktest libctest sh desktop (default: ktest libctest sh)
# The image holds the programs in $PROGS (set it to choose; default: what the suites need).
set -u
B=$1
CCPROGS=$2
SUITES=${3:-"ktest libctest sh"}
T=$(dirname "$0")
cd "$(cd "$T/../../.." && pwd)"
export ASAN_OPTIONS=detect_leaks=0
PROGS=${PROGS:-"sh cat echo ls true false wc grep sort head tail seq uniq tee env mkdir rm rmdir mv cp touch pwd which kill sleep uname date ps hexdump yes clear bc man ktest libctest cc ld"}
# only keep programs that exist in this tree
P=""
for p in $PROGS; do [ -e bin/$p.c ] || [ -d bin/$p ] && P="$P $p"; done
nice -n 19 make -s B="$B" PROGS="$P" || exit 1
nice -n 19 sh "$T/build.sh" "$B" >/dev/null 2>&1 || { echo "os: native build failed"; exit 1; }
fails=0
for p in $CCPROGS; do
  if nice -n 19 sh "$T/buildall.sh" "$B" $p; then \cp -f "$B/ccobj/$p" "$B/bin/$p" && touch "$B/bin/$p"; else fails=$((fails+1)); fi
done
make -s B="$B" PROGS="$P" >/dev/null || exit 1
for s in $SUITES; do
  case $s in
  ktest) nice -n 19 timeout 120 node tools/run.mjs --build="$B" --init=/bin/ktest | tail -1 ;;
  libctest) nice -n 19 timeout 120 node tools/run.mjs --build="$B" --init=/bin/libctest | tail -1 ;;
  desktop) nice -n 19 timeout 300 node tools/shot.mjs --build="$B" --script=tools/tests/desktop.mjs --quiet | tail -3; echo "os: screenshots in $B/shots" ;;
  sh) nice -n 19 timeout 300 node tools/run.mjs --build="$B" --init=/bin/sh --input='sh /usr/share/tests/sh-tests.sh\nexit\n' | grep -E "sh-tests:|FAIL" | tail -15 ;;
  esac
done
[ $fails -eq 0 ]
