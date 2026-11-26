#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Files that stay on the web server until a program opens them (kernel/remote.c).
#   sh tools/remote-files.sh BUILD_DIR [WAD_DIR]      (normally through make; WAD_DIR: make WADS=...)
# Writes:
#   BUILD/files/...              the files themselves, plus NAME.gz when gzip makes them smaller
#   BUILD/root/etc/remote-files  the manifest the kernel reads at boot: PATH SIZE SHA256 URL [gz]
# Doom's game data: every *.wad in WAD_DIR (default bin/doom/wads/, git-ignored;
# tools/get-doom-data.sh fetches the two free ones) becomes /usr/share/games/doom/NAME.
# A directory without data still builds: the manifest then lists only the small test
# files that bin/ktest.c uses.
set -e
B=${1:?usage: remote-files.sh BUILD_DIR [WAD_DIR]}
W=${2:-bin/doom/wads}
F=$B/files
M=$B/root/etc/remote-files
mkdir -p "$F/test" "$B/root/etc" "$B/root/usr/share/games/doom" "$B/root/usr/share/applications"
tmp=$M.new
echo "# Remote files: PATH SIZE SHA256 URL [gz]. Written by tools/remote-files.sh; read by kernel/remote.c." > "$tmp"

# add OS_PATH SOURCE URL: copy SOURCE to BUILD/URL, gzip it if that helps, add the manifest line
add() {
  dst=$F/${3#files/}
  [ -f "$dst" ] && cmp -s "$2" "$dst" || command cp "$2" "$dst"
  size=$(wc -c < "$dst" | tr -d ' ')
  sum=$(sha256sum "$dst" | cut -d' ' -f1)
  gz=
  magic=$(head -c 2 "$dst" | od -An -tx1 | tr -d ' \n')
  if [ "$magic" != 1f8b ] && [ "$size" -gt 4096 ]; then
    [ "$dst.gz" -nt "$dst" ] || gzip -9 -n -c "$dst" > "$dst.gz"
    [ "$(wc -c < "$dst.gz")" -lt "$size" ] && gz=gz
  fi
  echo "$1 $size $sum $3${gz:+ $gz}" >> "$tmp"
}

# ---- Doom ----
for w in "$W"/*.wad; do
  [ -f "$w" ] || continue
  n=$(basename "$w")
  add /usr/share/games/doom/$n "$w" files/$n
done
# Freedoom's licence must go wherever the file goes: beside it on the server and in the image.
if [ -f "$W/freedoom1.wad" ] && [ -f "$W/freedoom-COPYING.txt" ]; then
  command cp "$W/freedoom-COPYING.txt" "$F/freedoom-COPYING.txt"
  command cp "$W/freedoom-COPYING.txt" "$B/root/usr/share/games/doom/freedoom-COPYING.txt"
  printf 'name=Freedoom\nexec=doom -iwad freedoom1.wad -fullscreen\ncomment=Doom with free game data\n' > "$B/root/usr/share/applications/freedoom.app"
else # an earlier build with Freedoom left these in the staging directory
  for f in "$B/root/usr/share/applications/freedoom.app" "$B/root/usr/share/games/doom/freedoom-COPYING.txt"; do [ -f "$f" ] && rm "$f"; done
fi

# ---- test files for bin/ktest.c (tools/remote-node.mjs knows the .once. .flaky. .slow. names) ----
T=/usr/share/tests/remote
mk() { [ -f "$F/test/$1" ] || printf '%s\n' "$2" > "$F/test/$1"; }
mk hello.once.txt 'fetched on first open'
mk retry.flaky.txt 'second try works'
mk both.once.slow.txt 'two openers, one fetch'
mk nb.slow.txt 'not waiting'
mk never.txt 'never opened'
mk bad.txt 'the hash in the manifest is wrong'
[ -f "$F/test/packed.txt" ] || awk 'BEGIN { for (i = 0; i < 2000; i++) printf "line %d of a file sent compressed\n", i }' > "$F/test/packed.txt"
for t in hello.once retry.flaky both.once.slow nb.slow never packed; do add $T/$t.txt "$F/test/$t.txt" files/test/$t.txt; done
echo "$T/bad.txt $(wc -c < "$F/test/bad.txt" | tr -d ' ') 0000000000000000000000000000000000000000000000000000000000000000 files/test/bad.txt" >> "$tmp"
echo "$T/missing.txt 100 $(printf x | sha256sum | cut -d' ' -f1) files/test/missing.txt" >> "$tmp"

if cmp -s "$tmp" "$M"; then rm "$tmp"; else mv "$tmp" "$M"; echo "  REMOTE $(grep -vc '^#' "$M") files in /etc/remote-files"; fi
