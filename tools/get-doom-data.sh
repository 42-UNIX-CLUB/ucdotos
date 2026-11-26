#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Download Doom's two free data files into bin/doom/wads/ (git-ignored) and check them
# against pinned SHA-256 sums. The build (tools/remote-files.sh) then serves them beside the
# image, fetched by the OS the first time Doom opens one.
#   sh tools/get-doom-data.sh [DIR]          (default bin/doom/wads)
#
# freedoom1.wad: Freedoom Phase 1, version 0.13.0, BSD licence, from the Freedoom project's
#   own release on GitHub. Its licence (COPYING.txt) is kept as freedoom-COPYING.txt: the
#   licence must go wherever the file goes, and the build copies it beside the file.
# doom1.wad: the shareware episode of Doom, version 1.9 (id Software; "freely
#   distributable" in John Carmack's words, quoted in Debian's copyright file; not free
#   software). id Software no longer hosts it. It is taken from Debian's archive, package
#   doom-wad-shareware 1.9.fixed-5 (non-free), which ships the file unmodified: its SHA-256
#   is the well-known one of the 1.9 shareware file. Any copy with the same bytes will do:
#   set DOOM1_URL to a plain doom1.wad elsewhere.
# Needs curl (or wget), bsdtar (or unzip, ar and tar with xz) and sha256sum.
set -e
D=${1:-bin/doom/wads}
DOOM1_DEB=https://deb.debian.org/debian/pool/non-free/d/doom-wad-shareware/doom-wad-shareware_1.9.fixed-5_all.deb
DOOM1_SHA=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
FREEDOOM_URL=${FREEDOOM_URL:-https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip}
FREEDOOM_SHA=7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d
mkdir -p "$D"

get() { # URL FILE
  if command -v curl >/dev/null; then curl -fsSL -o "$2" "$1"; else wget -q -O "$2" "$1"; fi
}
ok() { [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ]; }
unpack() { # ZIP MEMBER_PATTERN OUT
  if command -v unzip >/dev/null; then unzip -p "$1" "$2" > "$3"; else bsdtar -xOf "$1" "$2" > "$3"; fi
}

if ok "$D/doom1.wad" $DOOM1_SHA; then echo "doom1.wad: already here"
else
  if [ -n "$DOOM1_URL" ]; then get "$DOOM1_URL" "$D/doom1.wad.part"
  else
    get "$DOOM1_DEB" "$D/doom1.deb.part"
    if command -v bsdtar >/dev/null; then bsdtar -xOf "$D/doom1.deb.part" 'data.tar*' | bsdtar -xOf - ./usr/share/games/doom/doom1.wad > "$D/doom1.wad.part"
    else (cd "$D" && ar p doom1.deb.part data.tar.xz | tar -xJOf - ./usr/share/games/doom/doom1.wad) > "$D/doom1.wad.part"; fi
    rm "$D/doom1.deb.part"
  fi
  if ok "$D/doom1.wad.part" $DOOM1_SHA; then mv "$D/doom1.wad.part" "$D/doom1.wad"; echo "doom1.wad: ok"
  else echo "doom1.wad: the download does not match the pinned SHA-256; left as $D/doom1.wad.part" >&2; exit 1; fi
fi

if ok "$D/freedoom1.wad" $FREEDOOM_SHA && [ -f "$D/freedoom-COPYING.txt" ]; then echo "freedoom1.wad: already here"
else
  get "$FREEDOOM_URL" "$D/freedoom.zip.part"
  unpack "$D/freedoom.zip.part" '*/freedoom1.wad' "$D/freedoom1.wad.part"
  unpack "$D/freedoom.zip.part" '*/COPYING.txt' "$D/freedoom-COPYING.txt"
  if ok "$D/freedoom1.wad.part" $FREEDOOM_SHA; then mv "$D/freedoom1.wad.part" "$D/freedoom1.wad"; echo "freedoom1.wad: ok"
  else echo "freedoom1.wad: the file in $FREEDOOM_URL does not match the pinned SHA-256; left as $D/freedoom1.wad.part" >&2; exit 1; fi
  rm "$D/freedoom.zip.part"
fi
