# SPDX-License-Identifier: GPL-2.0-or-later
# Inside UC.OS, after inst.sh: reinstall packages with their maintainer scripts run by
# Debian's dash under the engine (script-runner /bin/lx). $PKGS selects the packages.
# What the scripts leave behind goes to /tmp/out for comparison between modes.
mkdir -p /tmp/out
printf "native-architecture riscv64\nscript-runner /bin/lx\n" > /etc/dpkg/dpkg.cfg.d/integ
for p in $PKGS; do
  apt reinstall -y $p > /tmp/r.txt 2>&1; echo "REINSTALL $p exit $?" >> /tmp/out/status
  grep -i "error\|not found\|warning\|line [0-9]*:" /tmp/r.txt | head -4 >> /tmp/out/status
done
cat /debian/etc/shells > /tmp/out/shells 2>&1
cat /debian/etc/passwd > /tmp/out/passwd 2>&1
cat /debian/etc/group > /tmp/out/group 2>&1
dpkg -l | grep -v '^ii' | tail -n +6 > /tmp/out/notii
ls -la /debian/etc | sed 's/ [A-Z][a-z][a-z] [ 0-9]* [0-9:]* / /' > /tmp/out/etc
