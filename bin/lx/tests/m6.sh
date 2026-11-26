#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# M6 (natively): a minimal Debian root, set up by hand, whose own dpkg installs packages
# and runs their maintainer scripts with the root's /bin/sh, all inside the engine.
#   LX=/path/to/lx LX_SCRATCH=/dir sh m6.sh
# Uses $LX_SCRATCH/root (made by fetch-debian.sh) and the .deb files in $LX_SCRATCH/debs.
# The guest runs as uid 0 (LX_UID=0), like a fakeroot: chown is accepted and not done.
set -u
R=$LX_SCRATCH/root
mkdir -p "$R/var/lib/dpkg/info" "$R/var/lib/dpkg/updates" "$R/var/lib/dpkg/triggers" "$R/var/log" "$R/tmp/debs"
[ -f "$R/var/lib/dpkg/status" ] || : > "$R/var/lib/dpkg/status"
[ -f "$R/var/lib/dpkg/available" ] || : > "$R/var/lib/dpkg/available"
order="libgcc-s1 libc6 libdebconfclient0 base-passwd base-files debianutils bc"
for p in $order; do cp "$LX_SCRATCH"/debs/${p}_*.deb "$R/tmp/debs/"; done
run() { env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LX_UID=0 timeout 120 "$LX" --root="$R" "$@"; }
fail=0
for p in $order; do
  f=$(cd "$R/tmp/debs" && ls ${p}_*.deb | head -1)
  if run /usr/bin/dpkg --force-depends -i "/tmp/debs/$f" > "$LX_SCRATCH/m6-$p.log" 2>&1; then echo "ok    dpkg -i $p"; else echo "FAIL  dpkg -i $p (see $LX_SCRATCH/m6-$p.log)"; fail=$((fail+1)); fi
done
n=$(run /usr/bin/dpkg-query -W -f='${db:Status-Abbrev}\n' 2>/dev/null | grep -c '^ii')
echo "packages in state ii: $n"
[ -s "$R/etc/passwd" ] && grep -q '^root:' "$R/etc/passwd" && echo "ok    base-passwd postinst wrote /etc/passwd" || { echo "FAIL  /etc/passwd"; fail=$((fail+1)); }
grep -q '^/bin/sh' "$R/etc/shells" 2>/dev/null && echo "ok    debianutils postinst wrote /etc/shells" || { echo "FAIL  /etc/shells"; fail=$((fail+1)); }
[ "$(echo '2^10' | run /usr/bin/bc)" = 1024 ] && echo "ok    installed bc runs" || { echo "FAIL  bc"; fail=$((fail+1)); }
echo "m6: $fail failed"
[ $fail = 0 ]
