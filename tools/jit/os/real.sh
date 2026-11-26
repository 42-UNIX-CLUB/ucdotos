# SPDX-License-Identifier: GPL-2.0-or-later
# Inside UC.OS, after inst.sh: Debian's own programs under the engine. Every output goes to
# /tmp/out/NAME so the interpreter's and the translator's runs can be compared file by file.
# LX_JIT (off, on, lockstep) is inherited by every engine started, also through spawn.
mkdir -p /tmp/out /debian/tmp/w   # /tmp/w inside the guest root (/debian)
o=/tmp/out
lx /usr/bin/hello > $o/hello 2>&1; echo "hello $?" >> $o/status
lx /usr/bin/busybox sh /lx/m2.sh /usr/bin/busybox /tmp > $o/m2 2>&1; echo "m2 $?" >> $o/status
lx /bin/dash /lx/m34.sh > $o/m34 2>&1; echo "m34 $?" >> $o/status
printf 'b 2\na 1\nc 3\nb 9\n' > /debian/tmp/in.txt
echo "x y z" | lx /usr/bin/mawk '{ for (i = NF; i > 0; i--) printf "%s ", $i; print NR }' > $o/mawk 2>&1; echo "mawk $?" >> $o/status
lx /usr/bin/mawk '{ s[$1] += $2 } END { for (k in s) print k, s[k] }' /tmp/in.txt | sort > $o/mawk2 2>&1
lx /bin/dash -c 'for i in 1 2 3; do echo "n $i $(echo $i | /usr/bin/sed s/1/one/)"; done | /usr/bin/sort -r; x=$(/usr/bin/cat /tmp/in.txt | /usr/bin/wc -l); echo "lines $x"' > $o/dash 2>&1; echo "dash $?" >> $o/status
lx /usr/bin/ls -lR /usr/share/doc/hello /usr/lib/riscv64-linux-gnu/perl-base/File > $o/lsR 2>&1; echo "ls $?" >> $o/status
lx /usr/bin/sort -k2n /tmp/in.txt > $o/sort 2>&1; echo "sort $?" >> $o/status
lx /usr/bin/sha256sum /usr/bin/perl /usr/bin/bash > $o/sha 2>&1; echo "sha $?" >> $o/status
lx /usr/bin/perl -e 'my %h; $h{$_ % 7} += $_ for 1..20000; print join(",", map { "$_=$h{$_}" } sort keys %h), "\n"; printf "%.6f\n", atan2(1,1)*4' > $o/perl 2>&1; echo "perl $?" >> $o/status
lx /usr/bin/cp /usr/bin/bash /tmp/w/b; lx /usr/bin/gzip -9 /tmp/w/b; lx /usr/bin/gzip -d /tmp/w/b.gz; lx /usr/bin/cmp /tmp/w/b /usr/bin/bash > $o/gzip 2>&1; echo "gzip $?" >> $o/status
lx /usr/bin/sha256sum /tmp/w/b.gz /tmp/w/b >> $o/gzip 2>&1
px=$(find /repo -name Packages.xz | head -1); cp $px /debian/tmp/w/P.xz
lx /usr/bin/busybox xz -dc /tmp/w/P.xz > /debian/tmp/w/P 2> $o/xz; echo "xz $?" >> $o/status; lx /usr/bin/sha256sum /tmp/w/P >> $o/xz 2>&1; lx /usr/bin/wc -l /tmp/w/P >> $o/xz 2>&1
deb=$(find /repo -name 'hello_*.deb' | head -1)
cp $deb /debian/tmp/hello.deb
lx /usr/bin/dpkg-deb --info /tmp/hello.deb > $o/debinfo 2>&1; echo "dpkg-deb $?" >> $o/status
lx /usr/bin/bash -c 'declare -A m; for w in one two three two one one; do m[$w]=$(( ${m[$w]:-0} + 1 )); done; for k in one three two; do echo "$k ${m[$k]}"; done' > $o/bash 2>&1; echo "bash $?" >> $o/status
cat $o/status
