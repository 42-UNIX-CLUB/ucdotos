#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run every window manager test under tools/shot.mjs and count PASS/FAIL lines.
# Usage (from the repository root): sh bin/wm/tests/run.sh [build-dir]
#   build first: make B=build-wm PROGS="wm bar launcher uidemo term sh" (zoom needs term)
B=${1:-build-wm}
cd "$(dirname "$0")/../../.." || exit 1
fail=0
for t in layout workspace slide zoom robust misc perf showcase; do
  out=$(timeout 300 node tools/shot.mjs --build="$B" --init=/bin/wm --size=1280x720 --script="bin/wm/tests/$t.mjs" 2>&1)
  echo "$out" | grep -a "^PASS\|^FAIL\|^RESULT\|^DONE\|^idle\|^cycle"
  echo "$out" | grep -q "^DONE $t: 0 failed" || fail=$((fail + 1))
done
echo "test scripts failed: $fail"
exit $fail
