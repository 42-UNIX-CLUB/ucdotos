#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The manager and systemctl, built for the host, driving real host processes in a
# scratch unit tree. Usage: sh libsvc/tests/host-integration.sh [BUILD_DIR]
# (build first with libsvc/tests/run.sh). Touches nothing outside BUILD_DIR.
cd "$(dirname "$0")/../.."
B=${1:-build-svc-native}
T=$(pwd)/$B/host-$$
mkdir -p "$T/etc/systemd/system" "$T/usr/lib/systemd/system"
command cp rootfs/usr/lib/systemd/system/*.target "$T/usr/lib/systemd/system/"
U=$T/etc/systemd/system
pass=0; fail=0
ok() { if [ "$1" = 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $2"; fi; }
cat > $U/test.target <<EOF
[Unit]
Description=Host test target
Wants=hello.service one.service
EOF
cat > $U/hello.service <<EOF
[Unit]
Description=Hello service
[Service]
ExecStart=/bin/sh -c 'echo hello from \$\$; exec sleep 100'
EOF
cat > $U/one.service <<EOF
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'echo one done'
EOF
cat > $U/fail.service <<EOF
[Service]
Type=oneshot
ExecStart=/bin/false
EOF
cat > $U/crash.service <<EOF
[Unit]
StartLimitIntervalSec=30s
StartLimitBurst=3
[Service]
ExecStart=/bin/sh -c 'echo crashing; exit 3'
Restart=on-failure
RestartSec=200ms
EOF
cat > $U/tick.timer <<EOF
[Timer]
OnActiveSec=1s
OnUnitActiveSec=1s
EOF
cat > $U/tick.service <<EOF
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo tick'
EOF
cat > $U/en.service <<EOF
[Service]
ExecStart=/bin/sleep 100
[Install]
WantedBy=multi-user.target
EOF
export SVCD_SOCKET=$T/ctl SVC_ROOT=$T SVC_GUEST_ROOT=/nonexistent
SC=$B/systemctl
$B/svcd --root=$T --socket=$T/ctl --unit=test.target --guest-root=/nonexistent --quiet > $T/svcd.out 2>&1 &
MPID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S $T/ctl ] && break; sleep 0.2; done
sleep 0.5
$SC is-active hello.service >/dev/null; ok $? "hello active"
$SC is-active -q one.service; ok $? "oneshot active (remain)"
out=$($SC status hello.service); rc=$?
[ $rc = 0 ] && echo "$out" | grep -q "Active: active (running)" && echo "$out" | grep -q "hello from"; ok $? "status shows state and journal ($rc)"
$SC start fail.service 2>$T/err; rc=$?
[ $rc = 1 ] && grep -q "Job for fail.service failed because the control process exited with error code" $T/err; ok $? "failing start: message and rc 1 (got $rc)"
$SC is-failed -q fail.service; ok $? "is-failed"
$SC status fail.service >/dev/null; [ $? = 3 ]; ok $? "status of a failed unit is 3"
$SC status nosuch.service 2>/dev/null; [ $? = 4 ]; ok $? "status of an unknown unit is 4"
$SC start nosuch.service 2>$T/err; rc=$?; grep -q "Unit nosuch.service not found." $T/err; ok $? "start unknown: message (rc $rc)"
$SC start crash.service 2>/dev/null
sleep 2
st=$($SC show -p Result --value crash.service)
[ "$st" = "start-limit-hit" ]; ok $? "crash loop ends in start-limit-hit (got $st)"
$SC start tick.timer; ok $? "start timer"
sleep 2.6
n=$($B/systemctl show -p NRestarts --value tick.service)
$SC list-timers | grep -q tick.timer; ok $? "list-timers shows it"
cnt=$(SVCD_SOCKET=$T/ctl $SC status tick.service -n 50 | grep -c "tick$")
[ "$cnt" -ge 2 ]; ok $? "timer fired at least twice in 2.6 s (got $cnt)"
$SC is-enabled en.service | grep -q disabled; ok $? "is-enabled disabled"
$SC enable en.service | grep -q "Created symlink"; ok $? "enable prints the link"
$SC is-enabled -q en.service; ok $? "is-enabled after enable"
$SC start multi-user.target; ok $? "start multi-user.target"
$SC is-active -q en.service; ok $? "enabled unit started by its target after the implicit daemon-reload"
$SC disable --now en.service >/dev/null; ok $? "disable --now"
$SC is-active -q en.service; [ $? = 3 ]; ok $? "stopped by disable --now"
$SC restart hello.service; ok $? "restart"
$SC reload hello.service 2>$T/err; [ $? = 1 ] && grep -q "Job type reload is not applicable" $T/err; ok $? "reload without ExecReload"
$SC list-units | grep -q "hello.service"; ok $? "list-units"
$SC list-unit-files | grep -q "en.service.*disabled"; ok $? "list-unit-files"
$SC cat hello.service | grep -q "^# $U/hello.service"; ok $? "cat"
$SC mask fail.service 2>/dev/null; [ $? = 1 ]; ok $? "mask refuses a file in /etc"
$SC poweroff; ok $? "poweroff accepted"
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do kill -0 $MPID 2>/dev/null || break; sleep 0.2; done
kill -0 $MPID 2>/dev/null; [ $? != 0 ]; ok $? "manager exits after poweroff"
pgrep -f "sleep 100" >/dev/null && [ "$(pgrep -f -P $MPID 'sleep 100')" != "" ]; [ $? != 0 ]; ok $? "no service process left"
echo "host-integration: $pass passed, $fail failed"
[ $fail = 0 ]
