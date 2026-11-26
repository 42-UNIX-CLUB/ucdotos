# SPDX-License-Identifier: GPL-2.0-or-later
# Inside UC.OS: the timed commands of tbench.sh, run once with $LX_JIT; their outputs'
# sizes and SHA-256 go to /tmp/out/sums for comparison between modes.
export PATH=$PATH:/debian/usr/bin
mkdir -p /tmp/out /debian/tmp/w /debian/tmp/o
W=/debian/tmp/w; O=/debian/tmp/o
cp $(find /repo -name 'perl-base_*.deb' | head -1) $W/p.deb
cat $W/p.deb $W/p.deb $W/p.deb $W/p.deb $W/p.deb $W/p.deb > $W/big
LX_JIT=off lx /usr/bin/mawk 'BEGIN { srand(1); for (i = 0; i < 100000; i++) printf "%d line %d\n", int(rand() * 1000000), i }' > $W/lines
(cd $W && LX_JIT=off lx /usr/bin/busybox ar x /tmp/w/p.deb data.tar.xz)
LX_JIT=off lx /usr/bin/busybox xz -dc /tmp/w/data.tar.xz > $W/data.tar
lx /usr/bin/hello > $O/hello
lx /usr/bin/ls -lR /usr > $O/lsR
lx /usr/bin/busybox sh -c true > $O/bbtrue; echo $? >> $O/bbtrue
lx /usr/bin/dpkg -l > $O/dpkgl
lx /usr/bin/sha256sum /tmp/w/big > $O/sha256
lx /usr/bin/gzip -n -c /tmp/w/data.tar > $O/gzip
lx /usr/bin/busybox xz -dc /tmp/w/data.tar.xz > $O/xz
lx /usr/bin/perl -e 'my $s = 0; for my $i (1 .. 1000000) { $s += $i * 2 } print "$s\n"' > $O/perl
lx /usr/bin/sort -n /tmp/w/lines > $O/sort
lx /usr/bin/mawk '{ s += $1; c[$2]++ } END { print s, c["line"] }' /tmp/w/lines > $O/mawk
for f in hello lsR bbtrue dpkgl sha256 gzip xz perl sort mawk; do echo "$f $(wc -c < $O/$f) $(LX_JIT=off lx /usr/bin/sha256sum /tmp/o/$f | cut -c1-16)"; done > /tmp/out/sums
head -3 $O/dpkgl >> /tmp/out/sums; wc -l $O/lsR $O/dpkgl >> /tmp/out/sums
