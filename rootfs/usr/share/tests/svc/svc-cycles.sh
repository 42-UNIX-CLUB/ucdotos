# SPDX-License-Identifier: GPL-2.0-or-later
# Start and stop one service many times and watch the manager's memory.
#   node tools/run.mjs --build=B --init=/bin/svcd --args="--unit-path=/usr/share/tests/svc --unit=svc-cycles.target"
# The count comes from /etc/svc-cycles (default 1000).
mkdir -p /etc/systemd/system
cp /usr/share/tests/svc/units/svct-cycle.service /etc/systemd/system/
systemctl daemon-reload
n=1000
[ -f /etc/svc-cycles ] && n=$(cat /etc/svc-cycles)
pages() { v=$(systemctl show | grep MemoryPages=); echo ${v#*=}; }
systemctl start svct-cycle; systemctl stop svct-cycle
rm -f /tmp/svct-cycles
m0=$(pages)
t0=$(svctest now)
i=0
bad=0
while [ $i -lt $n ]; do
  systemctl start svct-cycle || bad=$((bad + 1))
  systemctl stop svct-cycle || bad=$((bad + 1))
  i=$((i + 1))
  if [ $((i % 250)) = 0 ]; then echo "      after $i cycles: manager memory $(pages) pages"; fi
done
t1=$(svctest now)
m1=$(pages)
echo "      $n cycles in $((t1 - t0)) ms; manager memory $m0 pages before, $m1 after"
starts=$(echo $(wc -l < /tmp/svct-cycles))
echo "      the start command ran $starts times"
fail=0
[ $bad = 0 ] || fail=$((fail + 1))
[ "$starts" = "$n" ] || fail=$((fail + 1))
[ "$m0" = "$m1" ] || fail=$((fail + 1))
[ "$(systemctl is-active svct-cycle)" = inactive ] || fail=$((fail + 1))
echo "svc-cycles: 4 checks, $((4 - fail)) passed, $fail failed"
systemctl poweroff
