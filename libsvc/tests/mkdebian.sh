#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Make a test image with real Debian unit files under /debian and native stand-ins
# for their daemons. Usage: sh libsvc/tests/mkdebian.sh BUILD UNPACKED OUT
#   BUILD     a build directory with kernel.wasm, root/ and bin/svctest (make B=BUILD)
#   UNPACKED  where the .deb files were unpacked, one directory per package with data/
#             (the survey's build-scratch/x); these files are Debian's: never commit them
#   OUT       a new build directory name for the image (kernel.wasm + initrd.tar)
set -e
B=$1; X=$2; O=$3
[ -f "$B/kernel.wasm" ] && [ -d "$X" ] && [ -n "$O" ] || { echo "usage: mkdebian.sh BUILD UNPACKED OUT"; exit 2; }
mkdir -p "$O/root"
command cp -r "$B/root/." "$O/root/"
command cp "$B/kernel.wasm" "$O/"
R=$O/root/debian
mkdir -p "$R/usr/lib/systemd/system" "$R/usr/sbin" "$R/etc/systemd/system" "$R/etc/ssh" "$R/run"
for f in openssh-server/data/usr/lib/systemd/system/ssh.service openssh-server/data/usr/lib/systemd/system/ssh.socket \
         cron/data/usr/lib/systemd/system/cron.service rsyslog/data/usr/lib/systemd/system/rsyslog.service \
         logrotate/data/usr/lib/systemd/system/logrotate.service logrotate/data/usr/lib/systemd/system/logrotate.timer; do
  command cp "$X/$f" "$R/usr/lib/systemd/system/"
done
for d in sshd cron rsyslogd logrotate; do command cp "$B/bin/svctest" "$R/usr/sbin/$d"; done
# ALL=1: every unit file of the sample (for timing how long loading them takes)
if [ "$ALL" = 1 ]; then
  for f in $(find "$X" -path '*/data/*systemd/system/*' -type f \( -name '*.service' -o -name '*.socket' -o -name '*.timer' -o -name '*.path' -o -name '*.target' -o -name '*.slice' \)); do
    command cp "$f" "$R/usr/lib/systemd/system/" 2>/dev/null || true
  done
fi
tar --format=ustar --owner=0 --group=0 -C "$O/root" -cf "$O/initrd.tar" .
echo "image: $O ($(du -k "$O/initrd.tar" | cut -f1) KiB)"
