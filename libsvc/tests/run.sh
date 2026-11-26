#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Native tests of libsvc and of the manager core. Usage: sh libsvc/tests/run.sh [BUILD_DIR]
# Output goes to BUILD_DIR (default build-svc-native, git-ignored).
set -e
cd "$(dirname "$0")/../.."
B=${1:-build-svc-native}
mkdir -p "$B"
CC=${CC:-cc}
F="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -g -O1 -fsanitize=address,undefined"
$CC $F -o "$B/test_svc" libsvc/tests/test_svc.c libsvc/unitfile.c libsvc/cal.c libsvc/names.c libsvc/os.c -lm
"$B/test_svc"
if [ -f libsvc/tests/test_core.c ]; then
  $CC $F -Ibin/svcd -o "$B/test_core" libsvc/tests/test_core.c bin/svcd/core.c libsvc/unitfile.c libsvc/cal.c libsvc/names.c libsvc/os.c -lm
  "$B/test_core" "$B"
fi
$CC $F -Ibin/svcd -o "$B/test_journal" libsvc/tests/test_journal.c bin/svcd/journal.c
"$B/test_journal"
# the programs themselves, built for the host (used by host-integration.sh)
L="libsvc/unitfile.c libsvc/cal.c libsvc/names.c libsvc/os.c -lm"
$CC $F -o "$B/svcd" bin/svcd/main.c bin/svcd/core.c bin/svcd/journal.c $L
$CC $F -o "$B/systemctl" bin/systemctl/main.c $L
