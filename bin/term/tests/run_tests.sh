#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Native tests of term (host cc, no OS needed). Run from the repository root:
#   sh bin/term/tests/run_tests.sh [PYTHON_WITH_PYTE]
# The in-OS tests are tools/shot.mjs scripts in bin/term/tests/os/ (see their headers).
set -e
B=build-term/native
mkdir -p $B
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all"
cc -std=c11 -Wall -Wextra $SAN -Ibin/term bin/term/tests/test_vt.c bin/term/vt.c -o $B/test_vt
$B/test_vt
cc -std=c11 $SAN -Ibin/term bin/term/tests/fuzz_vt.c bin/term/vt.c -o $B/fuzz_vt
$B/fuzz_vt fuzz 8000000
cc -std=c11 -O2 -Ibin/term bin/term/tests/fuzz_vt.c bin/term/vt.c -o $B/bench_vt
$B/bench_vt bench
GFX="bin/term/render.c bin/term/vt.c bin/term/theme_fallback.c libgfx/gfx.c libgfx/ttf.c -lm"
cc -std=c11 -O2 -Wall -Ibin/term -Ilibgfx -Ilibui bin/term/tests/render_test.c $GFX -o $B/render_test
python3 bin/term/tests/scenes.py build-term $B/render_test
$B/render_test --cols 160 --rows 45 --bench --out $B/bench.ppm build-term/scenes/prompt_ls.bin build-term/scenes/palette.bin
head -c 3000000 /dev/urandom > $B/random.bin
cc -std=c11 $SAN -Ibin/term -Ilibgfx -Ilibui bin/term/tests/render_test.c $GFX -o $B/render_asan
$B/render_asan --cols 120 --rows 40 --render-each 997 --out $B/fuzz.ppm $B/random.bin && echo "renderer on random input: no faults"
cc -std=c11 -O2 -Ibin/term bin/term/tests/replay.c bin/term/vt.c -o $B/replay
PY=${1:-python3}
if $PY -c "import pyte" 2>/dev/null; then $PY bin/term/tests/conformance.py build-term $B/replay
else echo "pyte not installed: conformance comparison skipped"; fi
