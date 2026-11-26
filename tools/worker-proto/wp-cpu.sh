# SPDX-License-Identifier: GPL-2.0-or-later
# Worker-process prototype: what each build costs real programs in user code. Suffixes:
# -p plain (same compiler wrapper, no instrumentation), -y yield points at every loop,
# -c -fsanitize-coverage hooks, -b approach B (ordinary code, runs in a Worker),
# -a approach A (atomics build with a shared memory, runs in a Worker).
#   sh /usr/share/tests/wp-cpu.sh coi|nocoi [runs]
mode=$1
n=${2:-7}
echo '2^60000' > /tmp/bc1
echo 'scale=1200; sqrt(2)' > /tmp/bc2
seq 1 40000 > /tmp/nums
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do cat /usr/share/tests/sh-tests.sh; done > /tmp/text
v="p y c b"
if [ "$mode" = coi ]; then v="p y c b a"; fi
for s in $v; do
  wproto time $n /tmp/bc1 /bin/bc-$s
  wproto time $n /tmp/bc2 /bin/bc-$s
  wproto time $n /tmp/nums /bin/sort-$s
  wproto time $n /tmp/text /bin/grep-$s -c 'e.*a.*s'
done
for p in wproto-p wproto-y wproto-b; do $p rate; done
if [ "$mode" = coi ]; then wproto-a rate; fi
echo "[test] phase end"
