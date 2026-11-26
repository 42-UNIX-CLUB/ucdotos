#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run the system test suites against a build directory and fail if any check fails.
#   sh tools/test.sh [build-dir]        (normally through: make B=build-NAME test)
B=${1:-build}
fail=0
run() {
  name=$1; shift
  last=$("$@" 2>&1 | tail -1)
  case "$last" in
    *" 0 failed"*) echo "  ok    $name: $last" ;;
    *) echo "  FAIL  $name: $last"; fail=1 ;;
  esac
}
run "kernel   " node tools/run.mjs --build="$B" --init=/bin/ktest
run "C library" node tools/run.mjs --build="$B" --init=/bin/libctest
run "shell    " node tools/run.mjs --build="$B" --init=/bin/sh --input='sh /usr/share/tests/sh-tests.sh\nexit\n'
run "sound    " node tools/tests/audio.mjs --build="$B"
if [ $fail -eq 0 ]; then echo "all suites passed"; else echo "some suites failed"; fi
exit $fail
