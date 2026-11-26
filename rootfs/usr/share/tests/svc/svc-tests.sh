# SPDX-License-Identifier: GPL-2.0-or-later
# In-OS test of the service manager. It runs as a service under the manager as pid 1:
#   node tools/run.mjs --build=B --init=/bin/svcd --args="--unit-path=/usr/share/tests/svc --unit=svc-selftest.target"
# Every check prints "ok" or "FAIL"; the last line is the count. It ends with poweroff.

pass=0
fail=0
check() { # check NAME EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
    echo "ok    $1"
  else
    fail=$((fail + 1))
    echo "FAIL  $1"
    echo "      expected: [$2]"
    echo "      got:      [$3]"
  fi
}
yes_if() { if "$@" >/dev/null 2>&1; then echo yes; else echo no; fi; }
prop() { systemctl show -p "$1" --value "$2"; }
pages() { v=$(systemctl show | grep MemoryPages=); echo ${v#*=}; }
alive() { kill -0 $1 2>/dev/null; }
waitfor() { # waitfor TRIES UNIT PROPERTY VALUE
  n=0
  while [ $n -lt $1 ]; do
    [ "$(prop $3 $2)" = "$4" ] && return 0
    sleep 0.1
    n=$((n + 1))
  done
  return 1
}

echo "--- the manager"
v=$(systemctl show | grep '^Pid=')
check "running as pid 1" 1 "${v#*=}"
check "/run/systemd/system exists" yes "$(yes_if test -d /run/systemd/system)"
check "is-system-running" running "$(systemctl is-system-running)"
check "default target reached" active "$(systemctl is-active svc-selftest.target)"
check "this script is a service" active "$(systemctl is-active svc-selftest.service)"
mkdir -p /etc/systemd/system
cp /usr/share/tests/svc/units/* /etc/systemd/system/
systemctl daemon-reload
check "daemon-reload" 0 "$?"
check "unit files listed" yes "$(yes_if sh -c 'systemctl list-unit-files | grep -q svct-simple.service')"

echo "--- start, status, stop"
systemctl start svct-simple.service
check "start" active "$(systemctl is-active svct-simple)"
pid=$(prop MainPID svct-simple.service)
check "main pid is a process" yes "$(yes_if alive $pid)"
st=$(systemctl status svct-simple.service)
check "status: Active line" yes "$(yes_if sh -c "echo \"$st\" | grep -q 'Active: active (running)'")"
check "status: journal lines" yes "$(yes_if sh -c "echo \"$st\" | grep -q 'running $pid'")"
check "status exit code when active" 0 "$(systemctl status svct-simple >/dev/null; echo $?)"
systemctl stop svct-simple.service
check "stop" inactive "$(systemctl is-active svct-simple)"
check "process gone after stop" no "$(yes_if alive $pid)"
check "status exit code when inactive" 3 "$(systemctl status svct-simple >/dev/null; echo $?)"
check "status of a unit that does not exist" 4 "$(systemctl status nosuch >/dev/null 2>&1; echo $?)"
systemctl restart svct-simple
check "restart from inactive starts" active "$(systemctl is-active svct-simple)"
t1=$(prop ActiveEnterTimestamp svct-simple)
sleep 0.05
systemctl restart svct-simple
check "restart gives a new activation" yes "$(test "$t1" != "$(prop ActiveEnterTimestamp svct-simple)" && echo yes)"
systemctl stop svct-simple

echo "--- restart on failure, with backoff, until the start limit"
systemctl start svct-crash.service
waitfor 80 svct-crash.service Result start-limit-hit
check "ends in start-limit-hit" start-limit-hit "$(prop Result svct-crash.service)"
check "failed state" failed "$(systemctl is-active svct-crash)"
check "4 automatic restarts, the last refused by the limit" 4 "$(prop NRestarts svct-crash.service)"
check "journal: restart counter" yes "$(yes_if sh -c 'journalctl -u svct-crash | grep -q "restart counter is at 3"')"
check "journal: start limit" yes "$(yes_if sh -c 'journalctl -u svct-crash | grep -q "Start request repeated too quickly"')"
check "is-failed" 0 "$(systemctl is-failed -q svct-crash; echo $?)"
systemctl reset-failed svct-crash
check "reset-failed" inactive "$(systemctl is-active svct-crash)"

echo "--- ordering"
rm -f /tmp/svct-order
systemctl start svct-order.target
check "b (notify, ready after 500 ms) before a" "b a" "$(echo $(cat /tmp/svct-order))"
check "both active" "active active" "$(echo $(systemctl is-active svct-a svct-b))"
check "status text from READY" serving "$(prop StatusText svct-b.service)"
systemctl stop svct-a svct-b

echo "--- oneshot"
rm -f /tmp/svct-once
systemctl start svct-once.service
check "oneshot remains active" "active exited" "$(prop ActiveState svct-once) $(prop SubState svct-once)"
check "both commands ran in order" "1 2" "$(echo $(cat /tmp/svct-once))"
systemctl stop svct-once

echo "--- a timer"
rm -f /tmp/svct-ticks
systemctl start svct-tick.timer
check "timer active" active "$(systemctl is-active svct-tick.timer)"
check "list-timers" yes "$(yes_if sh -c 'systemctl list-timers | grep -q svct-tick.timer')"
sleep 3.5
n=$(echo $(wc -l < /tmp/svct-ticks))
check "fired 3 times in 3.5 s" 3 "$n"
systemctl stop svct-tick.timer
sleep 1.2
check "stopped timer does not fire" "$n" "$(echo $(wc -l < /tmp/svct-ticks))"

echo "--- the journal"
m0=$(pages)
systemctl start svct-lines.service
check "last line kept" "line 10000 of 10000" "$(journalctl -u svct-lines -t svctest -n 1 -o cat)"
check "the manager's own lines are there too" "Finished Writes 10000 lines." "$(journalctl -u svct-lines -n 1 -o cat)"
check "-n 3 gives 3 lines" 3 "$(echo $(journalctl -u svct-lines -n 3 -o cat | wc -l))"
m1=$(pages)
echo "      manager memory: $m0 pages before 10000 journal lines, $m1 after"
check "manager memory flat after 10000 lines" "$m0" "$m1"
check "no entries for an unknown unit" "-- No entries --" "$(journalctl -u nothing.service)"

echo "--- enable and disable across daemon-reload"
check "disabled at first" disabled "$(systemctl is-enabled svct-en.service)"
systemctl enable svct-en.service > /tmp/svct-en.out
check "enable says what it made" yes "$(yes_if grep -q 'svct-en.target.wants/svct-en.service' /tmp/svct-en.out)"
check "is-enabled" enabled "$(systemctl is-enabled svct-en.service)"
systemctl daemon-reload
systemctl start svct-en.target
check "the target pulls it in" active "$(systemctl is-active svct-en.service)"
systemctl disable svct-en.service >/dev/null
systemctl daemon-reload
systemctl stop svct-en.target svct-en.service
systemctl start svct-en.target
check "after disable, not pulled in" inactive "$(systemctl is-active svct-en.service)"
check "is-enabled exit code when disabled" 1 "$(systemctl is-enabled -q svct-en.service; echo $?)"
systemctl stop svct-en.target

echo "--- a failing unit"
systemctl start svct-fail.service 2>/tmp/svct-fail.err
check "start fails with 1" 1 "$?"
check "message" yes "$(yes_if grep -q 'Job for svct-fail.service failed because the control process exited with error code' /tmp/svct-fail.err)"
check "failed" failed "$(systemctl is-active svct-fail)"
check "result" exit-code "$(prop Result svct-fail)"
systemctl reset-failed svct-fail

echo "--- dependency cycles"
systemctl start svct-cyc.target
check "cycle reported in the journal" yes "$(yes_if sh -c 'journalctl | grep -q "Found ordering cycle on svct-cyc"')"
check "cycle broken by deleting a job" yes "$(yes_if sh -c 'journalctl | grep -q "deleted to break ordering cycle"')"
check "target still reached" active "$(systemctl is-active svct-cyc.target)"
systemctl start svct-hard1.service 2>/tmp/svct-cyc.err
check "a cycle of requirements fails" 1 "$?"
check "message" yes "$(yes_if grep -q 'Transaction order is cyclic' /tmp/svct-cyc.err)"
systemctl stop svct-cyc.target svct-cyc1 svct-cyc2
check "units of a cycle can be stopped" "inactive inactive" "$(echo $(systemctl is-active svct-cyc1 svct-cyc2))"

echo "--- a stop command that hangs"
systemctl start svct-stubborn
t0=$(svctest now)
systemctl stop svct-stubborn
t1=$(svctest now)
check "stopped anyway" yes "$(test "$(systemctl is-active svct-stubborn)" != active && echo yes)"
check "after TimeoutStopSec=1s" yes "$(test $((t1 - t0)) -ge 900 && test $((t1 - t0)) -lt 5000 && echo yes)"
check "timeout logged" yes "$(yes_if sh -c 'journalctl -u svct-stubborn | grep -q "Stopping timed out"')"
systemctl reset-failed svct-stubborn

echo "--- all processes of a service"
systemctl start svct-kids
sleep 0.3
procs=$(prop Processes svct-kids)
set -- $procs
check "main and two children tracked" 3 "$#"
systemctl stop svct-kids
left=0
for p in $procs; do alive $p && left=$((left + 1)); done
check "all three gone after stop" 0 "$left"
systemctl start svct-fork
check "forking: active" active "$(systemctl is-active svct-fork)"
fp=$(cat /run/svct-fork.pid)
check "forking: main pid from PIDFile" "$fp" "$(prop MainPID svct-fork)"
systemctl stop svct-fork
check "forking: daemon gone after stop" no "$(yes_if alive $fp)"

echo "--- socket activation"
systemctl start svct-echo.socket
check "socket listening" "active listening" "$(prop ActiveState svct-echo.socket) $(prop SubState svct-echo.socket)"
check "service not started yet" inactive "$(systemctl is-active svct-echo.service)"
check "a connection starts the service, which answers" "echo: hello" "$(svctest client /run/svct-echo.sock hello)"
sleep 0.3
check "the service got LISTEN_FDS" yes "$(yes_if sh -c 'journalctl -u svct-echo.service | grep -q "served one connection"')"
check "and the socket listens again" listening "$(prop SubState svct-echo.socket)"
check "second connection" "echo: again" "$(svctest client /run/svct-echo.sock again)"
systemctl stop svct-echo.socket

echo "--- readiness"
t0=$(svctest now)
systemctl start svct-notify
t1=$(svctest now)
check "start waits for READY=1" yes "$(test $((t1 - t0)) -ge 250 && echo yes)"
check "active after READY=1" active "$(systemctl is-active svct-notify)"
systemctl stop svct-notify

systemctl start svct-shnotify
check "systemd-notify from a shell script" "active from-sh" "$(prop ActiveState svct-shnotify) $(prop StatusText svct-shnotify)"
systemctl stop svct-shnotify

echo "--- systemd-tmpfiles and systemd-sysusers"
mkdir -p /etc/tmpfiles.d
echo "d /run/svct-tmp 0755 root root -" > /etc/tmpfiles.d/svct.conf
echo "f /run/svct-tmp/new 0644 root root - hello" >> /etc/tmpfiles.d/svct.conf
echo "D /run/svct-tmp/cache 0755 root root -" >> /etc/tmpfiles.d/svct.conf
echo "x /run/svct-tmp/keep" >> /etc/tmpfiles.d/svct.conf
echo "p /run/svct-tmp/fifo 0644 root root -" >> /etc/tmpfiles.d/svct.conf
systemd-tmpfiles --create svct.conf 2>/tmp/tmpf.err
check "tmpfiles: directory and file" "yes hello" "$(yes_if test -d /run/svct-tmp/cache) $(cat /run/svct-tmp/new)"
check "tmpfiles: a FIFO cannot be made, and it says so" yes "$(yes_if grep -q 'FIFOs cannot be made' /tmp/tmpf.err)"
touch /run/svct-tmp/cache/old
systemd-tmpfiles --remove svct.conf 2>/dev/null
check "tmpfiles --remove empties D" no "$(yes_if test -f /run/svct-tmp/cache/old)"
check "systemd-sysusers is accepted" 0 "$(systemd-sysusers; echo $?)"

echo "--- reload by signal on a kernel without signals"
systemctl start svct-reload
t1=$(prop ActiveEnterTimestamp svct-reload)
sleep 0.05
systemctl reload svct-reload
check "reload succeeds" 0 "$?"
check "service still active" active "$(systemctl is-active svct-reload)"
v=$(systemctl show | grep Signals=)
if [ "${v#*=}" = no ]; then
  check "became a restart (new activation)" yes "$(test "$t1" != "$(prop ActiveEnterTimestamp svct-reload)" && echo yes)"
  check "said so in the journal" yes "$(yes_if sh -c 'journalctl -u svct-reload | grep -q "restarting instead"')"
fi
systemctl stop svct-reload

echo "--- memory limit"
systemctl start svct-mem
waitfor 80 svct-mem.service Result oom-kill
check "MemoryMax=4M: killed" oom-kill "$(prop Result svct-mem)"
systemctl reset-failed svct-mem

echo "--- the manager's own memory"
m2=$(pages)
echo "      manager memory now: $m2 pages (64 KiB each)"

echo "svc-tests: $((pass + fail)) checks, $pass passed, $fail failed"
systemctl poweroff
