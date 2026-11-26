# SPDX-License-Identifier: GPL-2.0-or-later
# Worker-process prototype: the scenario the browser test page runs (boot/worker-proto/test.mjs)
# and that can be run under Node too. Lines "[test] phase NAME" tell the page where each
# phase starts. Argument: coi (the page is cross-origin isolated, so approach A can run) or nocoi.
#   sh /usr/share/tests/wp-tests.sh coi
mode=$1
echo "[test] phase basic"
echo typed | wproto-b basic -
if [ "$mode" = coi ]; then echo typed | wproto-a basic -; fi
echo "[test] phase bench"
wproto bench
wproto-b bench
if [ "$mode" = coi ]; then wproto-a bench; fi
echo "[test] phase bench-under-load"
yes > /dev/null &
yp=$!
wproto-b bench
if [ "$mode" = coi ]; then wproto-a bench; fi
kill $yp
wait
echo "[test] phase startup"
wproto startup /bin/wproto 30
wproto startup /bin/wproto-b 30
if [ "$mode" = coi ]; then wproto startup /bin/wproto-a 30; fi
echo "[test] phase kill"
wproto killtest /bin/wproto-b 300
if [ "$mode" = coi ]; then wproto killtest /bin/wproto-a 300; fi
echo "[test] phase spin-main"
wproto spin 1.5
echo "[test] phase spin-b"
wproto-b spin 4 &
wproto tick 350 10 fb
wait
if [ "$mode" = coi ]; then
  echo "[test] phase spin-a"
  wproto-a spin 4 &
  wproto tick 350 10 fb
  wait
fi
echo "[test] phase spin-y"
wproto-y spin 4 &
wproto tick 350 10 fb
wait
echo "[test] phase end"
