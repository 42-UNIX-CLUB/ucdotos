# SPDX-License-Identifier: GPL-2.0-or-later
# Inside UC.OS, after inst.sh: time one command ($T) with the interpreter and with the
# translator, alternating, $N times each. Whole command: jtime measures from spawn to exit,
# so the engine's start-up is included. Lines "T MODE jtime MS ms" go to the console.
export PATH=$PATH:/debian/usr/bin
W=/debian/tmp/w
mkdir -p $W
cp $(find /repo -name 'perl-base_*.deb' | head -1) $W/p.deb
case $T in
  sha256) cat $W/p.deb $W/p.deb $W/p.deb $W/p.deb $W/p.deb $W/p.deb > $W/big ;;   # 10 MB
  sort|mawk) lx /usr/bin/mawk 'BEGIN { srand(1); for (i = 0; i < 100000; i++) printf "%d line %d\n", int(rand() * 1000000), i }' > $W/lines ;;
  xz|gzip) (cd $W && lx /usr/bin/busybox ar x /tmp/w/p.deb data.tar.xz) ;;
esac
[ "$T" = gzip ] && lx /usr/bin/busybox xz -dc /tmp/w/data.tar.xz > $W/data.tar
ls -l $W | head -5
run() {
  case $T in
    hello) jtime lx /usr/bin/hello ;;
    lsR) jtime lx /usr/bin/ls -lR /usr ;;
    bbtrue) jtime lx /usr/bin/busybox sh -c true ;;
    dpkgl) jtime lx /usr/bin/dpkg -l ;;
    sha256) jtime lx /usr/bin/sha256sum /tmp/w/big ;;
    gzip) jtime lx /usr/bin/gzip -n -c /tmp/w/data.tar ;;
    xz) jtime lx /usr/bin/busybox xz -dc /tmp/w/data.tar.xz ;;
    perl) jtime lx /usr/bin/perl -e 'my $s = 0; for my $i (1 .. 1000000) { $s += $i * 2 } print "$s\n"' ;;
    sort) jtime lx /usr/bin/sort -n /tmp/w/lines ;;
    mawk) jtime lx /usr/bin/mawk '{ s += $1; c[$2]++ } END { print s, c["line"] }' /tmp/w/lines ;;
    apt) jtime apt reinstall -y debianutils ;;
  esac
}
printf "native-architecture riscv64\nscript-runner /bin/lx\n" > /etc/dpkg/dpkg.cfg.d/integ
# SIDES: names among off, off2 (the interpreter again: the control), on, on500 (threshold 500),
# onsplit (split=1). Each cycle runs them forward then backward (ABBA), $N cycles.
SIDES=${SIDES:-"off on off2"}
REV=""; for x in $SIDES; do REV="$x $REV"; done
side() {
  case $1 in off|off2) m=off; o= ;; on|on2) m=on; o= ;; on500) m=on; o=threshold=500 ;; onsplit) m=on; o=split=1 ;; onsmall) m=on; o="maxinsns=128 maxblocks=32" ;; onsmall500) m=on; o="maxinsns=128 maxblocks=32 threshold=500" ;; esac
  LX_JIT=$m LX_JIT_OPTS=$o run > /dev/null 2> /tmp/e.txt
  echo "$T $1 $(grep jtime /tmp/e.txt)"
}
i=0
while [ $i -lt $N ]; do
  for x in $SIDES; do side $x; done
  for x in $REV; do side $x; done
  i=$((i+1))
done
