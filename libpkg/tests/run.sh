#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Native test suite for libpkg and the package tools. Needs: cc, tar, ar, gzip,
# xz, zstd, sha256sum (python3 is optional). No network.
#   sh libpkg/tests/run.sh [OUTDIR]
# Builds into OUTDIR (default build-pkgnative/, git-ignored), makes small .deb
# files from text with tar and ar, and runs dpkg, dpkg-deb, dpkg-query and apt
# against scratch roots under OUTDIR/runs/. Exits 1 if any check fails.
# Each run uses a new directory under OUTDIR/runs/; nothing is deleted.
set -u
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build-pkgnative}
case $out in /*) ;; *) out=$(pwd)/$out ;; esac # the tests change directory, so the path must be absolute
sh "$top/libpkg/tests/build-native.sh" "$out" >/dev/null || { echo "build failed"; exit 1; }
BIN=$out/bin
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
W=$out/runs/$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$W"
LOG=$W/test.log
: > "$LOG"
pass=0
fail=0

note() { printf '%s\n' "$*" >> "$LOG"; }
check() { # description command...
  d=$1; shift
  note "== $d: $*"
  if "$@" >> "$LOG" 2>&1; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $d"; fi
}
checknot() {
  d=$1; shift
  note "== (expect failure) $d: $*"
  if "$@" >> "$LOG" 2>&1; then fail=$((fail + 1)); echo "FAIL: $d (succeeded, should fail)"; else pass=$((pass + 1)); fi
}
has() { grep -q -e "$2" "$1"; }
hasnot() { ! grep -q -e "$2" "$1"; }
exists() { [ -e "$1" ]; }
missing() { [ ! -e "$1" ]; }
same() { [ "$(cat "$1")" = "$2" ]; }
status_of() { # root pkg -> the Status line
  awk -v p="$2" '$1=="Package:"{n=$2} $1=="Status:"&&n==p{print $2, $3, $4}' "$1/var/lib/dpkg/status"
}
st() { [ "$(status_of "$1" "$2")" = "$3" ]; }

# ---------- making packages ----------
S=$W/src
P=$W/pool
mkdir -p "$S" "$P"
# pkg NAME VERSION ARCH [control lines...]: start a package source tree
pkg() {
  n=$1 v=$2 a=$3; shift 3
  d=$S/${n}_$v
  mkdir -p "$d/DEBIAN"
  {
    printf 'Package: %s\nVersion: %s\nArchitecture: %s\nMaintainer: Test <test@example.invalid>\n' "$n" "$v" "$a"
    for l in "$@"; do printf '%s\n' "$l"; done
    printf 'Description: test package %s\n fixture for the libpkg tests.\n' "$n"
  } > "$d/DEBIAN/control"
  CUR=$d
}
file() { # path content [mode]
  mkdir -p "$(dirname "$CUR/$1")"
  printf '%s\n' "$2" > "$CUR/$1"
  chmod "${3:-644}" "$CUR/$1"
}
ctlfile() { printf '%s\n' "$2" > "$CUR/DEBIAN/$1"; chmod 644 "$CUR/DEBIAN/$1"; }
# script NAME [extra shell]: a maintainer script that logs its arguments to $DPKG_ROOT/scripts.log
script() {
  printf '#!/bin/sh\necho "%s %s $*" >> "$DPKG_ROOT/scripts.log"\n%s\nexit 0\n' "$(basename "$CUR" | sed 's/_.*//')" "$1" "${2:-}" > "$CUR/DEBIAN/$1"
  chmod 755 "$CUR/DEBIAN/$1"
}
# build [ctlcomp] [datacomp]: make $P/name_version_arch.deb from $CUR
build() {
  cc=${1:-gz} dc=${2:-xz}
  b=$CUR.b
  mkdir -p "$b"
  printf '2.0\n' > "$b/debian-binary"
  rm -f "$b/control.tar" "$b/control.tar.gz" "$b/control.tar.xz" "$b/control.tar.zst" "$b/data.tar" "$b/data.tar.gz" "$b/data.tar.xz" "$b/data.tar.zst"
  tar --format=gnu --owner=0 --group=0 --numeric-owner --mtime=@1700000000 -cf "$b/control.tar" -C "$CUR/DEBIAN" .
  tar --format=gnu --owner=0 --group=0 --mtime=@1700000000 --exclude=./DEBIAN -cf "$b/data.tar" -C "$CUR" .
  cz=control.tar dz=data.tar
  case $cc in gz) gzip -9n "$b/control.tar"; cz=control.tar.gz ;; xz) xz "$b/control.tar"; cz=control.tar.xz ;; zst) zstd -q --rm "$b/control.tar"; cz=control.tar.zst ;; esac
  case $dc in gz) gzip -9n "$b/data.tar"; dz=data.tar.gz ;; xz) xz "$b/data.tar"; dz=data.tar.xz ;; zst) zstd -q --rm "$b/data.tar"; dz=data.tar.zst ;; esac
  n=$(awk '$1=="Package:"{print $2}' "$CUR/DEBIAN/control")
  v=$(awk '$1=="Version:"{print $2}' "$CUR/DEBIAN/control")
  a=$(awk '$1=="Architecture:"{print $2}' "$CUR/DEBIAN/control")
  DEB=$P/${n}_${v}_$a.deb
  rm -f "$DEB"
  (cd "$b" && ar rcD "$DEB" debian-binary $cz $dz)
}

R=$W/root
CONF=$W/conf
mkdir -p "$CONF"
export UCOS_PKG_CONFDIR=$CONF
D() { "$BIN/dpkg" --root="$R" --force-script-chrootless "$@"; }
Q() { "$BIN/dpkg-query" --root="$R" "$@"; }
newroot() { R=$W/root-$1; mkdir -p "$R"; }

echo "unit tests"
check "unit tests" "$BIN/unit"
if command -v python3 >/dev/null 2>&1; then
  check "version comparison agrees with an independent oracle on 20000 random pairs" python3 -I "$top/libpkg/tests/version-oracle.py" "$BIN/unit" 20000
fi

echo "dpkg-deb"
pkg hello 1.0-1 all "Section: misc" "Priority: optional" "Installed-Size: 12"
file usr/bin/hello '#!/bin/sh
echo hello' 755
file usr/share/doc/hello/copyright 'public domain'
ln -s hello "$CUR/usr/bin/hi"
ln "$CUR/usr/share/doc/hello/copyright" "$CUR/usr/share/doc/hello/copyright.hard"
script postinst
script prerm
ctlfile md5sums "$(cd "$CUR" && md5sum usr/bin/hello usr/share/doc/hello/copyright)"
build gz xz
HELLO=$DEB
for combo in "none none" "gz gz" "xz zst" "zst gz"; do
  set -- $combo
  pkg hello-$1-$2 1 all
  file usr/share/hello-$1-$2/data "compressed with $1 and $2"
  build "$1" "$2"
  check "dpkg-deb -c reads control=$1 data=$2" sh -c "'$BIN/dpkg-deb' -c '$DEB' | grep -q './usr/share/hello-$1-$2/data'"
  check "dpkg-deb -f reads control=$1" sh -c "[ \"\$('$BIN/dpkg-deb' -f '$DEB' Package)\" = hello-$1-$2 ]"
done
check "dpkg-deb -I shows the header" sh -c "'$BIN/dpkg-deb' -I '$HELLO' | grep -q 'new Debian package, version 2.0.'"
check "dpkg-deb -I lists scripts as executable" sh -c "'$BIN/dpkg-deb' -I '$HELLO' | grep -q '\\*  postinst'"
check "dpkg-deb -I shows control fields" sh -c "'$BIN/dpkg-deb' -I '$HELLO' | grep -q '^ Package: hello'"
check "dpkg-deb -I FILE prints one control file" sh -c "'$BIN/dpkg-deb' -I '$HELLO' postinst | grep -q 'hello postinst'"
check "dpkg-deb -c shows a symlink" sh -c "'$BIN/dpkg-deb' -c '$HELLO' | grep -q 'lrwxrwxrwx root/root .* ./usr/bin/hi -> hello'"
check "dpkg-deb -c shows a hard link" sh -c "'$BIN/dpkg-deb' -c '$HELLO' | grep -q 'link to ./usr/share/doc/hello/copyright'"
check "dpkg-deb -c shows modes and sizes" sh -c "'$BIN/dpkg-deb' -c '$HELLO' | grep -q -- '-rwxr-xr-x root/root  *21 2023-11-14 22:13 ./usr/bin/hello'"
check "dpkg-deb -W" sh -c "[ \"\$('$BIN/dpkg-deb' -W '$HELLO')\" = \"\$(printf 'hello\\t1.0-1')\" ]"
check "dpkg-deb -f two fields" sh -c "'$BIN/dpkg-deb' -f '$HELLO' Package Version | grep -q '^Version: 1.0-1'"
check "dpkg-deb -x" "$BIN/dpkg-deb" -x "$HELLO" "$W/x"
check "  extracted file content" same "$W/x/usr/share/doc/hello/copyright" "public domain"
check "  extracted symlink" sh -c "[ \"\$(readlink '$W/x/usr/bin/hi')\" = hello ]"
check "  extracted executable mode" test -x "$W/x/usr/bin/hello"
check "dpkg-deb -e" "$BIN/dpkg-deb" -e "$HELLO" "$W/e"
check "  control file extracted" has "$W/e/control" "Package: hello"
check "  postinst executable" test -x "$W/e/postinst"
check "dpkg-deb --fsys-tarfile is a tar" sh -c "'$BIN/dpkg-deb' --fsys-tarfile '$HELLO' | tar -t | grep -q usr/bin/hello"
check "dpkg -I is dpkg-deb -I" sh -c "'$BIN/dpkg' -I '$HELLO' | grep -q 'Package: hello'"
check "dpkg -c is dpkg-deb -c" sh -c "'$BIN/dpkg' -c '$HELLO' | grep -q usr/bin/hello"
printf 'not a deb' > "$W/bad.deb"
checknot "a non-ar file is rejected" "$BIN/dpkg-deb" -I "$W/bad.deb"
cp "$HELLO" "$W/trunc.deb" && truncate -s 300 "$W/trunc.deb"
checknot "a truncated deb is rejected" "$BIN/dpkg-deb" -c "$W/trunc.deb"
b=$W/v3; mkdir -p "$b"; printf '3.0\n' > "$b/debian-binary"; (cd "$b" && ar rc ../v3.deb debian-binary)
checknot "format 3.0 is rejected" "$BIN/dpkg-deb" -I "$W/v3.deb"

echo "versions on the command line"
check "--compare-versions lt" "$BIN/dpkg" --compare-versions 1.0~rc1 lt 1.0
check "--compare-versions gt with epoch" "$BIN/dpkg" --compare-versions 1:0.1 gt 9.9
checknot "--compare-versions eq false" "$BIN/dpkg" --compare-versions 1.0 eq 1.0-1
check "--compare-versions empty is lowest" "$BIN/dpkg" --compare-versions "" lt 0
check "--compare-versions empty is highest with -nl" "$BIN/dpkg" --compare-versions "" gt-nl 1
check "--compare-versions <<" "$BIN/dpkg" --compare-versions 1 "<<" 2
checknot "--compare-versions bad syntax exits non-zero" "$BIN/dpkg" --compare-versions 1.0- lt 2

echo "architecture"
newroot arch
check "--print-architecture prints the configured value" sh -c "[ \"\$('$BIN/dpkg' --root='$R' --print-architecture)\" = amd64 ]"
printf 'native-architecture riscv64\n' > "$CONF/dpkg.cfg"
check "native-architecture in dpkg.cfg changes it" sh -c "[ \"\$('$BIN/dpkg' --root='$R' --print-architecture)\" = riscv64 ]"
: > "$CONF/dpkg.cfg"
check "--add-architecture" D --add-architecture arm64
check "--print-foreign-architectures" sh -c "[ \"\$('$BIN/dpkg' --root='$R' --print-foreign-architectures)\" = arm64 ]"
pkg foreign 1 i386
file usr/lib/foreign 'x'
build
checknot "a package of an unknown architecture is refused" D -i "$DEB"
check "... unless forced" D -i --force-architecture "$DEB"
check "--remove-architecture" D --remove-architecture arm64
checknot "dpkg refuses / on the host without --root" env -u DPKG_ROOT "$BIN/dpkg" -l
pkg nativepkg 1 amd64
file usr/lib/native 'x'
build
check "a native package installs" D -i "$DEB"
check "  status says installed" st "$R" nativepkg "install ok installed"

echo "install, query, remove, purge"
newroot basic
check "dpkg -i hello" D -i "$HELLO"
check "  status installed" st "$R" hello "install ok installed"
check "  file is there" same "$R/usr/share/doc/hello/copyright" "public domain"
check "  executable bit kept" test -x "$R/usr/bin/hello"
check "  symlink created" sh -c "[ \"\$(readlink '$R/usr/bin/hi')\" = hello ]"
check "  hard link created" same "$R/usr/share/doc/hello/copyright.hard" "public domain"
check "  postinst ran with configure and an empty version" has "$R/scripts.log" "^hello postinst configure $"
check "  info files" exists "$R/var/lib/dpkg/info/hello.list"
check "  md5sums stored" exists "$R/var/lib/dpkg/info/hello.md5sums"
check "  prerm stored" test -x "$R/var/lib/dpkg/info/hello.prerm"
check "  list has the root and directories" sh -c "head -1 '$R/var/lib/dpkg/info/hello.list' | grep -qx '/.' && grep -qx /usr/share/doc '$R/var/lib/dpkg/info/hello.list'"
check "  log written" has "$R/var/log/dpkg.log" "status installed hello"
check "  no .dpkg-new left behind" sh -c "! find '$R' -name '*.dpkg-new' | grep -q ."
check "dpkg -l" sh -c "'$BIN/dpkg' --root='$R' -l | grep -q '^ii  hello  *1.0-1  *all  *test package hello'"
check "dpkg -l with a pattern" sh -c "'$BIN/dpkg' --root='$R' -l 'hel*' | grep -q '^ii  hello'"
checknot "dpkg -l with no match" "$BIN/dpkg" --root="$R" -l nosuch
check "dpkg -L" sh -c "'$BIN/dpkg' --root='$R' -L hello | grep -qx /usr/bin/hello"
check "dpkg -s" sh -c "'$BIN/dpkg' --root='$R' -s hello | grep -q '^Status: install ok installed'"
check "dpkg -S with a path" sh -c "[ \"\$('$BIN/dpkg' --root='$R' -S /usr/bin/hello)\" = 'hello: /usr/bin/hello' ]"
check "dpkg -S with a substring" sh -c "'$BIN/dpkg' --root='$R' -S copyright | grep -q 'hello: /usr/share/doc/hello/copyright'"
check "dpkg-query -W" sh -c "[ \"\$('$BIN/dpkg-query' --root='$R' -W hello)\" = \"\$(printf 'hello\\t1.0-1')\" ]"
check "dpkg-query -W -f" sh -c "[ \"\$('$BIN/dpkg-query' --root='$R' -W -f '\${Package} \${db:Status-Abbrev} \${Installed-Size}\\n' hello)\" = 'hello ii  12' ]"
check "dpkg-query --showformat with width" sh -c "[ \"\$('$BIN/dpkg-query' --root='$R' -W --showformat='[\${Package;8}]' hello)\" = '[hello   ]' ]"
check "dpkg-query -c (control path)" sh -c "'$BIN/dpkg-query' --root='$R' -c hello | grep -q 'hello.postinst$'"
check "dpkg -V is clean" D -V hello
echo modified >> "$R/usr/bin/hello"
checknot "dpkg -V notices a changed file" D -V hello
check "dpkg -C is clean" D -C
check "dpkg --get-selections" sh -c "'$BIN/dpkg' --root='$R' --get-selections | grep -q '^hello  *install'"
check "dpkg -r hello" D -r hello
check "  prerm remove ran" has "$R/scripts.log" "^hello prerm remove$"
check "  files gone" missing "$R/usr/bin/hello"
check "  symlink gone" missing "$R/usr/bin/hi"
check "  status: no conffiles so not-installed" st "$R" hello "deinstall ok not-installed"
check "  directories gone" missing "$R/usr/share/doc/hello"
check "  info files gone" missing "$R/var/lib/dpkg/info/hello.list"
check "  status-old kept" exists "$R/var/lib/dpkg/status-old"
check "reinstall" D -i "$HELLO"
check "dpkg -P hello" D -P hello
checknot "  gone from status" grep -q "Package: hello" "$R/var/lib/dpkg/status"

echo "shared directories and file conflicts"
newroot shared
pkg aa 1 all; file usr/share/common/a 'from aa'; build; AA=$DEB
pkg bb 1 all; file usr/share/common/b 'from bb'; build; BB=$DEB
pkg cc 1 all; file usr/share/common/a 'from cc'; build; CC=$DEB
pkg cc 2 all "Replaces: aa (<< 2)"; file usr/share/common/a 'from cc 2'; build; CC2=$DEB
check "install aa and bb (shared directory)" D -i "$AA" "$BB"
check "remove aa" D -r aa
check "  shared directory stays" exists "$R/usr/share/common/b"
check "  aa's file gone" missing "$R/usr/share/common/a"
check "install aa again" D -i "$AA"
checknot "cc overwrites aa's file: refused" D -i "$CC"
check "  aa's file untouched" same "$R/usr/share/common/a" "from aa"
check "  cc not installed" sh -c "! grep -q 'Package: cc' '$R/var/lib/dpkg/status' || [ \"\$(awk '\$1==\"Package:\"{n=\$2} \$1==\"Status:\"&&n==\"cc\"{print \$4}' '$R/var/lib/dpkg/status')\" = not-installed ]"
check "  no stray .dpkg-new" missing "$R/usr/share/common/a.dpkg-new"
check "cc 2 with Replaces takes the file" D -i "$CC2"
check "  file is cc's now" same "$R/usr/share/common/a" "from cc 2"
check "  dpkg -S says cc" sh -c "[ \"\$('$BIN/dpkg' --root='$R' -S /usr/share/common/a)\" = 'cc: /usr/share/common/a' ]"
check "  aa disappeared (all its files replaced)" sh -c "! grep -q '^Package: aa$' '$R/var/lib/dpkg/status'"
check "--force-overwrite" D -i --force-overwrite "$AA"
check "  aa owns it again" same "$R/usr/share/common/a" "from aa"

echo "upgrades"
newroot upgrade
pkg up 1 all; file usr/share/up/keep 'v1'; file usr/share/up/old 'only in v1'; script preinst; script postinst; script prerm; script postrm; build; UP1=$DEB
pkg up 2 all; file usr/share/up/keep 'v2'; file usr/share/up/new 'only in v2'; script preinst; script postinst; script prerm; script postrm; build; UP2=$DEB
pkg up 0.5 all; file usr/share/up/keep 'v0.5'; build; UP05=$DEB
check "install up 1" D -i "$UP1"
: > "$R/scripts.log"
check "upgrade to up 2" D -i "$UP2"
check "  new content" same "$R/usr/share/up/keep" "v2"
check "  new file" exists "$R/usr/share/up/new"
check "  file dropped by v2 removed" missing "$R/usr/share/up/old"
check "  script order is Policy's" sh -c "[ \"\$(cat '$R/scripts.log')\" = \"\$(printf 'up prerm upgrade 2\\nup preinst upgrade 1 2\\nup postrm upgrade 2\\nup postinst configure 1')\" ]"
check "  version 2 in status" sh -c "'$BIN/dpkg' --root='$R' -s up | grep -q '^Version: 2$'"
check "  Config-Version cleared" sh -c "! grep -q Config-Version '$R/var/lib/dpkg/status'"
check "downgrade warns but works (dpkg default)" D -i "$UP05"
check "upgrade to 2 again" D -i "$UP2"
checknot "--refuse-downgrade" D -i --refuse-downgrade "$UP1"
check "  still version 2" sh -c "'$BIN/dpkg' --root='$R' -s up | grep -q '^Version: 2$'"
check "same version reinstall" D -i "$UP05"

echo "conffiles"
newroot conf
pkg cf 1 all; file etc/cf.conf 'setting=1'; file etc/other.conf 'other=1'; ctlfile conffiles '/etc/cf.conf
/etc/other.conf'; build; CF1=$DEB
pkg cf 2 all; file etc/cf.conf 'setting=2'; file etc/other.conf 'other=2'; ctlfile conffiles '/etc/cf.conf
/etc/other.conf'; build; CF2=$DEB
pkg cf 3 all; file etc/cf.conf 'setting=3'; file etc/other.conf 'other=2'; ctlfile conffiles '/etc/cf.conf
/etc/other.conf'; build; CF3=$DEB
pkg cf 4 all; file etc/cf.conf 'setting=4'; ctlfile conffiles '/etc/cf.conf'; build; CF4=$DEB
check "install cf 1" D -i "$CF1"
check "  Conffiles field has md5 hashes" sh -c "grep -A2 '^Conffiles:' '$R/var/lib/dpkg/status' | grep -q \"/etc/cf.conf \$(md5sum < '$R/etc/cf.conf' | cut -d' ' -f1)\""
echo 'setting=mine' > "$R/etc/cf.conf"
check "upgrade with a locally changed conffile" D -i "$CF2"
check "  user's version kept by default" same "$R/etc/cf.conf" "setting=mine"
check "  package's version saved as .dpkg-dist" same "$R/etc/cf.conf.dpkg-dist" "setting=2"
check "  unchanged conffile updated silently" same "$R/etc/other.conf" "other=2"
check "upgrade with --force-confnew" D -i --force-confnew "$CF3"
check "  package's version installed" same "$R/etc/cf.conf" "setting=3"
check "  user's version saved as .dpkg-old" same "$R/etc/cf.conf.dpkg-old" "setting=mine"
echo 'other=mine' > "$R/etc/other.conf"
check "upgrade where the package did not change a modified conffile" D -i "$CF3"
check "  modification kept, nothing new" sh -c "[ \"\$(cat '$R/etc/other.conf')\" = other=mine ] && [ ! -e '$R/etc/other.conf.dpkg-dist' ]"
check "upgrade to cf 4 which drops other.conf" D -i "$CF4"
check "  dropped conffile kept on disk" exists "$R/etc/other.conf"
check "  and marked obsolete" grep -q "/etc/other.conf .* obsolete" "$R/var/lib/dpkg/status"
rm -f "$R/etc/cf.conf"
check "a conffile the user deleted stays deleted on reinstall" D -i "$CF4"
check "  still deleted" missing "$R/etc/cf.conf"
check "--force-confmiss brings it back" D -i --force-confmiss "$CF4"
check "  back" same "$R/etc/cf.conf" "setting=4"
check "remove keeps conffiles" D -r cf
check "  config-files state" st "$R" cf "deinstall ok config-files"
check "  conffile still there" exists "$R/etc/cf.conf"
check "purge" D -P cf
check "  conffile gone" missing "$R/etc/cf.conf"
check "  backups gone" missing "$R/etc/cf.conf.dpkg-old"
checknot "  entry gone" grep -q "Package: cf" "$R/var/lib/dpkg/status"

echo "maintainer scripts"
newroot scripts
pkg sc 1 all; file usr/share/sc/f 'x'; script preinst; script postinst; script prerm; script postrm; build; SC1=$DEB
check "install" D -i "$SC1"
check "  preinst install" has "$R/scripts.log" "^sc preinst install$"
check "remove" D -r sc
check "  prerm remove, postrm remove" sh -c "grep -q '^sc prerm remove$' '$R/scripts.log' && grep -q '^sc postrm remove$' '$R/scripts.log'"
check "purge after remove" D -P sc
check "  postrm purge" has "$R/scripts.log" "^sc postrm purge$"
pkg envp 1 all; file usr/share/envp/f x
printf '#!/bin/sh\necho "$DPKG_MAINTSCRIPT_PACKAGE $DPKG_MAINTSCRIPT_NAME $DPKG_MAINTSCRIPT_ARCH" > "$DPKG_ROOT/env.txt"\n' > "$CUR/DEBIAN/postinst"; chmod 755 "$CUR/DEBIAN/postinst"
build
check "script environment" D -i "$DEB"
check "  DPKG_MAINTSCRIPT_* set" same "$R/env.txt" "envp postinst all"
newroot scripts2
checknot "without a way to run scripts, install stops" "$BIN/dpkg" --root="$R" -i "$SC1"
check "  preinst could not run: package not installed" sh -c "[ -z \"\$(status_of '$R' sc)\" ] || st '$R' sc 'unknown ok not-installed'"
pkg late 1 all; file usr/share/late/f x; script postinst; build; LATE=$DEB
checknot "postinst that cannot run leaves the package unpacked" "$BIN/dpkg" --root="$R" -i "$LATE"
check "  state is half-configured" st "$R" late "install ok half-configured"
check "--force-unrunnable-scripts finishes it" "$BIN/dpkg" --root="$R" --force-unrunnable-scripts --configure -a
check "  installed" st "$R" late "install ok installed"
check "  the skipped script is recorded" has "$R/var/lib/dpkg/info/late.unrun" "postinst configure"
newroot fail
pkg flaky 1 all; file usr/share/flaky/f x
printf '#!/bin/sh\n[ -e "$DPKG_ROOT/fail" ] && exit 1\nexit 0\n' > "$CUR/DEBIAN/postinst"; chmod 755 "$CUR/DEBIAN/postinst"
build; FLAKY=$DEB
touch "$R/fail"
checknot "a failing postinst" D -i "$FLAKY"
check "  leaves half-configured" st "$R" flaky "install ok half-configured"
check "  dpkg -C reports it" sh -c "'$BIN/dpkg' --root='$R' -C | grep -q flaky"
rm -f "$R/fail"
check "dpkg --configure -a repairs it" D --configure -a
check "  installed" st "$R" flaky "install ok installed"
pkg badpre 1 all; file usr/share/badpre/f x; printf '#!/bin/sh\nexit 1\n' > "$CUR/DEBIAN/preinst"; chmod 755 "$CUR/DEBIAN/preinst"; build
checknot "a failing preinst aborts the install" D -i "$DEB"
check "  nothing unpacked" missing "$R/usr/share/badpre/f"

echo "dependencies"
newroot deps
pkg libfoo 1 all "Provides: foo-virtual (= 1.5)"; file usr/lib/libfoo 'lib'; build; LIBFOO=$DEB
pkg app 1 all "Depends: libfoo (>= 1)"; file usr/bin/app 'app'; build; APP=$DEB
pkg vapp 1 all "Depends: foo-virtual (>= 1.2)"; file usr/bin/vapp 'vapp'; build; VAPP=$DEB
pkg alt 1 all "Depends: nosuch | libfoo"; file usr/bin/alt 'alt'; build; ALT=$DEB
pkg pre 1 all "Pre-Depends: libfoo"; file usr/bin/pre 'pre'; build; PRE=$DEB
pkg ess 1 all "Essential: yes"; file usr/bin/ess 'ess'; build; ESS=$DEB
checknot "Pre-Depends missing: refused" D -i "$PRE"
check "  not unpacked" missing "$R/usr/bin/pre"
checknot "Depends missing: unpacked, not configured" D -i "$APP"
check "  state unpacked" st "$R" app "install ok unpacked"
check "install the dependency, then --configure -a" D -i "$LIBFOO"
check "  configure -a" D --configure -a
check "  app installed" st "$R" app "install ok installed"
check "versioned Provides satisfies a versioned dependency" D -i "$VAPP"
check "alternatives" D -i "$ALT"
check "Pre-Depends satisfied now" D -i "$PRE"
checknot "removing a needed package is refused" D -r libfoo
check "  still installed" st "$R" libfoo "install ok installed"
check "--force-depends allows it" D -r --force-depends libfoo
check "both packages in one dpkg -i (order does not matter)" sh -c "newr='$W/root-deps2'; mkdir -p \$newr; '$BIN/dpkg' --root=\$newr --force-script-chrootless -i '$APP' '$LIBFOO' && awk '\$1==\"Package:\"{n=\$2} \$1==\"Status:\"&&n==\"app\"{print \$4}' \$newr/var/lib/dpkg/status | grep -qx installed"
check "install an essential package" D -i "$ESS"
checknot "removing it is refused" D -r ess
check "--force-remove-essential" D -r --force-remove-essential ess
pkg cyc1 1 all "Depends: cyc2"; file usr/share/cyc1 x; build; CYC1=$DEB
pkg cyc2 1 all "Depends: cyc1"; file usr/share/cyc2 x; build; CYC2=$DEB
check "a dependency cycle installs in one run" D -i "$CYC1" "$CYC2"
check "  cyc1 installed" st "$R" cyc1 "install ok installed"
check "  cyc2 installed" st "$R" cyc2 "install ok installed"

echo "conflicts"
newroot conflicts
pkg old 1 all; file usr/bin/tool 'old'; build; OLD=$DEB
pkg new 1 all "Conflicts: old"; file usr/bin/newtool 'new'; build; NEW=$DEB
pkg newer 1 all "Conflicts: old" "Replaces: old"; file usr/bin/tool 'newer'; build; NEWER=$DEB
pkg brk 1 all "Breaks: old (<< 2)"; file usr/bin/brk x; build; BRK=$DEB
check "install old" D -i "$OLD"
checknot "Conflicts refuses" D -i "$NEW"
checknot "Breaks refuses" D -i "$BRK"
check "--force-conflicts" D -i --force-conflicts "$NEW"
check "remove new" D -r new
check "Conflicts + Replaces removes the old package" D -i "$NEWER"
check "  old removed" st "$R" old "deinstall ok not-installed"
check "  the file belongs to newer" same "$R/usr/bin/tool" "newer"

echo "triggers"
newroot triggers
pkg watcher 1 all; file usr/share/watcher/x x; ctlfile triggers 'interest /usr/share/watched
interest-noawait my-trigger'; script postinst; build; WATCH=$DEB
pkg filer 1 all; file usr/share/watched/thing 'x'; build; FILER=$DEB
pkg activator 1 all; file usr/share/act/x x; ctlfile triggers 'activate-noawait my-trigger'; build; ACT=$DEB
check "install watcher" D -i "$WATCH"
check "  interest registered" has "$R/var/lib/dpkg/triggers/File" "/usr/share/watched watcher"
: > "$R/scripts.log"
check "installing a file under the watched path" D -i "$FILER"
check "  runs postinst triggered" has "$R/scripts.log" "^watcher postinst triggered /usr/share/watched$"
check "  watcher installed again" st "$R" watcher "install ok installed"
: > "$R/scripts.log"
check "an explicit trigger" D -i "$ACT"
check "  runs postinst triggered my-trigger" has "$R/scripts.log" "^watcher postinst triggered my-trigger$"

echo "configuration"
newroot cfg
mkdir -p "$CONF/dpkg.cfg.d"
printf '# docs are not installed\npath-exclude /usr/share/doc/*\npath-include /usr/share/doc/*/copyright\n' > "$CONF/dpkg.cfg.d/excludes"
pkg docs 1 all; file usr/share/doc/docs/README 'readme'; file usr/share/doc/docs/copyright 'c'; file usr/bin/docs 'x'; build
check "install with path-exclude" D -i "$DEB"
check "  excluded file not installed" missing "$R/usr/share/doc/docs/README"
check "  included again by path-include" exists "$R/usr/share/doc/docs/copyright"
mv "$CONF/dpkg.cfg.d/excludes" "$W/excludes.off"
printf 'root %s\n' "$W/root-fromcfg" > "$CONF/dpkg.cfg"
check "root from dpkg.cfg" sh -c "'$BIN/dpkg' --force-script-chrootless -i '$HELLO' && [ -e '$W/root-fromcfg/usr/bin/hello' ]"
: > "$CONF/dpkg.cfg"
check "DPKG_ROOT from the environment" sh -c "DPKG_ROOT='$W/root-fromenv' '$BIN/dpkg' --force-script-chrootless -i '$HELLO' && [ -e '$W/root-fromenv/usr/bin/hello' ]"
check "--admindir separate from --root" sh -c "'$BIN/dpkg' --root='$W/root-ad' --admindir='$W/admin-ad' --force-script-chrootless -i '$HELLO' && [ -e '$W/admin-ad/status' ] && [ -e '$W/root-ad/usr/bin/hello' ]"
check "--no-act changes nothing" sh -c "'$BIN/dpkg' --root='$W/root-noact' --no-act -i '$HELLO'; [ ! -e '$W/root-noact/usr/bin/hello' ]"

echo "interrupted install"
newroot crash
check "install up 1" D -i "$UP1"
# simulate a crash in the middle of an unpack: half-installed with reinstreq
sed -i 's/^Status: install ok installed$/Status: install reinstreq half-installed/' "$R/var/lib/dpkg/status"
check "dpkg -C reports the broken package" sh -c "'$BIN/dpkg' --root='$R' -C | grep -q 'needs to be reinstalled'"
checknot "removing it is refused without force" D -r up
check "reinstalling repairs it" D -i "$UP1"
check "  installed" st "$R" up "install ok installed"

if [ -f "$top/libpkg/tests/apt.sh" ]; then . "$top/libpkg/tests/apt.sh"; fi

echo
echo "pkg-tests: $((pass + fail)) checks, $pass passed, $fail failed (log: $LOG)"
[ "$fail" -eq 0 ]
