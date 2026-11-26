# SPDX-License-Identifier: GPL-2.0-or-later
# Real Debian unit files (from openssh-server, cron, rsyslog, logrotate) under /debian,
# with native stand-ins (/bin/svctest under the daemons' names) in place of the real
# riscv64 daemons, which need the engine. The image is made by libsvc/tests/mkdebian.sh;
# the unit files are Debian's and are not in git.
#   node tools/run.mjs --build=B-deb --init=/bin/svcd --args="--unit-path=/usr/share/tests/svc --unit=svc-debian.target"
pass=0
fail=0
check() {
  if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "ok    $1"
  else fail=$((fail + 1)); echo "FAIL  $1"; echo "      expected: [$2]"; echo "      got:      [$3]"; fi
}
yes_if() { if "$@" >/dev/null 2>&1; then echo yes; else echo no; fi; }
prop() { systemctl show -p "$1" --value "$2"; }

echo "--- the Debian root is seen"
v=$(systemctl show | grep GuestRoot=)
check "guest root" /debian "${v#*=}"
check "/debian/run/systemd/system exists for maintainer scripts" yes "$(yes_if test -d /debian/run/systemd/system)"
check "Debian units listed" yes "$(yes_if sh -c 'systemctl list-unit-files | grep -q "^ssh.service"')"
check "fragment path" /debian/usr/lib/systemd/system/ssh.service "$(prop FragmentPath ssh.service)"
check "guest unit" yes "$(prop Guest ssh.service)"

echo "--- what debhelper's postinst does, run by our shell"
# the generated lines, as found in 54 of the 61 packages surveyed
for u in ssh.service cron.service; do
  deb-systemd-helper unmask "$u" >/dev/null || true
  if deb-systemd-helper --quiet was-enabled "$u"; then
    deb-systemd-helper enable "$u" >/dev/null || true
  else
    deb-systemd-helper update-state "$u" >/dev/null || true
  fi
done
check "ssh enabled by deb-systemd-helper" enabled "$(systemctl is-enabled ssh.service)"
check "link made in the Debian root" yes "$(yes_if test -f /debian/etc/systemd/system/multi-user.target.wants/ssh.service)"
check "state file" yes "$(yes_if test -f /debian/var/lib/systemd/deb-systemd-helper-enabled/ssh.service.dsh-also)"
check "alias sshd.service" alias "$(systemctl is-enabled sshd.service)"
check "was-enabled now" 0 "$(deb-systemd-helper --quiet was-enabled ssh.service; echo $?)"
check "debian-installed" 0 "$(deb-systemd-helper debian-installed ssh.service; echo $?)"
if [ -d /run/systemd/system ]; then
  systemctl --system daemon-reload >/dev/null || true
  deb-systemd-invoke start ssh.service cron.service >/dev/null || true
fi
check "ssh started by deb-systemd-invoke" active "$(systemctl is-active ssh.service)"
check "cron started" active "$(systemctl is-active cron.service)"
check "ssh: ExecStartPre (sshd -t) and READY=1 from the stand-in" "stand-in ready" "$(prop StatusText ssh.service)"
check "ssh: RuntimeDirectory=sshd made in the guest root" yes "$(yes_if test -d /debian/run/sshd)"
check "sshd name reaches the same unit" active "$(systemctl is-active sshd.service)"
systemctl reload ssh.service
check "ssh reload (sshd -t, then kill -HUP: a restart on this kernel)" active "$(systemctl is-active ssh.service)"

echo "--- the admin disables, the package is upgraded"
deb-systemd-helper disable cron.service
check "disabled" disabled "$(systemctl is-enabled cron.service)"
check "was-enabled is now false: an upgrade keeps it disabled" 1 "$(deb-systemd-helper --quiet was-enabled cron.service; echo $?)"
deb-systemd-helper update-state cron.service
deb-systemd-invoke start cron.service
check "deb-systemd-invoke start skips a disabled unit (it keeps running if it was)" active "$(systemctl is-active cron.service)"
systemctl stop cron.service
deb-systemd-invoke start cron.service
check "and does not start it when stopped" inactive "$(systemctl is-active cron.service)"

echo "--- units that cannot work here, and say why"
systemctl start ssh.socket 2>/dev/null
check "ssh.socket (ListenStream=22) fails: no network" failed "$(systemctl is-active ssh.socket)"
check "reason in the journal" yes "$(yes_if sh -c 'journalctl -u ssh.socket | grep -q "not supported on UC.OS"')"
systemctl start rsyslog.service 2>/tmp/rs.err
check "rsyslog needs syslog.socket (shipped by systemd itself)" yes "$(yes_if grep -q 'syslog.socket not found' /tmp/rs.err)"

echo "--- a timer and a oneshot from logrotate"
systemctl start logrotate.timer
check "logrotate.timer (OnCalendar=daily, RandomizedDelaySec=1h) waits" "active waiting" "$(prop ActiveState logrotate.timer) $(prop SubState logrotate.timer)"
ne=$(prop NextElapse logrotate.timer)
check "next elapse is set" yes "$(test -n "$ne" && echo yes)"
systemctl start logrotate.service
check "logrotate.service ran to the end" "inactive success" "$(prop ActiveState logrotate.service) $(prop Result logrotate.service)"
check "its output is in the journal" yes "$(yes_if sh -c 'journalctl -u logrotate.service | grep -q "did its job"')"

echo "--- purge"
deb-systemd-helper purge ssh.service
check "purge removes the links and the state" "disabled no" "$(systemctl is-enabled ssh.service) $(yes_if test -f /debian/var/lib/systemd/deb-systemd-helper-enabled/ssh.service.dsh-also)"
deb-systemd-helper mask ssh.service
check "mask (by the helper)" masked "$(systemctl is-enabled ssh.service)"
deb-systemd-helper unmask ssh.service
check "unmask (only its own mask)" disabled "$(systemctl is-enabled ssh.service)"

echo "deb-tests: $((pass + fail)) checks, $pass passed, $fail failed"
systemctl poweroff
