# SPDX-License-Identifier: GPL-2.0-or-later
# Package tools inside UC.OS. Staged into the image by libpkg/tests/os-test.sh.
# Every check prints "ok" or "FAIL"; the last line is the count.
pass=0
fail=0
check() { # check NAME EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "ok    $1"
  else fail=$((fail + 1)); echo "FAIL  $1"; echo "      expected: [$2]"; echo "      got:      [$3]"; fi
}
yes_if() { if "$@" >/dev/null 2>&1; then echo yes; else echo no; fi }
S=/pkgtest/src
R=/pkgtest/root
D="dpkg --root=$R --force-script-chrootless"
mkdir -p $R

echo "--- dpkg-deb"
check "dpkg-deb -f" osa "$(dpkg-deb -f $S/osa_1_all.deb Package)"
check "dpkg-deb -W" "osb	1" "$(dpkg-deb -W $S/osb_1_all.deb)"
check "dpkg-deb -c lists a file" yes "$(yes_if sh -c "dpkg-deb -c $S/osa_1_all.deb | grep -q usr/share/osa/file")"
check "dpkg-deb -I header" yes "$(yes_if sh -c "dpkg-deb -I $S/osa_1_all.deb | grep -q 'new Debian package, version 2.0.'")"
dpkg-deb -x $S/osb_1_all.deb /pkgtest/x
check "dpkg-deb -x content" "osb 1" "$(cat /pkgtest/x/usr/share/osb/file)"
check "compare-versions" yes "$(yes_if dpkg --compare-versions 1.0~rc1 lt 1.0)"
check "print-architecture" amd64 "$(dpkg --root=$R --print-architecture)"

echo "--- dpkg"
check "dpkg -i osb" 0 "$($D -i $S/osb_1_all.deb >/dev/null 2>&1; echo $?)"
check "  file installed" "osb 1" "$(cat $R/usr/share/osb/file)"
check "  postinst ran in this system's shell" "osb postinst configure " "$(cat $R/scripts.log)"
check "  status" installed "$(dpkg-query --root=$R -W -f '${db:Status-Status}' osb)"
check "dpkg -i osc (shared directory)" 0 "$($D -i $S/osc_1_all.deb >/dev/null 2>&1; echo $?)"
check "dpkg -L" yes "$(yes_if sh -c "dpkg --root=$R -L osb | grep -q /usr/share/osb/file")"
check "dpkg -S" "osb: /usr/share/osb/file" "$(dpkg --root=$R -S /usr/share/osb/file)"
check "dpkg -l" yes "$(yes_if sh -c "dpkg --root=$R -l | grep -q '^ii  osb'")"
check "dpkg -r osc" 0 "$($D -r osc >/dev/null 2>&1; echo $?)"
check "  shared directory kept" "osb 1" "$(cat $R/usr/share/common/b)"
check "  osc's file gone" no "$(yes_if cat $R/usr/share/common/c)"
check "dpkg -i osa 1, then upgrade to 2" 0 "$($D -i $S/osa_1_all.deb >/dev/null 2>&1; $D -i $S/osa_2_all.deb >/dev/null 2>&1; echo $?)"
check "  new file" "osa 2" "$(cat $R/usr/share/osa/new)"
check "  version" 2 "$(dpkg-query --root=$R -W -f '${Version}' osa)"
check "dpkg -P osa" 0 "$($D -P osa >/dev/null 2>&1; echo $?)"
check "  gone" no "$(yes_if cat $R/usr/share/osa/file)"
check "dpkg -r osb" 0 "$($D -r osb >/dev/null 2>&1; echo $?)"
check "without chrootless, scripts cannot run in another root" 1 "$(dpkg --root=$R -i $S/osb_1_all.deb >/dev/null 2>&1; echo $?)"
check "  so osb is half-configured" half-configured "$(dpkg-query --root=$R -W -f '${db:Status-Status}' osb)"
check "--force-unrunnable-scripts finishes it" 0 "$(dpkg --root=$R --force-unrunnable-scripts --configure -a >/dev/null 2>&1; echo $?)"

echo "--- apt"
$D -P osb >/dev/null 2>&1
A="apt -o Dir=$R -o DPkg::Options::=--force-script-chrootless"
mkdir -p $R/etc/apt
echo "deb file:/pkgtest/repo test main" > $R/etc/apt/sources.list
check "apt update" 0 "$($A update >/dev/null 2>&1; echo $?)"
check "  index stored" yes "$(yes_if cat $R/var/lib/apt/lists/_pkgtest_repo_dists_test_main_binary-amd64_Packages)"
check "  signature warning" yes "$(yes_if sh -c "$A update 2>&1 | grep -q 'NOT checked'")"
check "apt-cache policy" yes "$(yes_if sh -c "apt-cache -o Dir=$R policy osa | grep -q 'Candidate: 1'")"
check "apt install -y osa" 0 "$($A install -y osa >/dev/null 2>&1; echo $?)"
check "  osa installed" installed "$(dpkg-query --root=$R -W -f '${db:Status-Status}' osa)"
check "  osb (dependency) installed" installed "$(dpkg-query --root=$R -W -f '${db:Status-Status}' osb)"
check "apt remove -y osa" 0 "$($A remove -y osa >/dev/null 2>&1; echo $?)"
check "apt autoremove -y removes osb" 0 "$($A autoremove -y >/dev/null 2>&1; echo $?)"
check "  osb removed" "deinstall not-installed" "$(dpkg-query --root=$R -W -f '${db:Status-Want} ${db:Status-Status}' osb)"
echo "pkg-os-tests: $((pass + fail)) checks, $pass passed, $fail failed"
