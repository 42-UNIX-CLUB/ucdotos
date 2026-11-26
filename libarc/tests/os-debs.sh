#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Extract real .deb packages inside UC.OS and compare the result with the
# host's GNU tar extraction of the same packages.
#   sh libarc/tests/os-debs.sh BUILD DEB...
# BUILD is a build directory with gzip, tar, ar, sha256sum, sh, mkdir, cat in
# its image. For each package the OS runs `ar x`, then `tar -xf data.tar.*`,
# then hashes the contents of every regular file in archive order; the host
# does the same with GNU tar and its own sha256sum. The two lines must match.
# Makes BUILD-debs/ (same kernel, initrd plus /debs); nothing in BUILD changes.
set -e
B=$1; shift
top=$(cd "$(dirname "$0")/../.." && pwd)
cd "$top"
O=$B-debs
mkdir -p $O/stage/debs $O/host
for f in "$@"; do cp "$f" $O/stage/debs/; done
cp $B/kernel.wasm $O/
cp $B/initrd.tar $O/initrd.tar
tar --format=ustar --owner=0 --group=0 -C $O/stage -rf $O/initrd.tar ./debs
cat > $O/stage/check.sh <<'EOF'
for d in /debs/*.deb; do
  n=${d##*/}
  mkdir /tmp/$n
  cd /tmp/$n
  ar x $d
  mkdir root
  tar -xf data.tar.* -C root 2>/dev/null
  cd root
  h=$(tar -tvf ../data.tar.* | while read m o s dt tm p rest; do case $m in -*|h*) cat "$p";; esac; done | sha256sum)
  echo "$n $h"
  cd /
done
EOF
tar --format=ustar --owner=0 --group=0 -C $O/stage -rf $O/initrd.tar ./check.sh
node tools/run.mjs --build=$O --init=/bin/sh --input='sh /check.sh\nexit\n' 2>&1 | grep '\.deb ' > $O/os.txt || true
: > $O/host.txt
for f in "$@"; do
  n=$(basename "$f")
  mkdir -p $O/host/$n/root
  (cd $O/host/$n && ar x "$f" && tar -xf data.tar.* -C root &&
   cd root && h=$(tar -tvf ../data.tar.* | while read m o s dt tm p rest; do case $m in -*|h*) cat "$p";; esac; done | sha256sum) &&
   echo "$n $h") >> $O/host.txt
done
sort -o $O/os.txt $O/os.txt; sort -o $O/host.txt $O/host.txt
if cmp -s $O/os.txt $O/host.txt; then echo "os-debs: $(wc -l < $O/host.txt) packages, all identical"; else echo "os-debs: DIFFERENT"; diff $O/os.txt $O/host.txt; exit 1; fi
