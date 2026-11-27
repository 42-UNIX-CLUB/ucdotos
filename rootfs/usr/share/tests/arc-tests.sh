# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for gzip, xz, zstd, tar, ar and the checksum tools (libarc).
# Run:  sh /usr/share/tests/arc-tests.sh
# Needs in the image: sh, echo, cat, cp, ls, mkdir, rm, head, seq, wc and the
# tools under test. Fixtures are in /usr/share/tests/arc (a few KiB).
# Every check prints "ok" or "FAIL"; the last line is the count.

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
h() { set -- $(sha256sum "$1"); echo $1; }        # sha256 of a file
hs() { set -- $(sha256sum); echo $1; }            # sha256 of standard input
F=/usr/share/tests/arc
SEQ=6251e5743b6fd6a7d606130bdf7c15077ce85ebd3a0fdee284d15a46df199e38
HELLO=a97d4b4ade99f266fc960a29ea623dce8c843cf8fad597cc783464773755aa9a
T=/tmp/arc-tests.$$
mkdir -p $T
cd $T

echo "--- checksums"
printf abc > abc
check "sha256 of abc" "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc" "$(sha256sum abc)"
check "md5 of abc" "900150983cd24fb0d6963f7d28e17f72  abc" "$(md5sum abc)"
check "sha512 of abc" "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f  abc" "$(sha512sum abc)"
: > empty
check "sha256 of nothing" "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  empty" "$(sha256sum empty)"
seq 1 3000 > s1
sha256sum abc s1 > list
check "sha256sum -c" "abc: OK
s1: OK" "$(sha256sum -c list)"
echo changed >> s1
sha256sum -c list > out 2>/dev/null
check "sha256sum -c finds a change" "1 abc: OK
s1: FAILED" "$? $(cat out)"

echo "--- decompression of files made by GNU gzip, xz and zstd"
check "zcat" $SEQ "$(zcat $F/seq.txt.gz | hs)"
check "gzip -dc" $SEQ "$(gzip -dc < $F/seq.txt.gz | hs)"
check "two gzip members" "Hello from the UNIX Club.
Hello from the UNIX Club." "$(zcat $F/twice.gz)"
check "xzcat (CRC64)" $SEQ "$(xzcat $F/seq.txt.xz | hs)"
check "xz with a SHA-256 check" $HELLO "$(xz -dc $F/hello-sha256.xz | hs)"
check "zstdcat" $SEQ "$(zstdcat $F/seq.txt.zst | hs)"
check "zstd -dc" $SEQ "$(zstd -dc $F/seq.txt.zst | hs)"
cp $F/seq.txt.xz a.xz; unxz a.xz
check "unxz makes the file and removes the input" "$SEQ no" "$(h a) $(ls a.xz 2>/dev/null || echo no)"
cp $F/seq.txt.zst b.zst; unzstd -q b.zst
check "unzstd keeps the input" "$SEQ b.zst" "$(h b) $(ls b.zst)"
check "gzip -t on good data" 0 "$(gzip -t $F/seq.txt.gz; echo $?)"
head -c 2000 $F/seq.txt.gz > cut.gz
check "a cut gzip file fails" 1 "$(gzip -t cut.gz 2>/dev/null; echo $?)"
head -c 600 $F/seq.txt.xz > cut.xz
check "a cut xz file fails" 1 "$(xz -t cut.xz 2>/dev/null; echo $?)"
head -c 1000 $F/seq.txt.zst > cut.zst
check "a cut zstd file fails" 1 "$(zstd -t cut.zst 2>/dev/null; echo $?)"
check "zcat refuses plain text" 1 "$(zcat abc 2>/dev/null; echo $?)"
check "zcat -f passes plain text" abc "$(zcat -f abc)"
check "xz cannot compress" 1 "$(xz abc 2>/dev/null; echo $?)"

echo "--- gzip round trips"
seq 1 20000 > big
B=$(h big)
for l in 1 6 9; do
  gzip -$l -c big > big.$l.gz
  check "gzip -$l and back" $B "$(zcat big.$l.gz | hs)"
done
cp big c; gzip c
check "gzip replaces the file" "no c.gz" "$(ls c 2>/dev/null || echo no) $(ls c.gz)"
gunzip c.gz
check "gunzip restores it" $B "$(h c)"
gzip -k c
check "gzip -k keeps the input" "c c.gz" "$(echo $(ls c c.gz))"
check "gzip refuses to overwrite" 1 "$(gzip c 2>/dev/null; echo $?)"
: > e; gzip e
check "empty file round trip" "0" "$(( $(zcat e.gz | wc -c) ))"

echo "--- tar"
mkdir -p src/sub/deeper
seq 1 500 > src/one
echo two > src/sub/two
: > src/sub/empty
seq 1 9000 > src/sub/deeper/three
tar -cf t.tar src
check "tar lists what it wrote" "src/
src/one
src/sub/
src/sub/deeper/
src/sub/deeper/three
src/sub/empty
src/sub/two" "$(tar -tf t.tar)"
mkdir x1; tar -xf t.tar -C x1
check "tar extracts the same bytes" "$(h src/sub/deeper/three) $(h src/one)" "$(h x1/src/sub/deeper/three) $(h x1/src/one)"
check "tar keeps empty files" 0 "$(( $(wc -c < x1/src/sub/empty) ))"
tar -czf t.tgz src
check "tar -z writes gzip" 0 "$(gzip -t t.tgz; echo $?)"
mkdir x2; tar -xzf t.tgz -C x2
check "tar -xz" "$(h src/sub/two)" "$(h x2/src/sub/two)"
mkdir x3; tar -xf t.tgz -C x3 --strip-components=2
check "strip-components" "$(h src/sub/two)" "$(h x3/two)"
mkdir x4; cd x4; tar xf ../t.tar src/sub/two; cd ..
check "old-style options and one member" "two" "$(echo $(cat x4/src/sub/two) $(ls x4/src/one 2>/dev/null))"
check "tar -O" "two" "$(tar -xOf t.tar src/sub/two)"

echo "--- a package: ar outside, tar inside, as in a .deb"
check "ar t" "debian-binary
control.tar.gz
data.tar.zst" "$(ar t $F/clubhello.deb)"
check "ar p" "2.0" "$(ar p $F/clubhello.deb debian-binary)"
mkdir deb; cd deb; ar x $F/clubhello.deb; cd ..
check "ar x" "control.tar.gz data.tar.zst debian-binary" "$(echo $(ls deb))"
check "control file" "Package: clubhello" "$(tar -xOf deb/control.tar.gz ./control | head -n 1)"
mkdir root
tar -xf deb/data.tar.zst -C root 2>warn.txt
st=$?
D=root/usr/share/doc/clubhello
check "zstd tar, pax names longer than 100 bytes" long "$(cat $D/some-long-directory-name-for-testing/another-long-directory-name/file-with-a-long-name.txt)"
check "regular file" $HELLO "$(h $D/HARD)"
check "hard link (made, or copied when links are missing)" $HELLO "$(h $D/README)"
check "script content" "#!/bin/sh" "$(head -n 1 root/usr/bin/clubhello)"
if [ -n "$(cat warn.txt)" ]; then
  # the kernel lacks some of symlink, link, chmod or utimensat: tar warned and went on
  check "tar goes on when the kernel lacks a feature" 0 $st
  tar --strict -xf deb/data.tar.zst -C root 2>/dev/null
  check "tar --strict fails when the kernel lacks a feature" 2 $?
else
  check "symbolic link exists" $D/LINK "$(ls $D/LINK)"
fi
mkdir root2; tar -xJf $F/data.tar.xz -C root2 2>/dev/null
check "xz tar" $HELLO "$(h root2/usr/share/doc/clubhello/HARD)"

echo "--- ar create"
echo first > f1; seq 1 33 > f2
ar rc new.a f1 f2 2>/dev/null
check "ar r creates" "f1
f2" "$(ar t new.a)"
check "ar p new member" "$(cat f2)" "$(ar p new.a f2)"
echo again > f1; ar r new.a f1
check "ar r replaces" "again" "$(ar p new.a f1)"

cd /
rm -r $T
echo "arc-tests: $((pass + fail)) checks, $pass passed, $fail failed"
