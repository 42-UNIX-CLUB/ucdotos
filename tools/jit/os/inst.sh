# SPDX-License-Identifier: GPL-2.0-or-later
# Inside UC.OS (image from tools/jit/os/mkimage.sh): install the test set from /repo, with
# maintainer scripts skipped. Run once with --store; the tests then reuse that file system.
mkdir -p /etc/dpkg/dpkg.cfg.d /debian/etc/apt
printf "native-architecture riscv64\nforce-unrunnable-scripts\n" > /etc/dpkg/dpkg.cfg.d/integ
echo "deb file:/repo trixie main" > /debian/etc/apt/sources.list
apt update > /tmp/u.txt 2>&1; echo "update exit $?"
apt install -y hello base-files base-passwd dash debianutils perl-base mawk coreutils sed grep findutils diffutils gzip tar bash libc-bin dpkg busybox-static > /tmp/i.txt 2>&1
echo "install exit $?"
dpkg -l | grep -c '^ii'
