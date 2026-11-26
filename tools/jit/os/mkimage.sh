#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The translator's test image: build-NAME/initrd.tar plus a Debian package repository at
# /repo, the engine's M2/M34 scripts in the guest root, and the scripts of tools/jit/os.
#   sh tools/jit/os/mkimage.sh build-NAME REPO_DIR   ->  build-NAME/jit-os/initrd.tar
set -eu
B=$1; REPO=$2
S=$B/jit-os/stage
mkdir -p $S/repo $S/debian/lx $S/t
command cp -r "$REPO/." $S/repo/
command cp bin/lx/tests/guest/m2.sh bin/lx/tests/guest/m34.sh $S/debian/lx/
command cp tools/jit/os/*.sh $S/t/
command cp $B/initrd.tar $B/jit-os/initrd.tar
tar --format=ustar --owner=0 --group=0 -C $S -rf $B/jit-os/initrd.tar ./repo ./debian/lx ./t
echo "image $B/jit-os/initrd.tar: $(wc -c < $B/jit-os/initrd.tar) bytes"
