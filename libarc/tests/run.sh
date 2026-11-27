#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Native tests for libarc and the archive tools, against the host's own
# gzip, xz, zstd, GNU tar, bsdtar, binutils ar, coreutils checksums and
# Python's tarfile. Builds everything with the host compiler under
# AddressSanitizer and UndefinedBehaviorSanitizer.
#
#   bash libarc/tests/run.sh                 from the top of the tree
#   ARC_DEBS=/some/dir bash libarc/tests/run.sh   also check every *.deb there
#
# Work files go to build-arctest/run.PID (git-ignored; delete it when done).
# Exits 1 if any check fails.
set -u
top=$(cd "$(dirname "$0")/../.." && pwd)
for t in cc gzip xz zstd tar bsdtar ar python3 sha256sum sha512sum md5sum cmp diff; do
  command -v $t > /dev/null || { echo "run.sh: needs $t on the host"; exit 2; }
done
W=$top/build-arctest/run.$$
mkdir -p $W/bin $W/d
cd $W
export ASAN_OPTIONS=detect_leaks=0
CF="-O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -I$top/libarc"
cc $CF -o bin/arctool $top/libarc/tests/arctool.c $top/libarc/*.c || exit 2
for p in gzip tar ar sha256sum; do cc $CF -o bin/$p $top/bin/$p/*.c $top/libarc/*.c || exit 2; done
for a in $(cat $top/bin/gzip/ALIASES); do ln -sf gzip bin/$a; done
for a in $(cat $top/bin/sha256sum/ALIASES); do ln -sf sha256sum bin/$a; done
A=$W/bin/arctool; NB=$W/bin

n=0; fails=0
check() { n=$((n + 1)); if eval "$1"; then :; else fails=$((fails + 1)); echo "FAIL: $1"; fi; }

# ---- test data
seq 1 200000 > d/seq
head -c 1500000 /dev/urandom > d/rnd
cat $top/libarc/*.c $top/bin/*/*.c $top/kernel/*.c > d/src
cat d/src d/src d/src > d/src3
head -c 1200000 "$(command -v cc)" > d/bin
: > d/empty; printf a > d/one; head -c 600000 /dev/zero > d/zero
python3 -I -c 'import random; random.seed(7); print("".join(random.choice("ab") for _ in range(300000)))' > d/ab

echo "--- checksums"
for f in d/*; do
  for k in md5 sha256 sha512; do check "[ \"\$($A sum $k < $f)\" = \"\$(${k}sum < $f | cut -d' ' -f1)\" ]"; done
  check "[ \"\$($A sum crc32 < $f)\" = \"\$(gzip -c < $f | tail -c 8 | od -An -tx4 -N4 | tr -d ' ')\" ]"
  check "[ \"\$($A sum adler32 < $f)\" = \"\$(python3 -I -c 'import sys,zlib; print(\"%08x\" % zlib.adler32(sys.stdin.buffer.read()))' < $f)\" ]"
done
check "[ \"\$(printf '' | $A sum xxh64)\" = ef46db3751d8e999 ]"   # published XXH64 of the empty input

echo "--- gzip, zlib, deflate"
for f in d/*; do
  for l in 1 6 9; do
    gzip -$l -c $f > x.gz
    check "$A d gzip < x.gz | cmp -s - $f"
    check "$A d gzip 7 < x.gz | cmp -s - $f"       # source returns 7 bytes at a time
  done
  for l in 0 1 3 4 6 9; do
    $A c gzip $l < $f > y.gz
    check "gzip -dc y.gz | cmp -s - $f"
    check "$A d < y.gz | cmp -s - $f"
    $A c zlib $l < $f > y.zz
    check "python3 -I -c 'import sys,zlib; sys.stdout.buffer.write(zlib.decompress(sys.stdin.buffer.read()))' < y.zz | cmp -s - $f"
    check "$A d zlib < y.zz | cmp -s - $f"
    $A c deflate $l < $f > y.raw
    check "$A d deflate < y.raw | cmp -s - $f"
  done
  python3 -I -c 'import sys,zlib; sys.stdout.buffer.write(zlib.compress(sys.stdin.buffer.read(), 9))' < $f > p.zz
  check "$A d zlib < p.zz | cmp -s - $f"
done
gzip -c d/one > m.gz; gzip -c d/seq >> m.gz; gzip -c d/empty >> m.gz
check "$A d gzip < m.gz | cmp -s - <(cat d/one d/seq)"
gzip -N -c d/seq > named.gz; check "$A d < named.gz | cmp -s - d/seq"
head -c 10000 named.gz > cut.gz; check "! $A d gzip < cut.gz > /dev/null 2>&1"
cp named.gz bad.gz; printf 'Z' | dd of=bad.gz bs=1 seek=5000 conv=notrunc 2>/dev/null; check "! $A d gzip < bad.gz > /dev/null 2>&1"
cp named.gz bad2.gz; printf 'Z' | dd of=bad2.gz bs=1 seek=$(($(stat -c%s named.gz) - 3)) conv=notrunc 2>/dev/null
check "$A d gzip < bad2.gz 2>&1 >/dev/null | grep -q 'length mismatch'"
check "! $A d gzip < /dev/null > /dev/null 2>&1"
check "[ \$($A d auto < /dev/null | wc -c) -eq 0 ]"

echo "--- xz"
for f in d/*; do
  for o in "-0" "-6" "-7e" "-1 --check=crc32" "-6 --check=none" "-6 --check=sha256" "--lzma2=preset=6,lc=0,lp=2,pb=0" "--lzma2=preset=1,lc=4" "--delta=dist=2 --lzma2" "--delta=dist=256 --lzma2=preset=0" "-T4 --block-size=100000"; do
    xz $o -c $f > x.xz
    check "$A d xz < x.xz | cmp -s - $f"
    check "$A d auto 13 < x.xz | cmp -s - $f"
  done
done
xz -c d/one > c.xz; printf '\0\0\0\0' >> c.xz; xz -c d/seq >> c.xz; xz -c d/empty >> c.xz
check "$A d xz < c.xz | cmp -s - <(cat d/one d/seq)"
xz --x86 --lzma2 -c d/bin > bcj.xz; check "$A d xz < bcj.xz 2>&1 >/dev/null | grep -q BCJ"
xz -c d/src > s.xz; head -c 20000 s.xz > cut.xz; check "! $A d xz < cut.xz > /dev/null 2>&1"
cp s.xz bad.xz; printf 'Z' | dd of=bad.xz bs=1 seek=3000 conv=notrunc 2>/dev/null; check "! $A d xz < bad.xz > /dev/null 2>&1"
cp s.xz badidx.xz; printf '\x7f' | dd of=badidx.xz bs=1 seek=$(($(stat -c%s s.xz) - 16)) conv=notrunc 2>/dev/null; check "! $A d xz < badidx.xz > /dev/null 2>&1"
# a header asking for a 192 MiB dictionary (over the 128 MiB default limit), made by patching a small one
xz --lzma2=dict=4KiB -c d/one > small.xz
python3 -I -c '
import sys, zlib, struct
d = bytearray(open("small.xz", "rb").read())
hs = (d[12] + 1) * 4
i = d.index(b"\x21\x01", 14, 12 + hs)   # the LZMA2 filter: ID 0x21, one property byte
d[i + 2] = 31
d[12 + hs - 4:12 + hs] = struct.pack("<I", zlib.crc32(bytes(d[12:12 + hs - 4])))
open("big.xz", "wb").write(d)'
check "$A d xz < big.xz 2>&1 >/dev/null | grep -q 'memory limit'"

echo "--- zstd"
for f in d/*; do
  for o in "-1" "-3" "-19" "--ultra -22" "--fast=5" "-3 --no-check" "-3 --no-content-size" "--long=24 -9" "-B4096 -5"; do
    zstd -q $o -c $f > x.zst
    check "$A d zstd < x.zst | cmp -s - $f"
    check "$A d auto 7 < x.zst | cmp -s - $f"
  done
  zstd -q -c < $f > x.zst; check "$A d zstd < x.zst | cmp -s - $f"
done
zstd -q -c d/one > c.zst; printf '\x50\x2a\x4d\x18\x05\x00\x00\x00hello' >> c.zst; zstd -q -c d/seq >> c.zst
check "$A d zstd < c.zst | cmp -s - <(cat d/one d/seq)"
printf '\x5f\x2a\x4d\x18\x03\x00\x00\x00abc' > sk.zst; zstd -q -c d/one >> sk.zst; check "$A d < sk.zst | cmp -s - d/one"
mkdir -p dt; for i in $(seq 1 60); do head -c $((i * 300)) d/src | tail -c 2000 > dt/s$i; done
zstd -q --train dt/* -o dict.zd > /dev/null 2>&1; zstd -q -D dict.zd -c dt/s5 > dd.zst
check "$A d zstd < dd.zst 2>&1 >/dev/null | grep -q dictionary"
zstd -q -19 -c d/src > s.zst; head -c 20000 s.zst > cut.zst; check "! $A d zstd < cut.zst > /dev/null 2>&1"
cp s.zst bad.zst; printf 'Z' | dd of=bad.zst bs=1 seek=4000 conv=notrunc 2>/dev/null; check "! $A d zstd < bad.zst > /dev/null 2>&1"
# a frame declaring a 1 GiB window and no content size: one raw block holding one byte
printf '\x28\xb5\x2f\xfd\x00\xa0\x09\x00\x00x' > w.zst
check "$A d zstd < w.zst 2>&1 >/dev/null | grep -q 'memory limit'"
check "zstd -dc --memory=2048MiB w.zst 2>/dev/null | grep -q x"   # the host agrees the frame is valid

echo "--- tar reading"
mkdir -p src/sub/deeper
printf hello > src/a.txt; : > src/empty; cp d/bin src/sub/bin
long=$(printf 'n%.0s' $(seq 1 150)); printf x > "src/sub/$long"
deep=src/sub/deeper/$(printf 'd%.0s' $(seq 1 90))/$(printf 'e%.0s' $(seq 1 90)); mkdir -p $deep; printf y > $deep/f
ln -sf a.txt src/link; ln -f src/a.txt src/hard; ln -sf "$long" src/sub/longlink
touch -d '2001-02-03 04:05:06' src/a.txt
cat > pylist.py <<'EOF'
import sys, tarfile
t = tarfile.open(fileobj=sys.stdin.buffer, mode="r|*")
for m in t:
    ty = {tarfile.REGTYPE: '0', tarfile.AREGTYPE: '0', tarfile.LNKTYPE: '1', tarfile.SYMTYPE: '2', tarfile.CHRTYPE: '3',
          tarfile.BLKTYPE: '4', tarfile.DIRTYPE: '5', tarfile.FIFOTYPE: '6', tarfile.CONTTYPE: '0'}.get(m.type, m.type.decode())
    line = "%s %o %d %d %d %d %s" % (ty, m.mode & 0o7777, m.uid, m.gid, m.size, int(m.mtime), m.name)
    if m.linkname:
        line += " -> " + m.linkname
    print(line)
EOF
for fmt in gnu pax oldgnu; do
  tar --format=$fmt -cf x-$fmt.tar src
  check "[ \"\$($A tar < x-$fmt.tar | sed 's,/\$,,')\" = \"\$(python3 -I pylist.py < x-$fmt.tar | sed 's,/\$,,')\" ]"
  check "$A tarcat src/sub/bin < x-$fmt.tar | cmp -s - src/sub/bin"
  for z in gzip xz zstd; do $z -c x-$fmt.tar > x.tz; check "[ \"\$($A tar < x.tz)\" = \"\$($A tar < x-$fmt.tar)\" ]"; done
done
tar --format=ustar -cf u.tar src/sub/deeper src/a.txt; check "[ \"\$($A tar < u.tar | sed 's,/\$,,')\" = \"\$(python3 -I pylist.py < u.tar | sed 's,/\$,,')\" ]"
tar --format=v7 -cf v.tar src/a.txt src/link; check "[ \"\$($A tar < v.tar)\" = \"\$(python3 -I pylist.py < v.tar)\" ]"
bsdtar --format=pax -cf b.tar src; check "[ \"\$($A tar < b.tar | sed 's,/\$,,')\" = \"\$(python3 -I pylist.py < b.tar | sed 's,/\$,,')\" ]"
truncate -s 10M sp; printf z >> sp; tar --sparse -cf sp.tar sp; check "$A tar < sp.tar 2>&1 | grep -q sparse"
python3 -I -c '
import tarfile, io
t = tarfile.open("bigpax.tar", "w", format=tarfile.PAX_FORMAT)
i = tarfile.TarInfo("f"); i.size = 3; i.pax_headers = {"path": "x" * 300, "uid": "123456789", "mtime": "-12.5"}; t.addfile(i, io.BytesIO(b"abc"))
t.close()'
check "[ \"\$($A tar < bigpax.tar)\" = \"0 644 123456789 0 3 -12 \$(printf 'x%.0s' \$(seq 1 300))\" ]"
head -c 700 x-gnu.tar > cut.tar; check "! $A tar < cut.tar > /dev/null 2>&1"
cp x-gnu.tar badsum.tar; printf 'Q' | dd of=badsum.tar bs=1 seek=10 conv=notrunc 2>/dev/null; check "$A tar < badsum.tar 2>&1 | grep -q checksum"

echo "--- ar reading"
printf 'x' > m1; cp d/bin m2; printf 'abc' > averyveryverylongmembername.txt
ar rc t.a m1 m2 averyveryverylongmembername.txt 2>/dev/null
check "[ \"\$($A ar < t.a | cut -d' ' -f1,4)\" = \"\$(for m in m1 m2 averyveryverylongmembername.txt; do echo \$m \$(sha256sum < \$m | cut -d' ' -f1); done)\" ]"
bsdtar --format=ar -cf b.a m1 averyveryverylongmembername.txt 2>/dev/null && check "[ \"\$($A ar < b.a | cut -d' ' -f1)\" = \"m1
averyveryverylongmembername.txt\" ]"

echo "--- tools"
cp d/src a.orig; cp d/bin b.bin
cp a.orig a.txt; $NB/gzip -k a.txt; check "[ -f a.txt.gz ] && gzip -dc a.txt.gz | cmp -s - a.txt"
check "$NB/gzip -t a.txt.gz"
rm -f a.txt; $NB/gunzip a.txt.gz; check "[ ! -f a.txt.gz ] && cmp -s a.txt a.orig"
gzip -9 -c b.bin > b.gz; check "$NB/zcat b.gz | cmp -s - b.bin"
check "! $NB/gzip -d b.bin 2>/dev/null"
check "$NB/zcat -f b.bin | cmp -s - b.bin"
cat b.gz b.gz > bb.gz; check "$NB/zcat bb.gz | cmp -s - <(cat b.bin b.bin)"
check "$NB/gzip -l b.gz | grep -q ' b\$'"
xz -c a.orig > a.xz; check "$NB/xzcat a.xz | cmp -s - a.orig"
check "$NB/xz -t a.xz && ! $NB/xz a.orig 2>/dev/null"
cp a.xz c.xz; $NB/unxz c.xz; check "cmp -s c a.orig && [ ! -f c.xz ]"
zstd -q -c a.orig > a.zst; check "$NB/zstdcat a.zst | cmp -s - a.orig"
cp a.zst e.zst; $NB/unzstd -q e.zst; check "cmp -s e a.orig && [ -f e.zst ]"
check "$NB/zstdcat a.orig | cmp -s - a.orig"
check "$NB/zstd -d -c -M1K a.zst 2>&1 >/dev/null | grep -q limit"
for k in md5 sha256 sha512; do
  check "[ \"\$($NB/${k}sum a.orig b.bin)\" = \"\$(${k}sum a.orig b.bin)\" ]"
  ${k}sum a.orig b.bin > sums.$k; check "$NB/${k}sum -c sums.$k > /dev/null"
  check "[ \"\$($NB/${k}sum --tag a.orig)\" = \"\$(${k}sum --tag a.orig)\" ]"
done
cp b.bin b2.bin; sha256sum b2.bin > s2; printf x >> b2.bin
check "! $NB/sha256sum -c s2 > out.txt 2>&1 && grep -q 'b2.bin: FAILED' out.txt"
touch 'we\ird'; check "[ \"\$($NB/sha256sum 'we\\ird')\" = \"\$(sha256sum 'we\\ird')\" ]"
printf 'hi' > n1; printf 'abc' > n2; printf 'x%.0s' $(seq 1 30) > averyveryverylongmembername
$NB/ar rc t2.a n1 n2 averyveryverylongmembername; check "[ \"\$(ar t t2.a | tr '\n' ' ')\" = 'n1 n2 averyveryverylongmembername ' ]"
check "[ \"\$(ar p t2.a n2)\" = abc ] && [ \"\$($NB/ar p t2.a n2)\" = abc ]"
mkdir arx; (cd arx && $NB/ar x ../t2.a); check "cmp -s arx/averyveryverylongmembername averyveryverylongmembername"
printf 'new' > n1; $NB/ar r t2.a n1; check "[ \"\$(ar p t2.a n1)\" = new ] && [ \"\$(ar t t2.a | head -1)\" = n1 ]"
$NB/ar d t2.a n2; check "[ \"\$(ar t t2.a | tr '\n' ' ')\" = 'n1 averyveryverylongmembername ' ]"
$NB/tar -cf x.tar src; check "[ \"\$(tar -tf x.tar | sort)\" = \"\$($NB/tar -tf x.tar | sort)\" ]"
mkdir g1; tar -xf x.tar -C g1; check "diff -r src g1/src > /dev/null && [ \"\$(readlink g1/src/link)\" = a.txt ]"
check "[ \"\$(stat -c %i g1/src/a.txt)\" = \"\$(stat -c %i g1/src/hard)\" ] && [ \$((\$(stat -c %s x.tar) % 10240)) -eq 0 ]"
$NB/tar -czf x.tgz src; check "gzip -t x.tgz && tar -tzf x.tgz > /dev/null"
check "! $NB/tar -cJf y.txz src 2>/dev/null"
for fmt in gnu pax oldgnu; do
  for z in "" gzip xz zstd; do
    f=x-$fmt.tar; if [ -n "$z" ]; then $z -c x-$fmt.tar > x-$fmt.tar.$z; f=x-$fmt.tar.$z; fi
    o=e-$fmt-$z; mkdir $o; $NB/tar -xf $f -C $o 2>/dev/null
    check "diff -r src $o/src > /dev/null && [ \"\$(readlink $o/src/link)\" = a.txt ] && [ \"\$(stat -c %Y $o/src/a.txt)\" = \"\$(stat -c %Y src/a.txt)\" ]"
  done
done
mkdir strip2; $NB/tar -xf x-gnu.tar -C strip2 --strip-components=2 2>/dev/null; check "[ -f strip2/bin ] && [ ! -e strip2/src ]"
mkdir s3; $NB/tar -xf x-gnu.tar -C s3 src/sub; check "[ -f s3/src/sub/bin ] && [ ! -e s3/src/a.txt ]"
python3 -I -c '
import tarfile, io
t = tarfile.open("evil.tar", "w")
for n in ["../escape", "/abs/file", "ok/../../up", "fine"]:
    i = tarfile.TarInfo(n); i.size = 1; t.addfile(i, io.BytesIO(b"x"))
i = tarfile.TarInfo("lnk"); i.type = tarfile.SYMTYPE; i.linkname = ".."; t.addfile(i)
i = tarfile.TarInfo("lnk/through"); i.size = 1; t.addfile(i, io.BytesIO(b"x"))
t.close()'
mkdir -p ev/in; (cd ev/in && $NB/tar -xf ../../evil.tar 2>/dev/null)
check "[ ! -e ev/escape ] && [ ! -e escape ] && [ -f ev/in/abs/file ] && [ -f ev/in/fine ] && [ ! -e ev/up ] && [ ! -e ev/through ]"

if [ -n "${ARC_DEBS:-}" ]; then
  echo "--- packages in $ARC_DEBS"
  for deb in "$ARC_DEBS"/*.deb; do
    b=$(basename "$deb" .deb); mkdir "p-$b"; cd "p-$b"
    $NB/ar x "$deb"; check "[ \"\$(ls | sort)\" = \"\$(ar t '$deb' | sort)\" ]"
    for m in control data; do
      f=$(ls $m.tar.*); mkdir ours-$m host-$m bsd-$m
      $NB/tar -xf $f -C ours-$m; tar -xf $f -C host-$m; bsdtar -xf $f -C bsd-$m
      check "diff -r --no-dereference ours-$m host-$m > /dev/null"
      check "[ \"\$(cd ours-$m && find . -mindepth 1 -printf '%p %m %y %l %T@\n' | sort)\" = \"\$(cd bsd-$m && find . -mindepth 1 -printf '%p %m %y %l %T@\n' | sort)\" ]"
      check "[ \"\$($NB/tar -tf $f)\" = \"\$(tar -tf $f)\" ]"
    done
    cd ..
  done
fi

echo "libarc native tests: $n checks, $fails failed (work files in $W)"
[ $fails -eq 0 ]
